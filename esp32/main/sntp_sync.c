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

static bool s_started;
static volatile bool s_have_sntp;

/* Runs in the SNTP/lwIP task: keep it short. */
static void sync_cb(struct timeval *tv)
{
    (void)tv;
    struct timeval now;
    gettimeofday(&now, NULL);
    uint64_t esp_us = (uint64_t)esp_timer_get_time(); /* back-to-back with gettimeofday */
    int64_t utc_us = (int64_t)now.tv_sec * 1000000 + now.tv_usec;
    s_have_sntp = true;

    rec_time_sync_t ts = {.utc_unix_us = utc_us, .source = TIME_SRC_SNTP};
    uint8_t buf[REC_TIME_SYNC_LEN];
    if (rec_time_sync_encode(buf, &ts) == 0) {
        int rc = ld2410_ring_push(esp_us, REC_TYPE_TIME_SYNC, buf, sizeof(buf), NULL);
        if (rc != 0) {
            ESP_LOGW(TAG, "ring push failed (%d)", rc);
        }
    }
    struct tm tmv;
    time_t sec = now.tv_sec;
    gmtime_r(&sec, &tmv);
    char iso[24];
    strftime(iso, sizeof iso, "%Y-%m-%dT%H:%M:%SZ", &tmv);
    ESP_LOGI(TAG, "SNTP sync: %s", iso);
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
