/* Variable-length record ring buffer with sequence numbers (plain C11).
 *
 * Records {seq u32, esp_time_us u64, type u8, len u16, payload[len]} live in a caller-
 * provided memory block, back to back, wrapping at the end of the block.
 * Sequence numbers are assigned by rb_push, are consecutive, and wrap at
 * 2^32; all comparisons are wrap-safe (valid while fewer than 2^31 records
 * are stored, which the memory size guarantees).
 * Pushing evicts the oldest records as needed.
 * The type byte is opaque to the ring (stored and returned as is); see
 * rec_proto.h for the record types and what receivers do with unknown ones.
 *
 * NOT THREAD-SAFE: the caller must hold a lock around every call that takes
 * the same rb_t (including cursor calls and rec_batch_from_ring). */
#ifndef RINGBUF_H
#define RINGBUF_H

#include <stddef.h>
#include <stdint.h>

#define RB_REC_HDR 15u   /* seq 4 + time 8 + type 1 + len 2 */
#define RB_MAX_RAW 256u  /* largest raw payload per record */

typedef struct {
    uint8_t *mem;
    size_t cap;
    size_t head;  /* offset of oldest record */
    size_t used;  /* bytes in use */
    uint32_t oldest_seq;
    uint32_t next_seq; /* seq the next push gets */
    uint32_t count;
} rb_t;

typedef struct {
    size_t off;
    uint32_t seq;
} rb_cursor_t;

/* mem must stay valid; cap must be >= RB_REC_HDR + RB_MAX_RAW to hold any record. */
void rb_init(rb_t *rb, void *mem, size_t cap, uint32_t first_seq);
void rb_clear(rb_t *rb); /* drop all records, keep numbering */

/* Store a record. Returns 0 (seq in *seq_out if non-null), -EINVAL, or
 * -EMSGSIZE (len > RB_MAX_RAW or record larger than the block). */
int rb_push(rb_t *rb, uint64_t esp_time_us, uint8_t type, const uint8_t *raw,
            size_t len, uint32_t *seq_out);

uint32_t rb_count(const rb_t *rb);
/* -ENODATA when empty. */
int rb_oldest_seq(const rb_t *rb, uint32_t *seq);
int rb_newest_seq(const rb_t *rb, uint32_t *seq);
uint32_t rb_next_seq(const rb_t *rb);

/* Position a cursor at from_seq. If from_seq was evicted (older than the
 * oldest stored) or is beyond the newest + 1 (e.g. the producer restarted),
 * the cursor starts at the oldest stored record and *gap is set to 1,
 * otherwise 0. from_seq == next_seq gives an empty cursor, no gap. */
int rb_cursor_open(const rb_t *rb, uint32_t from_seq, rb_cursor_t *cur, int *gap);

/* Copy the record at the cursor without advancing. Returns 0, -ENOENT (no
 * more records), -ESTALE (the record was evicted since the cursor was
 * opened; reopen, it is a gap), -EMSGSIZE (cap too small, *len = needed). */
int rb_cursor_peek(const rb_t *rb, const rb_cursor_t *cur, uint32_t *seq,
                   uint64_t *esp_time_us, uint8_t *type, uint8_t *out, size_t cap,
                   size_t *len);
/* Move past the record at the cursor. Same errors as peek (except size). */
int rb_cursor_advance(const rb_t *rb, rb_cursor_t *cur);

#endif
