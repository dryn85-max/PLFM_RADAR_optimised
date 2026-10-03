#include "ld2410_task.h"

#include <errno.h>
#include <inttypes.h>
#include <stdio.h>
#include <string.h>

#include "driver/uart.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

#include "nvs.h"

#include "ld2410_cmd.h"
#include "ld_settings.h"
#include "ld2410_parser.h"
#include "motion_det.h"
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
/* Engineering mode is expected but normal-mode frames (data_type 0x02) keep
 * arriving for this long: the module never got the command, or reset itself
 * (power glitch) and restarted in normal mode. Re-run the enable sequence, at
 * most once per LD_ENG_RETRY_US, for as long as it takes. */
#define LD_ENG_NORMAL_US 3000000
#define LD_ENG_RETRY_US 10000000
/* Detector tick while no data frame arrives (link lost): an ACTIVE event must still end. */
#define LD_MOTION_TICK_US 1000000

#define MOTION_NVS_NS "motion"
#define MOTION_NVS_KEY "cfg"
#define MOTION_NVS_VER 1
#define MOTION_NVS_LEN 15 /* ver, en, dmin(2), dmax(2), emin, tstart(4), tend(4), little endian */

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
    uint8_t ack_payload[LD_MAX_PAYLOAD]; /* payload of the awaited ACK (copy; the parser reuses its buffer) */
    size_t ack_len;
    uint32_t frames_window; /* data frames since last rate log */
    uint32_t frame_no;      /* data frames decoded since boot (wraps) */
    int64_t normal_since;   /* esp_timer us of the first normal frame of the current run, 0 = none */
    int64_t last_motion_step_us; /* esp_timer us of the last motion_det_step call */
} ld_ctx_t;

/* ---- Motion detector ----
 * s_det lives in the LD2410C task only (motion_det_step is not thread-safe). Other tasks
 * talk to it through s_mot_mtx, a leaf lock: while it is held nothing else is locked and
 * nothing blocks (no ring, snapshot, NVS or log call), so it cannot take part in a lock-order
 * cycle with s_ring_mtx / s_snap_mtx. It guards s_mcfg (the configuration, written by
 * ld2410_motion_set_cfg), the event list and the page state. */
static motion_det_t s_det;
static SemaphoreHandle_t s_mot_mtx;
static motion_cfg_t s_mcfg;
static motion_item_t s_mev[LD_MOTION_EVENTS]; /* newest first */
static int s_mev_n;
static struct {
    int active;
    uint32_t last_no;
    int dist_cm; /* current moving distance, -1 = none */
} s_mstate = {0, 0, -1};

static void motion_log_start(const motion_ev_t *e)
{
    ESP_LOGI(TAG, "motion start #%u at %u.%02u m, energy %u", (unsigned)e->event_no,
             (unsigned)(e->dist_cm / 100), (unsigned)(e->dist_cm % 100), (unsigned)e->max_energy);
}

static void motion_log_end(const motion_ev_t *e)
{
    ESP_LOGI(TAG, "motion end #%u, %u.%u s, %u.%02u m (%u.%02u..%u.%02u m), energy %u",
             (unsigned)e->event_no, (unsigned)(e->duration_ms / 1000),
             (unsigned)((e->duration_ms % 1000) / 100), (unsigned)(e->dist_cm / 100),
             (unsigned)(e->dist_cm % 100), (unsigned)(e->min_dist_cm / 100),
             (unsigned)(e->min_dist_cm % 100), (unsigned)(e->max_dist_cm / 100),
             (unsigned)(e->max_dist_cm % 100), (unsigned)e->max_energy);
}

/* One detector step (LD2410C task). d == NULL: no frame (link lost). */
static void motion_step(ld_ctx_t *ctx, const ld_data_t *d, uint64_t now)
{
    motion_cfg_t cfg;
    motion_ev_t ev;
    memset(&ev, 0, sizeof(ev));
    xSemaphoreTake(s_mot_mtx, portMAX_DELAY);
    cfg = s_mcfg;
    xSemaphoreGive(s_mot_mtx);

    ctx->last_motion_step_us = (int64_t)now;
    motion_ev_kind_t k = motion_det_step(&s_det, &cfg, d, now, &ev);

    if (k != MOTION_EV_NONE) {
        rec_motion_t r;
        uint8_t buf[REC_MOTION_LEN];
        memset(&r, 0, sizeof(r));
        r.event_no = ev.event_no;
        r.kind = k == MOTION_EV_START ? MOTION_KIND_START : MOTION_KIND_END;
        r.max_energy = ev.max_energy;
        r.dist_cm = ev.dist_cm;
        r.min_dist_cm = ev.min_dist_cm;
        r.max_dist_cm = ev.max_dist_cm;
        r.onset_esp_us = ev.onset_us;
        r.duration_ms = ev.duration_ms;
        if (rec_motion_encode(buf, &r) == 0) {
            int rc;
            xSemaphoreTake(s_ring_mtx, portMAX_DELAY);
            rc = rb_push(&s_ring, now, REC_TYPE_MOTION, buf, sizeof(buf), NULL);
            xSemaphoreGive(s_ring_mtx);
            if (rc != 0) {
                ESP_LOGW(TAG, "motion record push failed (%d)", rc);
            }
        }
        if (k == MOTION_EV_START) {
            motion_log_start(&ev);
        } else {
            motion_log_end(&ev);
        }
    }

    xSemaphoreTake(s_mot_mtx, portMAX_DELAY);
    if (k == MOTION_EV_START) {
        if (s_mev_n < LD_MOTION_EVENTS) {
            s_mev_n++;
        }
        memmove(&s_mev[1], &s_mev[0], (size_t)(s_mev_n - 1) * sizeof(s_mev[0]));
        s_mev[0].no = ev.event_no;
        s_mev[0].onset_us = ev.onset_us;
        s_mev[0].active = 1;
        s_mev[0].dur_ms = 0;
        s_mev[0].energy = ev.max_energy;
        s_mev[0].dist_cm = ev.dist_cm;
        s_mev[0].min_cm = ev.min_dist_cm;
        s_mev[0].max_cm = ev.max_dist_cm;
    } else if (k == MOTION_EV_END) {
        for (int i = 0; i < s_mev_n; i++) {
            if (s_mev[i].no == ev.event_no) {
                s_mev[i].active = 0;
                s_mev[i].dur_ms = ev.duration_ms;
                s_mev[i].energy = ev.max_energy;
                s_mev[i].dist_cm = ev.dist_cm;
                s_mev[i].min_cm = ev.min_dist_cm;
                s_mev[i].max_cm = ev.max_dist_cm;
                break;
            }
        }
    }
    s_mstate.active = motion_det_active(&s_det);
    s_mstate.last_no = s_det.event_no;
    s_mstate.dist_cm = (d != NULL && (d->target_state & 1u)) ? (int)d->moving_dist_cm : -1;
    xSemaphoreGive(s_mot_mtx);
}

static void on_data(const ld_frame_t *f, ld_ctx_t *ctx)
{
    ld_data_t d;
    if (ld_frame_decode(f->payload, f->payload_len, &d) != 0) {
        ESP_LOGW(TAG, "bad data frame (%u bytes)", (unsigned)f->raw_len);
        return;
    }
    /* Stamped when the parser completes the frame, not once per UART read. */
    uint64_t now = (uint64_t)esp_timer_get_time();
    ctx->frame_no++;
    /* The frames show the real mode: a module that reset itself is back in normal mode. */
    s_engineering = d.engineering != 0;
    if (d.engineering) {
        ctx->normal_since = 0;
    } else if (ctx->normal_since == 0) {
        ctx->normal_since = (int64_t)now;
    }
    uint32_t seq = 0;
    int rc;

    xSemaphoreTake(s_ring_mtx, portMAX_DELAY);
    rc = rb_push(&s_ring, now, REC_TYPE_LD2410_FRAME, f->raw, f->raw_len, &seq);
    xSemaphoreGive(s_ring_mtx);
    if (rc != 0) {
        ESP_LOGW(TAG, "ring push failed (%d)", rc);
    } else {
        xSemaphoreTake(s_snap_mtx, portMAX_DELAY);
        s_snap.valid = true;
        s_snap.data = d;
        s_snap.seq = seq;
        s_snap.frame_no = ctx->frame_no;
        s_snap.time_us = now;
        xSemaphoreGive(s_snap_mtx);
        ctx->frames_window++;
    }
    /* After the frame's own record, so the ring order is frame, then its motion event.
     * No lock is held here; motion_step takes s_mot_mtx and s_ring_mtx one at a time. */
    motion_step(ctx, &d, now);
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
            ctx->ack_len = f->payload_len < sizeof(ctx->ack_payload) ? f->payload_len
                                                                     : sizeof(ctx->ack_payload);
            memcpy(ctx->ack_payload, f->payload, ctx->ack_len);
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
    ctx->ack_len = 0;
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

/* ---- Motion configuration in NVS ----
 * One blob (MOTION_NVS_KEY) of MOTION_NVS_LEN bytes in a fixed little-endian layout with a
 * version byte: a single nvs_set_blob is one atomic NVS entry, so a power loss never leaves a
 * mix of old and new fields (separate keys could), and the layout does not depend on struct
 * padding. A blob that is absent, has the wrong length/version or fails motion_cfg_check
 * falls back to the defaults. Writes happen only in ld2410_motion_set_cfg (HTTP context). */
static void motion_cfg_pack(const motion_cfg_t *c, uint8_t b[MOTION_NVS_LEN])
{
    b[0] = MOTION_NVS_VER;
    b[1] = c->en;
    b[2] = (uint8_t)c->dmin_cm;
    b[3] = (uint8_t)(c->dmin_cm >> 8);
    b[4] = (uint8_t)c->dmax_cm;
    b[5] = (uint8_t)(c->dmax_cm >> 8);
    b[6] = c->emin;
    for (int i = 0; i < 4; i++) {
        b[7 + i] = (uint8_t)(c->tstart_ms >> (8 * i));
        b[11 + i] = (uint8_t)(c->tend_ms >> (8 * i));
    }
}

static int motion_cfg_unpack(const uint8_t b[MOTION_NVS_LEN], motion_cfg_t *c)
{
    if (b[0] != MOTION_NVS_VER) {
        return -1;
    }
    c->en = b[1];
    c->dmin_cm = (uint16_t)(b[2] | (b[3] << 8));
    c->dmax_cm = (uint16_t)(b[4] | (b[5] << 8));
    c->emin = b[6];
    c->tstart_ms = c->tend_ms = 0;
    for (int i = 0; i < 4; i++) {
        c->tstart_ms |= (uint32_t)b[7 + i] << (8 * i);
        c->tend_ms |= (uint32_t)b[11 + i] << (8 * i);
    }
    return motion_cfg_check(c);
}

static void motion_cfg_load(motion_cfg_t *out)
{
    uint8_t b[MOTION_NVS_LEN];
    size_t len = sizeof(b);
    nvs_handle_t h;
    motion_cfg_t c;
    esp_err_t err = nvs_open(MOTION_NVS_NS, NVS_READONLY, &h);
    if (err == ESP_OK) {
        err = nvs_get_blob(h, MOTION_NVS_KEY, b, &len);
        nvs_close(h);
    }
    if (err == ESP_ERR_NVS_NOT_FOUND) { /* namespace or key absent: first boot, not an error */
        motion_cfg_defaults(out);
        return;
    }
    if (err != ESP_OK || len != sizeof(b) || motion_cfg_unpack(b, &c) != 0) {
        ESP_LOGW(TAG, "motion config in NVS unreadable or invalid (%s, %u bytes), using defaults",
                 esp_err_to_name(err), (unsigned)len);
        motion_cfg_defaults(out);
        return;
    }
    *out = c;
}

void ld2410_motion_get_cfg(motion_cfg_t *out)
{
    if (out == NULL) {
        return;
    }
    if (s_mot_mtx == NULL) {
        motion_cfg_defaults(out);
        return;
    }
    xSemaphoreTake(s_mot_mtx, portMAX_DELAY);
    *out = s_mcfg;
    xSemaphoreGive(s_mot_mtx);
}

esp_err_t ld2410_motion_set_cfg(const motion_cfg_t *cfg)
{
    if (cfg == NULL || motion_cfg_check(cfg) != 0) {
        return ESP_ERR_INVALID_ARG;
    }
    if (s_mot_mtx == NULL) {
        return ESP_ERR_INVALID_STATE;
    }
    /* NVS first (may block for flash), without any of our locks: if it fails the running
     * configuration stays as it was. */
    uint8_t b[MOTION_NVS_LEN];
    nvs_handle_t h;
    motion_cfg_pack(cfg, b);
    esp_err_t err = nvs_open(MOTION_NVS_NS, NVS_READWRITE, &h);
    if (err != ESP_OK) {
        return err;
    }
    err = nvs_set_blob(h, MOTION_NVS_KEY, b, sizeof(b));
    if (err == ESP_OK) {
        err = nvs_commit(h);
    }
    nvs_close(h);
    if (err != ESP_OK) {
        return err;
    }
    motion_cfg_t old;
    xSemaphoreTake(s_mot_mtx, portMAX_DELAY);
    old = s_mcfg;
    s_mcfg = *cfg;
    xSemaphoreGive(s_mot_mtx);
    ESP_LOGI(TAG,
             "motion config: en %u -> %u, zone %u..%u cm -> %u..%u cm, energy >= %u -> %u, "
             "start %u -> %u ms, end %u -> %u ms",
             (unsigned)old.en, (unsigned)cfg->en, (unsigned)old.dmin_cm, (unsigned)old.dmax_cm,
             (unsigned)cfg->dmin_cm, (unsigned)cfg->dmax_cm, (unsigned)old.emin,
             (unsigned)cfg->emin, (unsigned)old.tstart_ms, (unsigned)cfg->tstart_ms,
             (unsigned)old.tend_ms, (unsigned)cfg->tend_ms);
    return ESP_OK;
}

int ld2410_motion_events(motion_item_t *out, int max)
{
    if (out == NULL || max <= 0 || s_mot_mtx == NULL) {
        return 0;
    }
    xSemaphoreTake(s_mot_mtx, portMAX_DELAY);
    int n = s_mev_n < max ? s_mev_n : max;
    memcpy(out, s_mev, (size_t)n * sizeof(out[0]));
    xSemaphoreGive(s_mot_mtx);
    return n;
}

void ld2410_motion_state(int *en, int *active, uint32_t *last_no, int *dist_cm_or_neg)
{
    int e = 0, a = 0, d = -1;
    uint32_t no = 0;
    if (s_mot_mtx != NULL) {
        xSemaphoreTake(s_mot_mtx, portMAX_DELAY);
        e = s_mcfg.en;
        a = s_mstate.active;
        no = s_mstate.last_no;
        d = s_mstate.dist_cm;
        xSemaphoreGive(s_mot_mtx);
    }
    if (en) *en = e;
    if (active) *active = a;
    if (last_no) *last_no = no;
    if (dist_cm_or_neg) *dist_cm_or_neg = d;
}

/* ---- Configuration requests ----
 *
 * Lifetime scheme. Everything a request touches after ld2410_request() was entered lives in
 * static storage, never on a caller's stack:
 *   - s_req_mtx serialises callers, so there is one request at a time.
 *   - The caller copies its ld_req_t into the static slot s_slot, marks s_busy and posts a
 *     one-byte token on the depth-1 queue s_req_q. The task owns s_slot from then until it
 *     clears s_busy; the caller does not touch it in between.
 *   - The task runs the request on s_slot, then (under s_st_mtx) clears s_busy and gives the
 *     binary semaphore s_done, unless the caller has abandoned the request meanwhile.
 *   - A caller that times out takes s_st_mtx: if the task finished in that instant it takes
 *     the result after all; otherwise it sets s_abandoned and returns ESP_ERR_TIMEOUT without
 *     copying anything out. The task then still finishes writing s_slot (static, harmless),
 *     clears s_busy and does not give s_done, so no stale completion can reach a later caller.
 *   - While s_busy is set (an abandoned request still running) new callers get
 *     ESP_ERR_INVALID_STATE instead of overwriting the slot. */
static QueueHandle_t s_req_q;
static SemaphoreHandle_t s_req_mtx; /* serialises ld2410_request callers */
static SemaphoreHandle_t s_st_mtx;  /* guards s_busy / s_abandoned */
static SemaphoreHandle_t s_done;    /* binary: task -> waiting caller */
static ld_req_t s_slot;
static bool s_busy;
static bool s_abandoned;

esp_err_t ld2410_request(ld_req_t *req, uint32_t timeout_ms)
{
    if (req == NULL || req->kind < LD_REQ_READ || req->kind > LD_REQ_FACTORY) {
        return ESP_ERR_INVALID_ARG;
    }
    if (s_req_q == NULL) {
        return ESP_ERR_INVALID_STATE;
    }
    /* One deadline for the whole call: the wait for s_req_mtx and the wait for completion
     * share timeout_ms. */
    const TickType_t wait = pdMS_TO_TICKS(timeout_ms);
    const TickType_t t0 = xTaskGetTickCount();
    if (xSemaphoreTake(s_req_mtx, wait) != pdTRUE) {
        return ESP_ERR_TIMEOUT;
    }
    const TickType_t used = xTaskGetTickCount() - t0; /* unsigned: tick wrap-around is fine */
    if (used >= wait) { /* the mutex took the whole budget: nothing is submitted */
        xSemaphoreGive(s_req_mtx);
        return ESP_ERR_TIMEOUT;
    }
    const TickType_t remaining = wait - used;
    esp_err_t err;

    xSemaphoreTake(s_st_mtx, portMAX_DELAY);
    if (s_busy) {
        xSemaphoreGive(s_st_mtx);
        xSemaphoreGive(s_req_mtx);
        return ESP_ERR_INVALID_STATE;
    }
    s_abandoned = false;
    (void)xSemaphoreTake(s_done, 0); /* belt and braces: no stale completion */
    s_slot = *req;
    s_slot.result = LD_RES_OK;
    s_slot.failed_cmd[0] = '\0';
    s_slot.have_version = s_slot.have_before = s_slot.have_after = false;
    s_busy = true;
    xSemaphoreGive(s_st_mtx);

    uint8_t token = 1;
    if (xQueueSend(s_req_q, &token, 0) != pdTRUE) {
        xSemaphoreTake(s_st_mtx, portMAX_DELAY);
        s_busy = false;
        xSemaphoreGive(s_st_mtx);
        xSemaphoreGive(s_req_mtx);
        return ESP_ERR_NO_MEM;
    }

    bool completed = xSemaphoreTake(s_done, remaining) == pdTRUE;
    if (!completed) {
        xSemaphoreTake(s_st_mtx, portMAX_DELAY);
        if (s_busy) {
            s_abandoned = true;
        } else {
            completed = xSemaphoreTake(s_done, 0) == pdTRUE; /* finished just now */
        }
        xSemaphoreGive(s_st_mtx);
    }
    if (completed) {
        *req = s_slot; /* the task is done with the slot, and no caller can start before we unlock */
        err = ESP_OK;
    } else {
        err = ESP_ERR_TIMEOUT;
    }
    xSemaphoreGive(s_req_mtx);
    return err;
}

static void fail_cmd(ld_req_t *r, const char *name)
{
    r->result = LD_RES_CMD_FAILED;
    snprintf(r->failed_cmd, sizeof(r->failed_cmd), "%s", name);
}

/* Send an already encoded command (n = encoder result) and wait for its ACK. */
static bool do_cmd(ld_ctx_t *ctx, ld_req_t *r, uint16_t cmd, int n, const uint8_t *buf,
                   const char *name)
{
    if (n <= 0 || !send_cmd(ctx, cmd, buf, (size_t)n, name)) {
        fail_cmd(r, name);
        return false;
    }
    return true;
}

static bool read_params(ld_ctx_t *ctx, ld_req_t *r, ld_settings_t *out)
{
    uint8_t buf[LD_CMD_MAX_FRAME];
    ld_params_t p;
    if (!do_cmd(ctx, r, LD_CMD_READ_PARAMS, ld_cmd_encode_read_params(buf, sizeof(buf)), buf,
                "read-params")) {
        return false;
    }
    if (ld_cmd_decode_params(ctx->ack_payload, ctx->ack_len, &p) != 0 ||
        ld_settings_from_params(&p, out) != 0) {
        fail_cmd(r, "read-params (bad ACK)");
        return false;
    }
    return true;
}

/* One line per changed value, old -> new. */
static void log_changes(const ld_settings_t *o, const ld_settings_t *n)
{
    if (o->max_move_gate != n->max_move_gate) {
        ESP_LOGI(TAG, "setting: max moving gate %u -> %u", (unsigned)o->max_move_gate,
                 (unsigned)n->max_move_gate);
    }
    if (o->max_still_gate != n->max_still_gate) {
        ESP_LOGI(TAG, "setting: max still gate %u -> %u", (unsigned)o->max_still_gate,
                 (unsigned)n->max_still_gate);
    }
    if (o->duration_s != n->duration_s) {
        ESP_LOGI(TAG, "setting: no-one duration %u s -> %u s", (unsigned)o->duration_s,
                 (unsigned)n->duration_s);
    }
    for (unsigned g = 0; g < LD_GATE_COUNT; g++) {
        if (o->move_sens[g] != n->move_sens[g]) {
            ESP_LOGI(TAG, "setting: gate %u moving sensitivity %u -> %u", g,
                     (unsigned)o->move_sens[g], (unsigned)n->move_sens[g]);
        }
        if (g >= 2 && o->still_sens[g] != n->still_sens[g]) {
            ESP_LOGI(TAG, "setting: gate %u still sensitivity %u -> %u", g,
                     (unsigned)o->still_sens[g], (unsigned)n->still_sens[g]);
        }
    }
}

static void exec_write(ld_ctx_t *ctx, ld_req_t *r)
{
    uint8_t buf[LD_CMD_MAX_FRAME];
    ld_diff_t d;
    if (!read_params(ctx, r, &r->before)) {
        return;
    }
    r->have_before = true;
    if (ld_settings_diff(&r->before, &r->target, &d) != 0) {
        r->result = LD_RES_BAD_ARG;
        return;
    }
    if (d.need_gates &&
        !do_cmd(ctx, r, LD_CMD_SET_GATES,
                ld_cmd_encode_set_gates(r->target.max_move_gate, r->target.max_still_gate,
                                        r->target.duration_s, buf, sizeof(buf)),
                buf, "set-gates")) {
        return;
    }
    for (unsigned i = 0; i < d.n_sens; i++) {
        char name[LD_REQ_NAME_MAX];
        snprintf(name, sizeof(name), "set-sens gate %u", (unsigned)d.sens[i].gate);
        if (!do_cmd(ctx, r, LD_CMD_SET_SENS,
                    ld_cmd_encode_set_sens(d.sens[i].gate, d.sens[i].move, d.sens[i].still, buf,
                                           sizeof(buf)),
                    buf, name)) {
            return;
        }
    }
    if (!read_params(ctx, r, &r->after)) {
        return;
    }
    r->have_after = true;
    log_changes(&r->before, &r->after);
    if (ld_settings_diff(&r->after, &r->target, &d) != 0 || d.need_gates || d.n_sens != 0) {
        ESP_LOGW(TAG, "write: read-back differs from the requested settings");
        r->result = LD_RES_VERIFY;
    }
}

/* Runs the commands between enable-config and end-config. Sets *restarted when a restart
 * ACK was received (the module is rebooting and must not get end-config). */
static void exec_in_config(ld_ctx_t *ctx, ld_req_t *r, bool *restarted)
{
    uint8_t buf[LD_CMD_MAX_FRAME];
    switch (r->kind) {
    case LD_REQ_READ: {
        ld_version_t v;
        if (do_cmd(ctx, r, LD_CMD_READ_VERSION, ld_cmd_encode_read_version(buf, sizeof(buf)), buf,
                   "read-version") &&
            ld_cmd_decode_version(ctx->ack_payload, ctx->ack_len, &v) == 0) {
            r->version = v;
            r->have_version = true;
        } else {
            /* The version is informational: a module without it can still be configured. */
            r->result = LD_RES_OK;
            r->failed_cmd[0] = '\0';
            ESP_LOGW(TAG, "firmware version unavailable");
        }
        if (read_params(ctx, r, &r->before)) {
            r->have_before = true;
        }
        break;
    }
    case LD_REQ_WRITE:
        exec_write(ctx, r);
        break;
    case LD_REQ_BT_OFF:
        ESP_LOGI(TAG, "action: Bluetooth off, then restart");
        if (!do_cmd(ctx, r, LD_CMD_BLUETOOTH, ld_cmd_encode_bluetooth_off(buf, sizeof(buf)), buf,
                    "bluetooth-off")) {
            break;
        }
        if (do_cmd(ctx, r, LD_CMD_RESTART, ld_cmd_encode_restart(buf, sizeof(buf)), buf,
                   "restart")) {
            *restarted = true;
        }
        break;
    case LD_REQ_RESTART:
        ESP_LOGI(TAG, "action: restart module");
        if (do_cmd(ctx, r, LD_CMD_RESTART, ld_cmd_encode_restart(buf, sizeof(buf)), buf,
                   "restart")) {
            *restarted = true;
        }
        break;
    case LD_REQ_FACTORY:
        ESP_LOGI(TAG, "action: factory reset, then restart");
        if (!do_cmd(ctx, r, LD_CMD_FACTORY_RESET, ld_cmd_encode_factory_reset(buf, sizeof(buf)),
                    buf, "factory-reset")) {
            break;
        }
        if (do_cmd(ctx, r, LD_CMD_RESTART, ld_cmd_encode_restart(buf, sizeof(buf)), buf,
                   "restart")) {
            *restarted = true;
        }
        break;
    }
}

/* Executes s_slot. Returns true if the module was restarted. */
static bool run_request(ld_ctx_t *ctx)
{
    ld_req_t *r = &s_slot;
    uint8_t buf[LD_CMD_MAX_FRAME];
    bool restarted = false;

    if (r->kind == LD_REQ_WRITE && ld_settings_check(&r->target) != 0) {
        r->result = LD_RES_BAD_ARG;
        return false;
    }
    if (do_cmd(ctx, r, LD_CMD_ENABLE_CONFIG, ld_cmd_encode_enable_config(buf, sizeof(buf)), buf,
               "enable-config")) {
        exec_in_config(ctx, r, &restarted);
    }
    if (!restarted) {
        /* Always leave configuration mode, also after a failure. */
        ld_res_t res = r->result;
        char name[LD_REQ_NAME_MAX];
        memcpy(name, r->failed_cmd, sizeof(name));
        if (!do_cmd(ctx, r, LD_CMD_END_CONFIG, ld_cmd_encode_end_config(buf, sizeof(buf)), buf,
                    "end-config") &&
            res != LD_RES_OK) {
            /* keep the first failure */
            r->result = res;
            memcpy(r->failed_cmd, name, sizeof(name));
        }
    } else {
        /* Normal-mode frames will follow: let the engineering-mode recovery take over. */
        ctx->normal_since = 0;
        s_engineering = false;
    }
    if (r->result != LD_RES_OK) {
        ESP_LOGW(TAG, "request %d failed: result %d, command '%s'", (int)r->kind, (int)r->result,
                 r->failed_cmd);
    }
    return restarted;
}

static void ld2410_task(void *arg)
{
    (void)arg;
    ld_ctx_t ctx;
    memset(&ctx, 0, sizeof(ctx));
    ld_parser_init(&s_parser);
    motion_det_init(&s_det);

    enable_engineering(&ctx);
    ctx.last_motion_step_us = esp_timer_get_time(); /* the first silent tick is 1 s from here */

    int64_t window_start = esp_timer_get_time();
    int64_t last_eng_try = window_start;
    unsigned eng_attempts = 0;
    for (;;) {
        pump(&ctx, pdMS_TO_TICKS(100));
        int64_t now = esp_timer_get_time();
        /* Zero-wait poll: no cost for the frame path while nothing is pending. */
        uint8_t token;
        if (xQueueReceive(s_req_q, &token, 0) == pdTRUE) {
            bool restarted = run_request(&ctx);
            xSemaphoreTake(s_st_mtx, portMAX_DELAY);
            s_busy = false;
            if (!s_abandoned) {
                xSemaphoreGive(s_done);
            }
            xSemaphoreGive(s_st_mtx);
            if (restarted) {
                /* Not an error storm: just allow the recovery as soon as normal-mode
                 * frames have shown up for LD_ENG_NORMAL_US. */
                last_eng_try = esp_timer_get_time() - LD_ENG_RETRY_US;
            }
            now = esp_timer_get_time();
        }
        /* No data frame for LD_MOTION_TICK_US: tell the detector (counts as a non-moving frame),
         * so an ACTIVE event ends when the link is lost. Frames call motion_step() themselves
         * (also inside send_cmd's pump), so this only fires in silence. */
        if (now - ctx.last_motion_step_us >= LD_MOTION_TICK_US) {
            motion_step(&ctx, NULL, (uint64_t)now);
        }
        /* Normal-mode frames for more than LD_ENG_NORMAL_US: (re-)enable engineering
         * mode, rate limited, unlimited over time. Data frames keep being parsed
         * inside send_cmd()'s pump loop, so reception is not disturbed. */
        if (ctx.normal_since != 0 && now - ctx.normal_since >= LD_ENG_NORMAL_US &&
            now - last_eng_try >= LD_ENG_RETRY_US) {
            eng_attempts++;
            ESP_LOGW(TAG, "normal-mode frames for %lld ms, re-enabling engineering mode (attempt %u)",
                     (long long)((now - ctx.normal_since) / 1000), eng_attempts);
            enable_engineering(&ctx);
            last_eng_try = esp_timer_get_time();
            now = last_eng_try;
            /* Judge the result by the frames that follow, not by the ACK alone. */
            ctx.normal_since = 0;
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
    s_req_mtx = xSemaphoreCreateMutex();
    s_st_mtx = xSemaphoreCreateMutex();
    s_done = xSemaphoreCreateBinary();
    s_mot_mtx = xSemaphoreCreateMutex();
    s_req_q = xQueueCreate(1, sizeof(uint8_t));
    if (s_ring_mtx == NULL || s_snap_mtx == NULL || s_req_mtx == NULL || s_st_mtx == NULL ||
        s_done == NULL || s_mot_mtx == NULL || s_req_q == NULL) {
        s_req_q = NULL; /* ld2410_request() then reports ESP_ERR_INVALID_STATE */
        return ESP_ERR_NO_MEM;
    }
    motion_cfg_load(&s_mcfg); /* before the task exists: no lock needed */
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

    BaseType_t ok = xTaskCreatePinnedToCore(ld2410_task, "ld2410", 6144, NULL, 5,
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

int ld2410_ring_push(uint64_t esp_time_us, uint8_t type, const uint8_t *payload,
                     size_t len, uint32_t *seq_out)
{
    if (s_ring_mtx == NULL) {
        return -ENODEV;
    }
    ld2410_ring_lock();
    int rc = rb_push(&s_ring, esp_time_us, type, payload, len, seq_out);
    ld2410_ring_unlock();
    return rc;
}

int ld2410_ring_batch(rec_stream_t *st, uint32_t from_seq, uint32_t boot_id,
                      uint8_t *buf, size_t cap, size_t *out_len, uint16_t *count)
{
    ld2410_ring_lock();
    int rc = rec_stream_batch(&s_ring, st, from_seq, boot_id, buf, cap, out_len, count);
    ld2410_ring_unlock();
    return rc;
}
