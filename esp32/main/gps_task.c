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
#include "gps_rx.h"
#include "nmea.h"
#include "ubx.h"
#include "rec_payload.h"

#define GPS_UART UART_NUM_2
#define GPS_BAUD 9600
#define GPS_PIN_TX 4 /* ESP32 TX -> module RX (NAV-TIMEUTC poll once per second) */
#define GPS_PIN_RX 5 /* module TX -> ESP32 RX */
#define GPS_RX_BUF 2048
#define GPS_TX_BUF 256 /* uart_write_bytes copies into it and returns: the 8-byte poll never blocks */
#define GPS_READ_CHUNK 128
#define GPS_SILENT_US 3000000
#define GPS_LOG_US 10000000
#define GPS_POLL_US 1000000
/* The ESP stamp of a GPS time_sync is taken when the RMC line completes, d = 128 ms after
 * the UTC instant the RMC names (bench 2026-10-02 against SNTP, p1..p99 119..136 ms). The
 * host maps utc(t) = utc_sync + (t - t_sync) (host/ld2410_rec.py utc_us), so for the same
 * stamp the record must carry utc_rmc + d. Only time_sync gets it; gps_fix and the live
 * snapshot keep the raw RMC time. */
#define GPS_RMC_DELAY_US 128000
/* UTC state machine (spec R3): a NAV-TIMEUTC answer younger than UTC_FRESH_US decides
 * valid / not valid; with no fresh answer the state is "not valid" (wait, GPS time is not
 * used yet) until NO_UBX_US have passed since the later of the last answer and the talk
 * start (the first good NMEA line after the link was unknown: never talked, or silent for
 * more than GPS_SILENT_US); then "no ubx" (time_sync fallback). The talk start matters
 * when the module comes back after a silence: its old answer is stale, and without the
 * talk start the state would be "no ubx" at once and an unverified time_sync could be
 * written before the first new answer. */
#define UTC_FRESH_US 3000000
#define UTC_NO_UBX_US 5000000
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
static uint64_t s_talk_start_us; /* first good NMEA line after the link was unknown */
static bool s_have_utc;          /* a NAV-TIMEUTC answer was decoded */
static uint64_t s_utc_us;        /* when the latest one was decoded */
static bool s_utc_valid;         /* its validUTC bit */
static gps_ubx_counters_t s_ubx_cnt;

/* Callback context, only the GPS task touches it. */
typedef struct {
    uint64_t rmc_us; /* when the latest RMC line completed */
    bool rmc_us_set;
    uint32_t fixes_pushed;
} gps_ctx_t;

/* Needs s_mtx held. Returns SNAP-style state for gps_utc_t. */
static gps_utc_t utc_state_locked(uint64_t now)
{
    if (!s_any_bytes || s_last_good_us == 0 || (int64_t)(now - s_last_good_us) > GPS_SILENT_US) {
        return GPS_UTC_UNKNOWN;
    }
    if (s_have_utc && (int64_t)(now - s_utc_us) < UTC_FRESH_US) {
        return s_utc_valid ? GPS_UTC_VALID : GPS_UTC_NOT_VALID;
    }
    uint64_t ref = (s_have_utc && (int64_t)(s_utc_us - s_talk_start_us) > 0) ? s_utc_us : s_talk_start_us;
    return (int64_t)(now - ref) > UTC_NO_UBX_US ? GPS_UTC_NO_UBX : GPS_UTC_NOT_VALID;
}

static gps_utc_t utc_state_now(void)
{
    xSemaphoreTake(s_mtx, portMAX_DELAY);
    gps_utc_t st = utc_state_locked((uint64_t)esp_timer_get_time());
    xSemaphoreGive(s_mtx);
    return st;
}

static void on_ubx(uint8_t cls, uint8_t id, const uint8_t *payload, size_t len, void *vctx)
{
    (void)vctx;
    if (cls != UBX_CLASS_NAV || id != UBX_ID_NAV_TIMEUTC) {
        return;
    }
    ubx_timeutc_t t;
    if (ubx_decode_nav_timeutc(payload, len, &t) != 0) {
        return;
    }
    uint64_t now = (uint64_t)esp_timer_get_time();
    xSemaphoreTake(s_mtx, portMAX_DELAY);
    s_have_utc = true;
    s_utc_us = now;
    s_utc_valid = ubx_timeutc_valid_utc(&t);
    xSemaphoreGive(s_mtx);
}

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
        gps_utc_t st = utc_state_now();
        /* time_sync only with verified UTC, or as the fallback when the module does not
         * answer UBX at all (spec decision 2); never while "not valid" (leap seconds). */
        if ((ev->fix.flags & both) == both && ev->fix.utc_unix_ms > 0 &&
            (st == GPS_UTC_VALID || st == GPS_UTC_NO_UBX)) {
            rec_time_sync_t ts = {.utc_unix_us = ev->fix.utc_unix_ms * 1000 + GPS_RMC_DELAY_US, .source = TIME_SRC_GPS};
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
    rec_gps_fix_t fix = ev->fix;
    const uint8_t both = GPS_FLAG_TIME_VALID | GPS_FLAG_DATE_VALID;
    if ((fix.flags & both) == both && utc_state_now() == GPS_UTC_VALID) {
        fix.flags |= GPS_FLAG_UTC_VERIFIED;
    }
    uint8_t buf[REC_GPS_FIX_LEN];
    if (rec_gps_fix_encode(buf, &fix) == 0) {
        push_record(t, REC_TYPE_GPS_FIX, buf, sizeof(buf));
        ctx->fixes_pushed++;
    }
    xSemaphoreTake(s_mtx, portMAX_DELAY);
    s_snap.valid = true;
    s_snap.fix = fix;
    s_snap.time_us = t;
    xSemaphoreGive(s_mtx);
}

static void gps_task(void *arg)
{
    (void)arg;
    gps_rx_t rx;
    gps_ctx_t ctx;
    memset(&ctx, 0, sizeof(ctx));
    gps_rx_init(&rx);
    uint8_t poll[UBX_POLL_LEN];
    int poll_len = ubx_encode_poll(UBX_CLASS_NAV, UBX_ID_NAV_TIMEUTC, poll, sizeof(poll));
    int64_t last_poll = 0;
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
            gps_rx_feed(&rx, chunk, (size_t)n, on_event, &ctx, on_ubx, &ctx);
            uint64_t now = (uint64_t)esp_timer_get_time();
            xSemaphoreTake(s_mtx, portMAX_DELAY);
            s_any_bytes = true;
            if (rx.nmea.good_lines != seen_good) {
                if (s_last_good_us == 0 || (int64_t)(now - s_last_good_us) > GPS_SILENT_US) {
                    s_talk_start_us = now; /* link was UNKNOWN: talking again */
                }
                s_last_good_us = now;
            }
            s_snap.good_lines = rx.nmea.good_lines;
            s_snap.bad_lines = rx.nmea.bad_lines;
            s_snap.dropped_lines = rx.nmea.dropped_lines;
            s_ubx_cnt.good = rx.ubx.good;
            s_ubx_cnt.bad_ck = rx.ubx.bad_ck;
            s_ubx_cnt.skipped = rx.ubx.skipped;
            s_ubx_cnt.bad_len = rx.ubx.bad_len;
            xSemaphoreGive(s_mtx);
            seen_good = rx.nmea.good_lines;
        }
        int64_t now = esp_timer_get_time();
        if (poll_len > 0 && now - last_poll >= GPS_POLL_US) {
            last_poll = now;
            /* TX buffer has room (GPS_TX_BUF), so this copies and returns at once. */
            int w = uart_write_bytes(GPS_UART, (const char *)poll, (size_t)poll_len);
            if (w != poll_len) {
                ESP_LOGW(TAG, "UBX poll not queued (%d)", w);
            }
        }
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
                         rx.nmea.good_lines, rx.nmea.bad_lines, rx.nmea.dropped_lines);
            } else {
                ESP_LOGI(TAG, "%s, fix %u, sats %u, hdop %u.%02u, lines good %" PRIu32
                         " bad %" PRIu32 " dropped %" PRIu32,
                         st == GPS_LINK_OK ? "ok" : "silent", (unsigned)s.fix.fix_quality,
                         (unsigned)s.fix.sats, (unsigned)(s.fix.hdop_x100 / 100),
                         (unsigned)(s.fix.hdop_x100 % 100), rx.nmea.good_lines,
                         rx.nmea.bad_lines, rx.nmea.dropped_lines);
            }
            gps_utc_t us = utc_state_now();
            if (us != GPS_UTC_UNKNOWN) {
                gps_ubx_counters_t c;
                xSemaphoreTake(s_mtx, portMAX_DELAY);
                c = s_ubx_cnt;
                xSemaphoreGive(s_mtx);
                const char *name = us == GPS_UTC_VALID ? "utc valid"
                                   : us == GPS_UTC_NOT_VALID ? "utc not valid" : "utc no ubx";
                if (us == GPS_UTC_NO_UBX) {
                    ESP_LOGW(TAG, "%s: no NAV-TIMEUTC answer (GPS time is not verified), ubx good %"
                             PRIu32 " bad_ck %" PRIu32 " skipped %" PRIu32 " bad_len %" PRIu32,
                             name, c.good, c.bad_ck, c.skipped, c.bad_len);
                } else {
                    ESP_LOGI(TAG, "%s, ubx good %" PRIu32 " bad_ck %" PRIu32 " skipped %" PRIu32
                             " bad_len %" PRIu32, name, c.good, c.bad_ck, c.skipped, c.bad_len);
                }
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
    s_talk_start_us = 0;
    s_have_utc = false;
    s_utc_us = 0;
    s_utc_valid = false;
    memset(&s_ubx_cnt, 0, sizeof(s_ubx_cnt));

    const uart_config_t cfg = {
        .baud_rate = GPS_BAUD,
        .data_bits = UART_DATA_8_BITS,
        .parity = UART_PARITY_DISABLE,
        .stop_bits = UART_STOP_BITS_1,
        .flow_ctrl = UART_HW_FLOWCTRL_DISABLE,
        .source_clk = UART_SCLK_DEFAULT,
    };
    esp_err_t err = uart_driver_install(GPS_UART, GPS_RX_BUF, GPS_TX_BUF, 0, NULL, 0);
    if (err != ESP_OK) {
        return err;
    }
    /* Short FIFO threshold and idle timeout: RMC bytes reach the task promptly
     * (time-stamp latency), not only when the default 120-byte threshold fills. */
    err = uart_set_rx_full_threshold(GPS_UART, 16);
    if (err != ESP_OK) {
        return err;
    }
    err = uart_set_rx_timeout(GPS_UART, 2);
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

gps_utc_t gps_utc_state(void)
{
    if (s_mtx == NULL) {
        return GPS_UTC_UNKNOWN;
    }
    return utc_state_now();
}
