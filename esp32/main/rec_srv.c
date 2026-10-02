/* Recording server. Two tasks on core 0:
 *  - accept task: accepts connections; a new one shutdown()s the client being
 *    served (so a blocked recv/send fails at once) and hands the new socket
 *    to the serve task through a one-slot queue;
 *  - serve task: owns the sockets (it alone close()s them), reads the 12-byte
 *    request, then sends a batch about every second.
 * The ring lock is taken only inside ld2410_ring_batch() while copying; every
 * send() happens without it, with SO_SNDTIMEO so a stalled client cannot hold
 * anything for long. */
#include "rec_srv.h"

#include <errno.h>
#include <stdbool.h>
#include <string.h>

#include "esp_log.h"
#include "esp_random.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "lwip/sockets.h"

#include "ld2410_task.h"
#include "rec_proto.h"

#define REQ_TIMEOUT_S 5
#define SEND_TIMEOUT_S 5
#define BATCH_PERIOD_MS 1000
#define BATCH_BUF_BYTES (16u * 1024u)

static const char *TAG = "rec_srv";

static uint32_t s_boot_id;
static QueueHandle_t s_new_fd_q;   /* one slot: accepted socket waiting to be served */
static SemaphoreHandle_t s_cur_mtx; /* guards s_cur_fd */
static int s_cur_fd = -1;           /* socket being served, -1 if none */
static TaskHandle_t s_serve_task;
static uint8_t s_batch[BATCH_BUF_BYTES];

uint32_t rec_srv_boot_id(void)
{
    return s_boot_id;
}

static bool preempted(void)
{
    return uxQueueMessagesWaiting(s_new_fd_q) > 0;
}

static void set_timeout(int fd, int opt, int seconds)
{
    struct timeval tv = {.tv_sec = seconds, .tv_usec = 0};
    setsockopt(fd, SOL_SOCKET, opt, &tv, sizeof tv);
}

/* Read exactly len bytes (each recv bounded by SO_RCVTIMEO). */
static bool recv_exact(int fd, uint8_t *buf, size_t len)
{
    size_t got = 0;
    while (got < len) {
        int n = recv(fd, buf + got, len - got, 0);
        if (n <= 0 || preempted())
            return false;
        got += (size_t)n;
    }
    return true;
}

/* Send everything, handling partial sends; false on error or timeout. */
static bool send_all(int fd, const uint8_t *buf, size_t len)
{
    size_t sent = 0;
    while (sent < len) {
        int n = send(fd, buf + sent, len - sent, 0);
        if (n <= 0)
            return false;
        sent += (size_t)n;
    }
    return true;
}

static void serve(int fd)
{
    uint8_t req[REC_REQ_LEN];
    uint32_t seq = 0;
    rec_stream_t stream; /* cursor kept across batches, see rec_stream_batch() */
    rec_stream_init(&stream);

    set_timeout(fd, SO_RCVTIMEO, REQ_TIMEOUT_S);
    set_timeout(fd, SO_SNDTIMEO, SEND_TIMEOUT_S);
    if (!recv_exact(fd, req, sizeof req)) {
        ESP_LOGW(TAG, "no request");
        return;
    }
    int rc = rec_req_parse(req, sizeof req, &seq);
    if (rc != 0) {
        ESP_LOGW(TAG, "bad request (%d)", rc);
        return;
    }
    ESP_LOGI(TAG, "client from_seq=%u boot_id=%08x", (unsigned)seq, (unsigned)s_boot_id);

    while (!preempted()) {
        size_t len = 0;
        uint16_t count = 0;
        rc = ld2410_ring_batch(&stream, seq, s_boot_id, s_batch, sizeof s_batch, &len, &count);
        if (rc != 0) {
            ESP_LOGE(TAG, "batch build failed (%d)", rc);
            return;
        }
        if (!send_all(fd, s_batch, len)) {
            ESP_LOGW(TAG, "send failed (errno %d)", errno);
            return;
        }
        /* Only the first call uses seq; later batches continue the cursor
         * (and reopen it with GAP if its record was evicted). */
        if (rec_batch_next_seq(s_batch, len, &seq) != 0)
            return;
        if (count > 0 && rec_batch_maybe_truncated(len, sizeof s_batch))
            continue; /* more pending: next batch at once */
        /* Idle or caught up: wait a second; a new client wakes us early. */
        uint32_t note;
        xTaskNotifyWait(0, UINT32_MAX, &note, pdMS_TO_TICKS(BATCH_PERIOD_MS));
    }
}

static void serve_task(void *arg)
{
    (void)arg;
    for (;;) {
        int fd;
        if (xQueueReceive(s_new_fd_q, &fd, portMAX_DELAY) != pdTRUE)
            continue;
        /* Taking ownership and the "was I already replaced?" check are one
         * critical section; the accept task queues new sockets under the same
         * mutex. Either it ran before (the queue is non-empty here and we
         * shutdown() our own fd, so serve() fails at once) or after (it sees
         * s_cur_fd and shutdown()s it). A new client never waits for a timeout. */
        xSemaphoreTake(s_cur_mtx, portMAX_DELAY);
        s_cur_fd = fd;
        if (preempted())
            shutdown(fd, SHUT_RDWR);
        xSemaphoreGive(s_cur_mtx);

        serve(fd);

        /* close under the mutex: the accept task never shutdown()s a recycled fd */
        xSemaphoreTake(s_cur_mtx, portMAX_DELAY);
        s_cur_fd = -1;
        close(fd);
        xSemaphoreGive(s_cur_mtx);
        ESP_LOGI(TAG, "client closed");
    }
}

static void accept_task(void *arg)
{
    (void)arg;
    for (;;) {
        int lfd = socket(AF_INET, SOCK_STREAM, IPPROTO_IP);
        if (lfd < 0) {
            ESP_LOGE(TAG, "socket failed (errno %d), retry", errno);
            vTaskDelay(pdMS_TO_TICKS(2000));
            continue;
        }
        int one = 1;
        setsockopt(lfd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof one);
        struct sockaddr_in addr;
        memset(&addr, 0, sizeof addr);
        addr.sin_family = AF_INET;
        addr.sin_addr.s_addr = htonl(INADDR_ANY);
        addr.sin_port = htons(REC_SRV_PORT);
        if (bind(lfd, (struct sockaddr *)&addr, sizeof addr) != 0 || listen(lfd, 2) != 0) {
            ESP_LOGE(TAG, "bind/listen failed (errno %d), retry", errno);
            close(lfd);
            vTaskDelay(pdMS_TO_TICKS(2000));
            continue;
        }
        ESP_LOGI(TAG, "listening on port %d, boot_id=%08x", REC_SRV_PORT, (unsigned)s_boot_id);

        for (;;) {
            int fd = accept(lfd, NULL, NULL);
            if (fd < 0) {
                ESP_LOGW(TAG, "accept failed (errno %d)", errno);
                break; /* recreate the listening socket */
            }
            /* A new connection replaces the current one. */
            xSemaphoreTake(s_cur_mtx, portMAX_DELAY);
            int stale;
            while (xQueueReceive(s_new_fd_q, &stale, 0) == pdTRUE)
                close(stale); /* accepted but never served */
            if (s_cur_fd >= 0)
                shutdown(s_cur_fd, SHUT_RDWR);
            xQueueSend(s_new_fd_q, &fd, 0); /* queue is empty: cannot fail */
            xSemaphoreGive(s_cur_mtx);
            xTaskNotifyGive(s_serve_task);  /* cut the 1 s wait short */
        }
        close(lfd);
        vTaskDelay(pdMS_TO_TICKS(1000));
    }
}

esp_err_t rec_srv_start(void)
{
    do {
        s_boot_id = esp_random();
    } while (s_boot_id == 0);

    s_new_fd_q = xQueueCreate(1, sizeof(int));
    s_cur_mtx = xSemaphoreCreateMutex();
    if (s_new_fd_q == NULL || s_cur_mtx == NULL)
        return ESP_ERR_NO_MEM;
    if (xTaskCreatePinnedToCore(serve_task, "rec_serve", 4096, NULL, 4, &s_serve_task, 0) != pdPASS)
        return ESP_ERR_NO_MEM;
    if (xTaskCreatePinnedToCore(accept_task, "rec_accept", 4096, NULL, 4, NULL, 0) != pdPASS)
        return ESP_ERR_NO_MEM;
    return ESP_OK;
}
