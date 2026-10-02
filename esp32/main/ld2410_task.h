/* LD2410C link: UART1 reader task, ring buffer producer, latest snapshot. */
#ifndef LD2410_TASK_H
#define LD2410_TASK_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"
#include "ld2410_frame.h"
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
    uint64_t time_us;   /* esp_timer_get_time() at reception */
} ld_snapshot_t;

/* Start the LD2410C task (core 1). ring_mem/ring_cap: memory for the ring
 * buffer, must stay valid forever. Sequence numbers start at 0. */
esp_err_t ld2410_start(void *ring_mem, size_t ring_cap);

/* Copy the latest decoded frame. Returns false until the first frame. */
bool ld2410_get_snapshot(ld_snapshot_t *out);

ld_link_t ld2410_link_status(void);
/* Whether engineering mode was acknowledged (at boot or by a later retry). */
bool ld2410_engineering_enabled(void);

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
