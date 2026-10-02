#include "live.h"

#include <string.h>

#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

#include "http_srv.h"
#include "ld2410_task.h"
#include "snapshot_json.h"
#include "ws_slots.h"

#define LIVE_PERIOD_MS 100 /* 10 Hz */
/* Longest snapshot JSON is about 330 bytes (engineering frame); keep wide margin. */
#define LIVE_JSON_MAX 768
#define LIVE_RX_MAX 128 /* client frames are ignored; bigger ones close the socket */

static const char *TAG = "live";

extern const char web_page_start[] asm("_binary_web_page_html_start");

static SemaphoreHandle_t s_lock;
static ws_slots_t s_slots;                 /* guarded by s_lock */
static char s_buf[WS_SLOTS_MAX][LIVE_JSON_MAX]; /* slot idx buffer: written only while not in flight */
static size_t s_len[WS_SLOTS_MAX];
static char s_tmp[LIVE_JSON_MAX];          /* broadcaster task only */
static TaskHandle_t s_task;

static void on_close(int fd)
{
    if (s_lock == NULL) return;
    xSemaphoreTake(s_lock, portMAX_DELAY);
    ws_slots_remove(&s_slots, fd);
    xSemaphoreGive(s_lock);
}

void live_prepare(void)
{
    if (s_lock == NULL) {
        s_lock = xSemaphoreCreateMutex();
        ws_slots_init(&s_slots);
    }
    http_srv_set_close_hook(on_close);
}

static esp_err_t page_get(httpd_req_t *req)
{
    httpd_resp_set_type(req, "text/html; charset=utf-8");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    return httpd_resp_send(req, web_page_start, HTTPD_RESP_USE_STRLEN);
}

static esp_err_t ws_handler(httpd_req_t *req)
{
    if (req->method == HTTP_GET) { /* handshake done: register the client */
        int fd = httpd_req_to_sockfd(req);
        xSemaphoreTake(s_lock, portMAX_DELAY);
        int rc = ws_slots_add(&s_slots, fd);
        xSemaphoreGive(s_lock);
        if (rc < 0) {
            ESP_LOGW(TAG, "no free WebSocket slot (fd %d)", fd);
            return ESP_FAIL; /* closes the connection */
        }
        return ESP_OK;
    }
    /* Incoming frames carry nothing we need; read and drop them. */
    httpd_ws_frame_t f;
    memset(&f, 0, sizeof f);
    esp_err_t err = httpd_ws_recv_frame(req, &f, 0);
    if (err != ESP_OK) return err;
    if (f.type == HTTPD_WS_TYPE_CLOSE) return ESP_OK; /* server answers; close_fn removes the slot */
    if (f.len > 0) {
        if (f.len > LIVE_RX_MAX) return ESP_FAIL;
        uint8_t rx[LIVE_RX_MAX];
        f.payload = rx;
        err = httpd_ws_recv_frame(req, &f, f.len);
        if (err != ESP_OK) return err;
    }
    return ESP_OK;
}

/* Runs in the httpd task. */
static void send_work(void *arg)
{
    int idx = (int)(intptr_t)arg;
    xSemaphoreTake(s_lock, portMAX_DELAY);
    int fd = ws_slots_fd(&s_slots, idx);
    xSemaphoreGive(s_lock);

    esp_err_t err = ESP_OK;
    if (fd >= 0) {
        httpd_ws_frame_t f;
        memset(&f, 0, sizeof f);
        f.type = HTTPD_WS_TYPE_TEXT;
        f.final = true;
        f.payload = (uint8_t *)s_buf[idx];
        f.len = s_len[idx];
        err = httpd_ws_send_frame_async(http_srv_handle(), fd, &f);
    }
    xSemaphoreTake(s_lock, portMAX_DELAY);
    ws_slots_end(&s_slots, idx);
    xSemaphoreGive(s_lock);
    if (err != ESP_OK && fd >= 0) {
        ESP_LOGD(TAG, "ws send failed (fd %d): %s", fd, esp_err_to_name(err));
        httpd_sess_trigger_close(http_srv_handle(), fd); /* close_fn drops the slot */
    }
}

static void tick(void)
{
    xSemaphoreTake(s_lock, portMAX_DELAY);
    int clients = ws_slots_count(&s_slots);
    xSemaphoreGive(s_lock);
    if (clients == 0) return;

    ld_snapshot_t ls;
    snapshot_t s;
    memset(&s, 0, sizeof s);
    if (ld2410_get_snapshot(&ls) && ls.valid) {
        s.have_frame = 1;
        s.data = ls.data;
        s.seq = ls.seq;
        s.esp_time_us = ls.time_us;
    }
    switch (ld2410_link_status()) {
    case LD_LINK_OK: s.link = SNAP_LINK_OK; break;
    case LD_LINK_LOST: s.link = SNAP_LINK_LOST; break;
    default: s.link = SNAP_LINK_NO_DATA; break;
    }
    int n = snapshot_json(s_tmp, sizeof s_tmp, &s);
    if (n <= 0) return;

    int todo[WS_SLOTS_MAX], cnt = 0;
    xSemaphoreTake(s_lock, portMAX_DELAY);
    for (int i = 0; i < WS_SLOTS_MAX; i++) {
        if (ws_slots_begin(&s_slots, i)) { /* skips clients whose last send is pending */
            memcpy(s_buf[i], s_tmp, (size_t)n + 1);
            s_len[i] = (size_t)n;
            todo[cnt++] = i;
        }
    }
    xSemaphoreGive(s_lock);

    for (int k = 0; k < cnt; k++) {
        if (httpd_queue_work(http_srv_handle(), send_work, (void *)(intptr_t)todo[k]) != ESP_OK) {
            xSemaphoreTake(s_lock, portMAX_DELAY);
            ws_slots_end(&s_slots, todo[k]);
            xSemaphoreGive(s_lock);
        }
    }
}

/* Dedicated task (not the shared esp_timer task): it takes mutexes and queues
 * work to httpd. Latest-only: a slot with a send still pending is skipped. */
static void live_task(void *arg)
{
    (void)arg;
    TickType_t last = xTaskGetTickCount();
    for (;;) {
        vTaskDelayUntil(&last, pdMS_TO_TICKS(LIVE_PERIOD_MS));
        tick();
    }
}

esp_err_t live_start(void)
{
    if (s_lock == NULL || http_srv_handle() == NULL || s_task != NULL) return ESP_ERR_INVALID_STATE;
    static const httpd_uri_t get_page = {.uri = "/", .method = HTTP_GET, .handler = page_get};
    static const httpd_uri_t get_ws = {.uri = "/ws", .method = HTTP_GET, .handler = ws_handler,
                                       .is_websocket = true};
    esp_err_t err = http_srv_register(&get_page);
    if (err == ESP_OK) err = http_srv_register(&get_ws);
    if (err != ESP_OK) return err;
    BaseType_t ok = xTaskCreate(live_task, "live_10hz", 4096, NULL, 4, &s_task);
    return ok == pdPASS ? ESP_OK : ESP_ERR_NO_MEM;
}
