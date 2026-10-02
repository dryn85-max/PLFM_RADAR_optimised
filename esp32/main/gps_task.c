#include "gps_task.h"

#include <inttypes.h>
#include <string.h>

#include "driver/uart.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

#include "ld2410_task.h"
#include "nmea.h"
#include "rec_payload.h"

#define GPS_UART UART_NUM_2
#define GPS_BAUD 9600
#define GPS_PIN_TX 4 /* ESP32 TX -> module RX (nothing is sent, factory config) */
#define GPS_PIN_RX 5 /* module TX -> ESP32 RX */
#define GPS_RX_BUF 2048
#define GPS_READ_CHUNK 128
#define GPS_SILENT_US 3000000
#define GPS_LOG_US 10000000
/* Core 1 with the LD2410C task, but below it (priority 5): the GPS load is about
 * 1 kB/s and mostly blocked in uart_read_bytes, and core 1 has no Wi-Fi/lwIP
 * tasks whose bursts would add jitter to the time stamp taken at the RMC line.
 * The LD2410C task preempts this one at any time. */
#define GPS_TASK_PRIO 4
#define GPS_TASK_CORE 1
#define GPS_TASK_STACK 4096

static const char *TAG = "gps";

static SemaphoreHandle_t s_mtx; /* guards everything shared below */
static gps_snapshot_t s_snap;
static bool s_any_bytes;
static uint64_t s_last_good_us;
static uint64_t s_last_sync_us; /* esp_time of the latest GPS time_sync record, 0 = none */

/* Callback context, only the GPS task touches it. */
typedef struct {
    nmea_t *parser;
    uint64_t rmc_us; /* when the latest RMC line completed */
    bool rmc_us_set;
    uint32_t fixes_pushed;
} gps_ctx_t;

static void push_record(uint64_t t_us, uint8_t type, const uint8_t *payload, size_t len)
{
    int rc = ld2410_ring_push(t_us, type, payload, len, NULL);
    if (rc != 0) {
        ESP_LOGW(TAG, "ring push (type %u) failed (%d)", (unsigned)type, rc);
    }
}

static void on_event(const nmea_event_t *ev, void *vctx)
{
    gps_ctx_t *ctx = (gps_ctx_t *)vctx;
    if (ev->kind == NMEA_EV_RMC) {
        /* Stamped when the parser completes the RMC line (callback runs inside feed). */
        ctx->rmc_us = (uint64_t)esp_timer_get_time();
        ctx->rmc_us_set = true;
        const uint8_t both = GPS_FLAG_TIME_VALID | GPS_FLAG_DATE_VALID;
        if ((ev->fix.flags & both) == both && ev->fix.utc_unix_ms > 0) {
            rec_time_sync_t ts = {.utc_unix_us = ev->fix.utc_unix_ms * 1000, .source = TIME_SRC_GPS};
            uint8_t buf[REC_TIME_SYNC_LEN];
            if (rec_time_sync_encode(buf, &ts) == 0) {
                push_record(ctx->rmc_us, REC_TYPE_TIME_SYNC, buf, sizeof(buf));
                xSemaphoreTake(s_mtx, portMAX_DELAY);
                s_last_sync_us = ctx->rmc_us;
                xSemaphoreGive(s_mtx);
            }
        }
        return;
    }

    /* Epoch fix: stamp = its RMC line; an epoch without RMC gets the current time. */
    uint64_t t = (ev->have_rmc && ctx->rmc_us_set) ? ctx->rmc_us
                                                   : (uint64_t)esp_timer_get_time();
    ctx->rmc_us_set = false;
    uint8_t buf[REC_GPS_FIX_LEN];
    if (rec_gps_fix_encode(buf, &ev->fix) == 0) {
        push_record(t, REC_TYPE_GPS_FIX, buf, sizeof(buf));
        ctx->fixes_pushed++;
    }
    xSemaphoreTake(s_mtx, portMAX_DELAY);
    s_snap.valid = true;
    s_snap.fix = ev->fix;
    s_snap.time_us = t;
    xSemaphoreGive(s_mtx);
}

static void gps_task(void *arg)
{
    (void)arg;
    nmea_t parser;
    gps_ctx_t ctx;
    memset(&ctx, 0, sizeof(ctx));
    nmea_init(&parser);
    ctx.parser = &parser;
    uint8_t chunk[GPS_READ_CHUNK];
    uint32_t seen_good = 0;
    int64_t last_log = esp_timer_get_time();

    for (;;) {
        /* Wait for one byte, then take what is buffered without waiting, so a
         * line is parsed (and stamped) as soon as its last byte arrives. */
        int n = uart_read_bytes(GPS_UART, chunk, 1, pdMS_TO_TICKS(200));
        if (n > 0) {
            size_t buffered = 0;
            if (uart_get_buffered_data_len(GPS_UART, &buffered) == ESP_OK && buffered > 0) {
                size_t room = sizeof(chunk) - (size_t)n;
                int m = uart_read_bytes(GPS_UART, chunk + n, buffered < room ? buffered : room, 0);
                if (m > 0) {
                    n += m;
                }
            }
            nmea_feed(&parser, chunk, (size_t)n, on_event, &ctx);
            uint64_t now = (uint64_t)esp_timer_get_time();
            xSemaphoreTake(s_mtx, portMAX_DELAY);
            s_any_bytes = true;
            if (parser.good_lines != seen_good) {
                s_last_good_us = now;
            }
            s_snap.good_lines = parser.good_lines;
            s_snap.bad_lines = parser.bad_lines;
            s_snap.dropped_lines = parser.dropped_lines;
            xSemaphoreGive(s_mtx);
            seen_good = parser.good_lines;
        }
        int64_t now = esp_timer_get_time();
        if (now - last_log >= GPS_LOG_US) {
            last_log = now;
            gps_snapshot_t s;
            bool have = gps_get_snapshot(&s);
            gps_link_t st = gps_status();
            if (st == GPS_LINK_ABSENT) {
                ESP_LOGI(TAG, "no data from the module (not connected?)");
            } else if (!have) {
                ESP_LOGI(TAG, "%s, no epoch yet, lines good %" PRIu32 " bad %" PRIu32
                         " dropped %" PRIu32, st == GPS_LINK_OK ? "ok" : "silent",
                         parser.good_lines, parser.bad_lines, parser.dropped_lines);
            } else {
                ESP_LOGI(TAG, "%s, fix %u, sats %u, hdop %u.%02u, lines good %" PRIu32
                         " bad %" PRIu32 " dropped %" PRIu32,
                         st == GPS_LINK_OK ? "ok" : "silent", (unsigned)s.fix.fix_quality,
                         (unsigned)s.fix.sats, (unsigned)(s.fix.hdop_x100 / 100),
                         (unsigned)(s.fix.hdop_x100 % 100), parser.good_lines,
                         parser.bad_lines, parser.dropped_lines);
            }
        }
    }
}

esp_err_t gps_start(void)
{
    s_mtx = xSemaphoreCreateMutex();
    if (s_mtx == NULL) {
        return ESP_ERR_NO_MEM;
    }
    memset(&s_snap, 0, sizeof(s_snap));
    s_any_bytes = false;
    s_last_good_us = 0;
    s_last_sync_us = 0;

    const uart_config_t cfg = {
        .baud_rate = GPS_BAUD,
        .data_bits = UART_DATA_8_BITS,
        .parity = UART_PARITY_DISABLE,
        .stop_bits = UART_STOP_BITS_1,
        .flow_ctrl = UART_HW_FLOWCTRL_DISABLE,
        .source_clk = UART_SCLK_DEFAULT,
    };
    esp_err_t err = uart_driver_install(GPS_UART, GPS_RX_BUF, 0, 0, NULL, 0);
    if (err != ESP_OK) {
        return err;
    }
    err = uart_param_config(GPS_UART, &cfg);
    if (err != ESP_OK) {
        return err;
    }
    err = uart_set_pin(GPS_UART, GPS_PIN_TX, GPS_PIN_RX, UART_PIN_NO_CHANGE, UART_PIN_NO_CHANGE);
    if (err != ESP_OK) {
        return err;
    }
    BaseType_t ok = xTaskCreatePinnedToCore(gps_task, "gps", GPS_TASK_STACK, NULL, GPS_TASK_PRIO,
                                            NULL, GPS_TASK_CORE);
    return ok == pdPASS ? ESP_OK : ESP_ERR_NO_MEM;
}

bool gps_get_snapshot(gps_snapshot_t *out)
{
    if (out == NULL || s_mtx == NULL) {
        return false;
    }
    xSemaphoreTake(s_mtx, portMAX_DELAY);
    *out = s_snap;
    xSemaphoreGive(s_mtx);
    return out->valid;
}

gps_link_t gps_status(void)
{
    if (s_mtx == NULL) {
        return GPS_LINK_ABSENT;
    }
    xSemaphoreTake(s_mtx, portMAX_DELAY);
    bool any = s_any_bytes;
    uint64_t last = s_last_good_us;
    xSemaphoreGive(s_mtx);
    if (!any) {
        return GPS_LINK_ABSENT;
    }
    if (last == 0 || (int64_t)((uint64_t)esp_timer_get_time() - last) > GPS_SILENT_US) {
        return GPS_LINK_SILENT;
    }
    return GPS_LINK_OK;
}

uint64_t gps_last_time_sync_us(void)
{
    if (s_mtx == NULL) {
        return 0;
    }
    xSemaphoreTake(s_mtx, portMAX_DELAY);
    uint64_t t = s_last_sync_us;
    xSemaphoreGive(s_mtx);
    return t;
}
