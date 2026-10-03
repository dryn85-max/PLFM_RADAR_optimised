/* LD2410C link: UART1 reader task, ring buffer producer, latest snapshot. */
#ifndef LD2410_TASK_H
#define LD2410_TASK_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"
#include "ld2410_cmd.h"
#include "ld2410_frame.h"
#include "ld_settings.h"
#include "motion_det.h"
#include "rec_proto.h"
#include "ringbuf.h"

typedef enum {
    LD_LINK_NO_DATA = 0, /* no data frame since boot */
    LD_LINK_OK = 1,      /* last frame less than 1 s ago */
    LD_LINK_LOST = 2     /* had frames, none for more than 1 s */
} ld_link_t;

typedef struct {
    bool valid;         /* false until the first data frame */
    ld_data_t data;
    uint32_t seq;       /* ring sequence number of this frame */
    uint32_t frame_no;  /* LD2410C data frames decoded since boot (wraps); unlike seq it
                         * does not advance for GPS/IMU/time records */
    uint64_t time_us;   /* esp_timer_get_time() at reception */
} ld_snapshot_t;

/* Start the LD2410C task (core 1). ring_mem/ring_cap: memory for the ring
 * buffer, must stay valid forever. Sequence numbers start at 0. */
esp_err_t ld2410_start(void *ring_mem, size_t ring_cap);

/* Copy the latest decoded frame. Returns false until the first frame. */
bool ld2410_get_snapshot(ld_snapshot_t *out);

ld_link_t ld2410_link_status(void);
/* Whether engineering mode is currently active: acknowledged at boot or by a
 * later retry, and false again while normal-mode frames arrive (e.g. after the
 * module reset itself). */
bool ld2410_engineering_enabled(void);

/* ---- Configuration requests (other tasks -> LD2410C task) ---- */
typedef enum {
    LD_REQ_READ = 0,  /* firmware version + parameters */
    LD_REQ_WRITE,     /* make the module's settings equal to `target` (only the differences are sent) */
    LD_REQ_BT_OFF,    /* 0x00A4 off, then restart 0x00A3 */
    LD_REQ_RESTART,   /* 0x00A3 */
    LD_REQ_FACTORY    /* 0x00A2, then restart 0x00A3 */
} ld_req_kind_t;

typedef enum {
    LD_RES_OK = 0,
    LD_RES_CMD_FAILED, /* a command got no ACK / a bad ACK: see failed_cmd */
    LD_RES_BAD_ARG,    /* WRITE target out of range, nothing was sent */
    LD_RES_VERIFY      /* WRITE: the read-back differs from the target */
} ld_res_t;

#define LD_REQ_NAME_MAX 32

/* Caller-owned request. Inputs: kind, target (WRITE). Outputs (valid when
 * ld2410_request returned ESP_OK; the have_* flags say which are filled):
 * result, failed_cmd, version, before (READ, WRITE), after (WRITE: read-back). */
typedef struct {
    ld_req_kind_t kind;
    ld_settings_t target;
    ld_res_t result;
    char failed_cmd[LD_REQ_NAME_MAX]; /* name of the failing command, internal literal */
    bool have_version;
    ld_version_t version;
    bool have_before;
    ld_settings_t before;
    bool have_after;
    ld_settings_t after;
} ld_req_t;

/* Run a request in the LD2410C task and wait up to timeout_ms in total (callers are
 * serialised; the wait for another caller and the wait for completion share one
 * deadline, and nothing is submitted when the deadline passed while waiting). Returns ESP_OK when the
 * task completed it (look at req->result), ESP_ERR_TIMEOUT, ESP_ERR_INVALID_STATE (not
 * started, or an earlier timed-out request is still running), ESP_ERR_INVALID_ARG,
 * ESP_ERR_NO_MEM. On any non-OK return *req is left untouched. A timed-out request may
 * still be carried out by the task. Data frames pause while it runs (up to about 1 s). */
esp_err_t ld2410_request(ld_req_t *req, uint32_t timeout_ms);

/* ---- Motion detector (runs in the LD2410C task) ---- */
#define LD_MOTION_EVENTS 10

/* One event for the page. active: dur_ms is not final yet (0). energy/dist/min/max are the
 * START values while active (seen during the start delay) and the final ones after the END. */
typedef struct {
    uint32_t no;
    uint64_t onset_us; /* esp_timer us of the first moving frame */
    uint32_t dur_ms;
    int active;
    uint8_t energy;
    uint16_t dist_cm, min_cm, max_cm;
} motion_item_t;

/* Last events, newest first; returns how many were copied (0..max, at most LD_MOTION_EVENTS). */
int ld2410_motion_events(motion_item_t *out, int max);
/* en: detector enabled; active: an event is ACTIVE; last_no: number of the last started event
 * (0 = none); dist_cm_or_neg: moving distance of the latest frame, -1 when it has no moving
 * target or no frame arrived for about a second. Any pointer may be NULL. */
void ld2410_motion_state(int *en, int *active, uint32_t *last_no, int *dist_cm_or_neg);
/* Current configuration copy. */
void ld2410_motion_get_cfg(motion_cfg_t *out);
/* Validate (motion_cfg_check, else ESP_ERR_INVALID_ARG), store in NVS namespace "motion"
 * (may block on flash: call from the HTTP handler, never from a frame path), then hand over to
 * the LD2410C task, which applies it from its next frame/tick. On an NVS error the running
 * configuration is unchanged. ESP_ERR_INVALID_STATE before ld2410_start(). */
esp_err_t ld2410_motion_set_cfg(const motion_cfg_t *cfg);

/* Ring access for readers. Hold the lock only while copying; never block
 * (network I/O, long waits) while holding it. */
void ld2410_ring_lock(void);
void ld2410_ring_unlock(void);
const rb_t *ld2410_ring(void); /* use only between lock and unlock */

/* Producer API for other sensor tasks: lock, rb_push(), unlock (the lock is held
 * only for the copy). Returns rb_push's result, or -ENODEV before ld2410_start()
 * has created the ring. seq_out may be NULL. */
int ld2410_ring_push(uint64_t esp_time_us, uint8_t type, const uint8_t *payload,
                     size_t len, uint32_t *seq_out);

/* Convenience: lock, rec_stream_batch(), unlock. Returns what
 * rec_stream_batch returns. st is the caller's per-connection cursor. */
int ld2410_ring_batch(rec_stream_t *st, uint32_t from_seq, uint32_t boot_id,
                      uint8_t *buf, size_t cap, size_t *out_len, uint16_t *count);

#endif
