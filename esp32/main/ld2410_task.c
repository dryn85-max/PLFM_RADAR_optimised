#include "ld2410_task.h"

#include <errno.h>
#include <inttypes.h>
#include <string.h>

#include "driver/uart.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

#include "ld2410_cmd.h"
#include "ld2410_parser.h"
#include "rec_payload.h"
#include "rec_proto.h"

#define LD_UART UART_NUM_1
#define LD_BAUD 256000
#define LD_PIN_TX 17
#define LD_PIN_RX 18
#define LD_RX_BUF 2048
#define LD_READ_CHUNK 256
#define LD_ACK_TIMEOUT_MS 500
#define LD_LOST_US 1000000
#define LD_RATE_LOG_US 10000000
#define LD_ENG_RETRY_US 5000000 /* between engineering-mode enable attempts */
#define LD_ENG_RETRIES 5        /* retries after the boot attempt */

static const char *TAG = "ld2410";

static rb_t s_ring;
static SemaphoreHandle_t s_ring_mtx;
static ld_snapshot_t s_snap;
static SemaphoreHandle_t s_snap_mtx;
static volatile bool s_engineering;
static ld_parser_t s_parser;

/* Callback context. Only the LD2410C task touches it. */
typedef struct {
    uint16_t want_cmd; /* 0 = not waiting for an ACK */
    bool ack_seen;
    uint16_t ack_status;
    uint32_t frames_window; /* data frames since last rate log */
    bool eng_frames_seen;   /* an engineering data frame was decoded */
} ld_ctx_t;

static void on_data(const ld_frame_t *f, ld_ctx_t *ctx)
{
    ld_data_t d;
    if (ld_frame_decode(f->payload, f->payload_len, &d) != 0) {
        ESP_LOGW(TAG, "bad data frame (%u bytes)", (unsigned)f->raw_len);
        return;
    }
    /* Stamped when the parser completes the frame, not once per UART read. */
    uint64_t now = (uint64_t)esp_timer_get_time();
    if (d.engineering) {
        ctx->eng_frames_seen = true;
    }
    uint32_t seq = 0;
    int rc;

    xSemaphoreTake(s_ring_mtx, portMAX_DELAY);
    rc = rb_push(&s_ring, now, REC_TYPE_LD2410_FRAME, f->raw, f->raw_len, &seq);
    xSemaphoreGive(s_ring_mtx);
    if (rc != 0) {
        ESP_LOGW(TAG, "ring push failed (%d)", rc);
        return;
    }

    xSemaphoreTake(s_snap_mtx, portMAX_DELAY);
    s_snap.valid = true;
    s_snap.data = d;
    s_snap.seq = seq;
    s_snap.time_us = now;
    xSemaphoreGive(s_snap_mtx);
    ctx->frames_window++;
}

static void on_frame(const ld_frame_t *f, void *vctx)
{
    ld_ctx_t *ctx = (ld_ctx_t *)vctx;
    if (f->kind == LD_FRAME_DATA) {
        on_data(f, ctx);
    } else if (f->kind == LD_FRAME_CMD) {
        ld_ack_t ack;
        if (ld_cmd_decode_ack(f->payload, f->payload_len, &ack) == 0 &&
            ctx->want_cmd != 0 && ack.cmd == ctx->want_cmd) {
            ctx->ack_seen = true;
            ctx->ack_status = ack.status;
        }
    }
}

/* Wait up to `wait` for at least one byte, then take whatever is buffered
 * (never more than one chunk) without waiting, so frames are parsed as soon as
 * they arrive instead of being batched behind a 256-byte read. */
static void pump(ld_ctx_t *ctx, TickType_t wait)
{
    uint8_t chunk[LD_READ_CHUNK];
    int n = uart_read_bytes(LD_UART, chunk, 1, wait);
    if (n <= 0) {
        return;
    }
    size_t buffered = 0;
    if (uart_get_buffered_data_len(LD_UART, &buffered) == ESP_OK && buffered > 0) {
        size_t room = sizeof(chunk) - (size_t)n;
        int m = uart_read_bytes(LD_UART, chunk + n, buffered < room ? buffered : room, 0);
        if (m > 0) {
            n += m;
        }
    }
    ld_parser_feed(&s_parser, chunk, (size_t)n, on_frame, ctx);
}

/* Send one encoded command and wait for its ACK. Returns true on ACK status 0. */
static bool send_cmd(ld_ctx_t *ctx, uint16_t cmd, const uint8_t *frame, size_t len,
                     const char *name)
{
    ctx->want_cmd = cmd;
    ctx->ack_seen = false;
    ctx->ack_status = 0xFFFF;
    int w = uart_write_bytes(LD_UART, (const char *)frame, len);
    if (w != (int)len) {
        ESP_LOGW(TAG, "%s: UART write failed (%d)", name, w);
        ctx->want_cmd = 0;
        return false;
    }
    int64_t deadline = esp_timer_get_time() + (int64_t)LD_ACK_TIMEOUT_MS * 1000;
    while (!ctx->ack_seen && esp_timer_get_time() < deadline) {
        pump(ctx, pdMS_TO_TICKS(20));
    }
    ctx->want_cmd = 0;
    if (!ctx->ack_seen) {
        ESP_LOGW(TAG, "%s: no ACK within %d ms", name, LD_ACK_TIMEOUT_MS);
        return false;
    }
    if (ctx->ack_status != 0) {
        ESP_LOGW(TAG, "%s: ACK status 0x%04x", name, (unsigned)ctx->ack_status);
        return false;
    }
    ESP_LOGI(TAG, "%s: ACK ok", name);
    return true;
}

static void enable_engineering(ld_ctx_t *ctx)
{
    uint8_t buf[LD_CMD_MAX_FRAME];
    int n;
    bool ok;

    n = ld_cmd_encode_enable_config(buf, sizeof(buf));
    ok = n > 0 && send_cmd(ctx, LD_CMD_ENABLE_CONFIG, buf, (size_t)n, "enable-config");
    if (ok) {
        n = ld_cmd_encode_enable_engineering(buf, sizeof(buf));
        ok = n > 0 && send_cmd(ctx, LD_CMD_ENABLE_ENGINEERING, buf, (size_t)n,
                               "enable-engineering");
        s_engineering = ok;
    }
    /* Always try to leave configuration mode, even after a failure. */
    n = ld_cmd_encode_end_config(buf, sizeof(buf));
    if (n > 0) {
        (void)send_cmd(ctx, LD_CMD_END_CONFIG, buf, (size_t)n, "end-config");
    }
    if (s_engineering) {
        ESP_LOGI(TAG, "engineering mode enabled");
    } else {
        ESP_LOGW(TAG, "engineering mode not enabled, continuing in normal mode");
    }
}

static void ld2410_task(void *arg)
{
    (void)arg;
    ld_ctx_t ctx;
    memset(&ctx, 0, sizeof(ctx));
    ld_parser_init(&s_parser);

    enable_engineering(&ctx);

    int64_t window_start = esp_timer_get_time();
    int64_t last_eng_try = window_start;
    int eng_retries = 0;
    for (;;) {
        pump(&ctx, pdMS_TO_TICKS(100));
        int64_t now = esp_timer_get_time();
        /* Engineering mode was not acknowledged at boot: retry now and then.
         * Data frames keep being parsed inside send_cmd()'s pump loop. */
        if (!s_engineering && !ctx.eng_frames_seen && eng_retries < LD_ENG_RETRIES &&
            now - last_eng_try >= LD_ENG_RETRY_US) {
            eng_retries++;
            ESP_LOGI(TAG, "retrying engineering mode (%d/%d)", eng_retries, LD_ENG_RETRIES);
            enable_engineering(&ctx);
            last_eng_try = esp_timer_get_time();
            now = last_eng_try;
        }
        if (now - window_start >= LD_RATE_LOG_US) {
            float secs = (float)(now - window_start) / 1e6f;
            ESP_LOGI(TAG, "%.1f frames/s, parser dropped %" PRIu32 " bytes",
                     (double)((float)ctx.frames_window / secs),
                     s_parser.dropped_bytes);
            ctx.frames_window = 0;
            window_start = now;
        }
    }
}

esp_err_t ld2410_start(void *ring_mem, size_t ring_cap)
{
    if (ring_mem == NULL || ring_cap < RB_REC_HDR + RB_MAX_RAW) {
        return ESP_ERR_INVALID_ARG;
    }
    s_ring_mtx = xSemaphoreCreateMutex();
    s_snap_mtx = xSemaphoreCreateMutex();
    if (s_ring_mtx == NULL || s_snap_mtx == NULL) {
        return ESP_ERR_NO_MEM;
    }
    rb_init(&s_ring, ring_mem, ring_cap, 0);
    memset(&s_snap, 0, sizeof(s_snap));

    const uart_config_t cfg = {
        .baud_rate = LD_BAUD,
        .data_bits = UART_DATA_8_BITS,
        .parity = UART_PARITY_DISABLE,
        .stop_bits = UART_STOP_BITS_1,
        .flow_ctrl = UART_HW_FLOWCTRL_DISABLE,
        .source_clk = UART_SCLK_DEFAULT,
    };
    esp_err_t err = uart_driver_install(LD_UART, LD_RX_BUF, 0, 0, NULL, 0);
    if (err != ESP_OK) {
        return err;
    }
    err = uart_param_config(LD_UART, &cfg);
    if (err != ESP_OK) {
        return err;
    }
    err = uart_set_pin(LD_UART, LD_PIN_TX, LD_PIN_RX, UART_PIN_NO_CHANGE,
                       UART_PIN_NO_CHANGE);
    if (err != ESP_OK) {
        return err;
    }

    BaseType_t ok = xTaskCreatePinnedToCore(ld2410_task, "ld2410", 4096, NULL, 5,
                                            NULL, 1);
    return ok == pdPASS ? ESP_OK : ESP_ERR_NO_MEM;
}

bool ld2410_get_snapshot(ld_snapshot_t *out)
{
    if (out == NULL || s_snap_mtx == NULL) {
        return false;
    }
    xSemaphoreTake(s_snap_mtx, portMAX_DELAY);
    *out = s_snap;
    xSemaphoreGive(s_snap_mtx);
    return out->valid;
}

ld_link_t ld2410_link_status(void)
{
    ld_snapshot_t s;
    if (!ld2410_get_snapshot(&s)) {
        return LD_LINK_NO_DATA;
    }
    int64_t age = esp_timer_get_time() - (int64_t)s.time_us;
    return age > LD_LOST_US ? LD_LINK_LOST : LD_LINK_OK;
}

bool ld2410_engineering_enabled(void)
{
    return s_engineering;
}

void ld2410_ring_lock(void)
{
    xSemaphoreTake(s_ring_mtx, portMAX_DELAY);
}

void ld2410_ring_unlock(void)
{
    xSemaphoreGive(s_ring_mtx);
}

const rb_t *ld2410_ring(void)
{
    return &s_ring;
}

int ld2410_ring_batch(rec_stream_t *st, uint32_t from_seq, uint32_t boot_id,
                      uint8_t *buf, size_t cap, size_t *out_len, uint16_t *count)
{
    ld2410_ring_lock();
    int rc = rec_stream_batch(&s_ring, st, from_seq, boot_id, buf, cap, out_len, count);
    ld2410_ring_unlock();
    return rc;
}
