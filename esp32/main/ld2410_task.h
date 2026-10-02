/* LD2410C link: UART1 reader task, ring buffer producer, latest snapshot. */
#ifndef LD2410_TASK_H
#define LD2410_TASK_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"
#include "ld2410_frame.h"
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
/* Whether engineering mode was acknowledged at start. */
bool ld2410_engineering_enabled(void);

/* Ring access for readers. Hold the lock only while copying; never block
 * (network I/O, long waits) while holding it. */
void ld2410_ring_lock(void);
void ld2410_ring_unlock(void);
const rb_t *ld2410_ring(void); /* use only between lock and unlock */

/* Convenience: lock, rec_batch_from_ring(), unlock. Returns what
 * rec_batch_from_ring returns. */
int ld2410_ring_batch(uint32_t from_seq, uint8_t *buf, size_t cap,
                      size_t *out_len, uint16_t *count);

#endif
