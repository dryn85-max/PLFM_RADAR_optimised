#include "live.h"

#include <errno.h>
#include <stdio.h>
#include <string.h>

#include "esp_idf_version.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

#include "esp_timer.h"
#include "lwip/sockets.h"

#include "gps_task.h"
#include "http_srv.h"
#include "imu_task.h"
#include "ld2410_task.h"
#include "snapshot_json.h"
#include "sntp_sync.h"
#include "ws_slots.h"

#define LIVE_PERIOD_MS 100 /* 10 Hz */
/* Worst-case snapshot JSON is bounded by SNAPSHOT_JSON_MAX (asserted by a host test). */
#define LIVE_JSON_MAX SNAPSHOT_JSON_MAX
#define LIVE_IMU_STALE_US 1000000 /* no IMU record for this long: report "error" */
#define LIVE_RX_MAX 128 /* client frames are ignored; bigger ones close the socket */
#define LIVE_STATS_TICKS 100 /* one stats line every 100 ticks = 10 s */
/* A stalled browser blocks the httpd task for the send timeout on every frame; the
 * httpd default is 5 s, so cap it at 1 s on WebSocket fds (the send-failure path closes). */
#define LIVE_WS_SEND_TIMEOUT_S 1

/* ESP-IDF v5.5.5 (and master) no longer call the URI handler for the WebSocket
 * handshake GET ("If the request is websocket handshake, then do not call the
 * uri->handler", httpd_uri.c); a new client is only reported through
 * ws_post_handshake_cb, which exists only with this option. v5.5.4 and older
 * (and v6.0) still call the handler with method HTTP_GET. A stale sdkconfig
 * keeps the option off even though sdkconfig.defaults enables it: delete
 * esp32/sdkconfig and rebuild. */
#if !defined(CONFIG_HTTPD_WS_POST_HANDSHAKE_CB_SUPPORT) &&                                   \
    ((ESP_IDF_VERSION >= ESP_IDF_VERSION_VAL(5, 5, 5) &&                                     \
      ESP_IDF_VERSION < ESP_IDF_VERSION_VAL(6, 0, 0)) ||                                     \
     ESP_IDF_VERSION >= ESP_IDF_VERSION_VAL(6, 1, 0))
#error "CONFIG_HTTPD_WS_POST_HANDSHAKE_CB_SUPPORT=y is required (delete esp32/sdkconfig and rebuild)"
#endif

static const char *TAG = "live";

extern const char web_page_start[] asm("_binary_web_page_html_start");

static SemaphoreHandle_t s_lock;
static ws_slots_t s_slots;                 /* guarded by s_lock */
static char s_buf[WS_SLOTS_MAX][LIVE_JSON_MAX]; /* slot idx buffer: written only while not in flight */
static size_t s_len[WS_SLOTS_MAX];
static char s_tmp[LIVE_JSON_MAX];          /* broadcaster task only */
static TaskHandle_t s_task;
/* Diagnostics for the current 10 s window, guarded by s_lock. */
static unsigned s_sent, s_skipped_inflight, s_queue_fail;

static void on_close(int fd)
{
    if (s_lock == NULL) return;
    xSemaphoreTake(s_lock, portMAX_DELAY);
    int rc = ws_slots_remove(&s_slots, fd);
    xSemaphoreGive(s_lock);
    if (rc == 0) ESP_LOGI(TAG, "ws client removed fd=%d", fd); /* plain HTTP sockets: silent */
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

/* GET /motion/events: last motion events, newest first (STA and AP, like the page). One event is
 * at most about 140 characters; the buffer holds LD_MOTION_EVENTS of them. Only the httpd task
 * runs this handler, so a static buffer is safe and keeps its stack free. */
#define MOTION_EVT_JSON_MAX (LD_MOTION_EVENTS * 160 + 8)
static char s_evt[MOTION_EVT_JSON_MAX];

static esp_err_t events_get(httpd_req_t *req)
{
    motion_item_t it[LD_MOTION_EVENTS];
    int n = ld2410_motion_events(it, LD_MOTION_EVENTS);
    if (n < 0) n = 0;
    if (n > LD_MOTION_EVENTS) n = LD_MOTION_EVENTS;
    uint64_t now = (uint64_t)esp_timer_get_time();
    size_t len = 0;
    s_evt[len++] = '[';
    for (int i = 0; i < n; i++) {
        uint64_t ago = now > it[i].onset_us ? (now - it[i].onset_us) / 1000u : 0;
        char dur[16];
        if (it[i].active) snprintf(dur, sizeof dur, "null");
        else snprintf(dur, sizeof dur, "%lu", (unsigned long)it[i].dur_ms);
        int w = snprintf(s_evt + len, sizeof s_evt - len,
                         "%s{\"no\":%lu,\"ago_ms\":%llu,\"dur_ms\":%s,\"energy\":%u,"
                         "\"dist\":%u,\"min\":%u,\"max\":%u}",
                         i ? "," : "", (unsigned long)it[i].no, (unsigned long long)ago, dur,
                         (unsigned)it[i].energy, (unsigned)it[i].dist_cm, (unsigned)it[i].min_cm,
                         (unsigned)it[i].max_cm);
        if (w < 0 || (size_t)w >= sizeof s_evt - len - 2) {
            return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, NULL);
        }
        len += (size_t)w;
    }
    s_evt[len++] = ']';
    s_evt[len] = '\0';
    httpd_resp_set_type(req, "application/json");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    return httpd_resp_send(req, s_evt, (ssize_t)len);
}

/* Handshake done: register the client. Returning ESP_FAIL closes the connection. */
static esp_err_t ws_register(httpd_req_t *req)
{
    int fd = httpd_req_to_sockfd(req);
    xSemaphoreTake(s_lock, portMAX_DELAY);
    int before = ws_slots_count(&s_slots);
    int rc = ws_slots_add(&s_slots, fd); /* idempotent for an fd already registered */
    int added = ws_slots_count(&s_slots) > before;
    xSemaphoreGive(s_lock);
    if (rc < 0) {
        ESP_LOGW(TAG, "no free WebSocket slot (fd %d)", fd);
        return ESP_FAIL;
    }
    if (added) {
        struct timeval tv = {.tv_sec = LIVE_WS_SEND_TIMEOUT_S, .tv_usec = 0};
        if (setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof tv) != 0) {
            ESP_LOGW(TAG, "SO_SNDTIMEO on fd %d failed (errno %d), keeping the client", fd, errno);
        }
        ESP_LOGI(TAG, "ws client added fd=%d slot=%d", fd, rc);
    }
    return ESP_OK;
}

static esp_err_t ws_handler(httpd_req_t *req)
{
    /* Only ESP-IDF <= v5.5.4 / v6.0 call the handler for the handshake GET;
     * newer ones use ws_post_handshake_cb (see the #error above). */
    if (req->method == HTTP_GET) return ws_register(req);
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
    if (err == ESP_OK && fd >= 0) s_sent++;
    xSemaphoreGive(s_lock);
    if (err != ESP_OK && fd >= 0) {
        /* at most once per connection: the close below drops the slot */
        ESP_LOGW(TAG, "ws send failed (fd %d): %s", fd, esp_err_to_name(err));
        httpd_sess_trigger_close(http_srv_handle(), fd); /* close_fn drops the slot */
    }
}

static void fill_gps(snapshot_t *s)
{
    switch (gps_status()) {
    case GPS_LINK_OK: s->gps_status = SNAP_GPS_OK; break;
    case GPS_LINK_SILENT: s->gps_status = SNAP_GPS_SILENT; break;
    default: s->gps_status = SNAP_GPS_ABSENT; break;
    }
    gps_snapshot_t g;
    if (gps_get_snapshot(&g) && g.valid) {
        s->have_gps = 1;
        s->gps = g.fix;
    }
    switch (gps_utc_state()) {
    case GPS_UTC_VALID: s->gps_utc_state = SNAP_UTC_VALID; break;
    case GPS_UTC_NOT_VALID: s->gps_utc_state = SNAP_UTC_NOT_VALID; break;
    case GPS_UTC_NO_UBX: s->gps_utc_state = SNAP_UTC_NO_UBX; break;
    default: s->gps_utc_state = SNAP_UTC_UNKNOWN; break;
    }
}

static void fill_imu(snapshot_t *s)
{
    switch (imu_status()) {
    case IMU_LINK_OK: s->imu_status = SNAP_IMU_OK; break;
    case IMU_LINK_ERROR: s->imu_status = SNAP_IMU_ERROR; break;
    default: s->imu_status = SNAP_IMU_ABSENT; break;
    }
    imu_snapshot_t m;
    if (imu_get_snapshot(&m) && m.valid) {
        s->have_imu = 1;
        s->imu = m.rec;
        /* the task may stall without changing its status: stale data is an error */
        if (s->imu_status == SNAP_IMU_OK &&
            (int64_t)(esp_timer_get_time() - (int64_t)m.time_us) > LIVE_IMU_STALE_US)
            s->imu_status = SNAP_IMU_ERROR;
    } else if (s->imu_status == SNAP_IMU_OK) {
        s->imu_status = SNAP_IMU_ERROR; /* "ok" without any record yet is not usable */
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
        s.frame_no = ls.frame_no;
        s.esp_time_us = ls.time_us;
    }
    switch (ld2410_link_status()) {
    case LD_LINK_OK: s.link = SNAP_LINK_OK; break;
    case LD_LINK_LOST: s.link = SNAP_LINK_LOST; break;
    default: s.link = SNAP_LINK_NO_DATA; break;
    }
    fill_gps(&s);
    fill_imu(&s);
    {
        int en, active, dist;
        uint32_t no;
        ld2410_motion_state(&en, &active, &no, &dist);
        s.motion_en = en != 0;
        s.motion_active = active != 0;
        s.motion_n = no;
        s.have_motion_dist = dist >= 0;
        s.motion_dist_cm = dist >= 0 ? (uint16_t)dist : 0;
    }
    s.time_source = time_source_current();
    int n = snapshot_json(s_tmp, sizeof s_tmp, &s);
    if (n <= 0) return;

    int todo[WS_SLOTS_MAX], cnt = 0;
    xSemaphoreTake(s_lock, portMAX_DELAY);
    for (int i = 0; i < WS_SLOTS_MAX; i++) {
        if (ws_slots_begin(&s_slots, i)) { /* skips clients whose last send is pending */
            memcpy(s_buf[i], s_tmp, (size_t)n + 1);
            s_len[i] = (size_t)n;
            todo[cnt++] = i;
        } else if (ws_slots_fd(&s_slots, i) >= 0) {
            s_skipped_inflight++;
        }
    }
    xSemaphoreGive(s_lock);

    for (int k = 0; k < cnt; k++) {
        if (httpd_queue_work(http_srv_handle(), send_work, (void *)(intptr_t)todo[k]) != ESP_OK) {
            xSemaphoreTake(s_lock, portMAX_DELAY);
            ws_slots_end(&s_slots, todo[k]);
            s_queue_fail++;
            xSemaphoreGive(s_lock);
        }
    }
}

/* One line per 10 s; the counters cover that window. */
static void stats_log(void)
{
    xSemaphoreTake(s_lock, portMAX_DELAY);
    int clients = ws_slots_count(&s_slots);
    unsigned sent = s_sent, skipped = s_skipped_inflight, qfail = s_queue_fail;
    s_sent = s_skipped_inflight = s_queue_fail = 0;
    xSemaphoreGive(s_lock);
    ESP_LOGI(TAG, "clients=%d sent=%u skipped_inflight=%u queue_fail=%u", clients, sent,
             skipped, qfail);
}

/* Dedicated task (not the shared esp_timer task): it takes mutexes and queues
 * work to httpd. Latest-only: a slot with a send still pending is skipped. */
static void live_task(void *arg)
{
    (void)arg;
    TickType_t last = xTaskGetTickCount();
    unsigned ticks = 0;
    for (;;) {
        vTaskDelayUntil(&last, pdMS_TO_TICKS(LIVE_PERIOD_MS));
        tick();
        if (++ticks >= LIVE_STATS_TICKS) {
            ticks = 0;
            stats_log();
        }
    }
}

esp_err_t live_start(void)
{
    if (s_lock == NULL || http_srv_handle() == NULL || s_task != NULL) return ESP_ERR_INVALID_STATE;
    static const httpd_uri_t get_page = {.uri = "/", .method = HTTP_GET, .handler = page_get};
    static const httpd_uri_t get_ws = {.uri = "/ws", .method = HTTP_GET, .handler = ws_handler,
                                       .is_websocket = true,
#ifdef CONFIG_HTTPD_WS_POST_HANDSHAKE_CB_SUPPORT
                                       .ws_post_handshake_cb = ws_register,
#endif
    };
    static const httpd_uri_t get_events = {.uri = "/motion/events", .method = HTTP_GET,
                                           .handler = events_get};
    esp_err_t err = http_srv_register(&get_page);
    if (err == ESP_OK) err = http_srv_register(&get_ws);
    if (err == ESP_OK) err = http_srv_register(&get_events);
    if (err != ESP_OK) return err;
    BaseType_t ok = xTaskCreate(live_task, "live_10hz", 4096, NULL, 4, &s_task);
    return ok == pdPASS ? ESP_OK : ESP_ERR_NO_MEM;
}
