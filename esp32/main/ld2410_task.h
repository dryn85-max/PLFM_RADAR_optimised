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

/* Run a request in the LD2410C task and wait up to timeout_ms (callers are serialised;
 * the wait for another caller counts against the same timeout). Returns ESP_OK when the
 * task completed it (look at req->result), ESP_ERR_TIMEOUT, ESP_ERR_INVALID_STATE (not
 * started, or an earlier timed-out request is still running), ESP_ERR_INVALID_ARG,
 * ESP_ERR_NO_MEM. On any non-OK return *req is left untouched. A timed-out request may
 * still be carried out by the task. Data frames pause while it runs (up to about 1 s). */
esp_err_t ld2410_request(ld_req_t *req, uint32_t timeout_ms);

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
