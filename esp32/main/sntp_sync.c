#include "sntp_sync.h"

#include <stdbool.h>
#include <stdio.h>
#include <sys/time.h>
#include <time.h>

#include "esp_log.h"
#include "esp_netif_sntp.h"
#include "esp_timer.h"

#include "gps_task.h"
#include "ld2410_task.h"
#include "rec_payload.h"

static const char *TAG = "sntp";

#define RESYNC_PERIOD_S 60 /* SNTP itself syncs hourly: re-emit so every recording holds a time_sync */

static bool s_started;
static volatile bool s_have_sntp;
static volatile bool s_log_pending; /* set by the lwIP callback, consumed by tick_cb */
static esp_timer_handle_t s_tick;
static unsigned s_ticks;

/* Read the system clock and the ESP timer back-to-back and push a type-3 record
 * (source SNTP). Only reads clocks and pushes to the ring: no logging, no malloc. */
static int emit_time_sync(time_t *sec_out)
{
    struct timeval now;
    gettimeofday(&now, NULL);
    uint64_t esp_us = (uint64_t)esp_timer_get_time();
    int64_t utc_us = (int64_t)now.tv_sec * 1000000 + now.tv_usec;
    if (sec_out != NULL) {
        *sec_out = now.tv_sec;
    }
    rec_time_sync_t ts = {.utc_unix_us = utc_us, .source = TIME_SRC_SNTP};
    uint8_t buf[REC_TIME_SYNC_LEN];
    if (rec_time_sync_encode(buf, &ts) != 0) {
        return -1;
    }
    return ld2410_ring_push(esp_us, REC_TYPE_TIME_SYNC, buf, sizeof(buf), NULL);
}

/* Runs in the SNTP/lwIP task: minimal (clock reads, ring push, two flags). */
static void sync_cb(struct timeval *tv)
{
    (void)tv;
    emit_time_sync(NULL);
    s_have_sntp = true;
    s_log_pending = true;
}

/* 1 s esp_timer callback (esp_timer task, not lwIP): deferred log and the
 * periodic re-emit once SNTP has synced at least once. */
static void tick_cb(void *arg)
{
    (void)arg;
    if (!s_have_sntp) {
        return;
    }
    time_t sec = 0;
    int rc = 0;
    if (s_log_pending) {
        s_log_pending = false;
        s_ticks = 0;
        /* the on-sync record was already pushed by sync_cb */
        struct timeval now;
        gettimeofday(&now, NULL);
        sec = now.tv_sec;
    } else if (++s_ticks >= RESYNC_PERIOD_S) {
        s_ticks = 0;
        rc = emit_time_sync(&sec);
    } else {
        return;
    }
    if (rc != 0) {
        ESP_LOGW(TAG, "ring push failed (%d)", rc);
    }
    struct tm tmv;
    gmtime_r(&sec, &tmv);
    char iso[24];
    strftime(iso, sizeof iso, "%Y-%m-%dT%H:%M:%SZ", &tmv);
    ESP_LOGI(TAG, "SNTP time %s", iso);
}

void sntp_sync_start(void)
{
    if (s_started) {
        return;
    }
    esp_sntp_config_t cfg = ESP_NETIF_SNTP_DEFAULT_CONFIG("pool.ntp.org");
    cfg.sync_cb = sync_cb;
    esp_err_t err = esp_netif_sntp_init(&cfg);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "esp_netif_sntp_init failed: %s", esp_err_to_name(err));
        return; /* s_started stays false: the next IP event retries */
    }
    s_started = true;
    const esp_timer_create_args_t targs = {.callback = tick_cb, .name = "sntp_tick"};
    if (esp_timer_create(&targs, &s_tick) == ESP_OK) {
        esp_timer_start_periodic(s_tick, 1000000);
    } else {
        ESP_LOGE(TAG, "periodic time_sync timer not created");
    }
}

time_source_t time_source_current(void)
{
    uint64_t last = gps_last_time_sync_us();
    int64_t age = -1;
    if (last != 0) {
        age = (int64_t)((uint64_t)esp_timer_get_time() - last);
    }
    return time_source_decide(age, s_have_sntp);
}
