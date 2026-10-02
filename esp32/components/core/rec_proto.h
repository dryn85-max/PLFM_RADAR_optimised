/* Recording protocol codec (plain C11), all integers little-endian.
 *   request (client -> device, 12 B): "LDRQ", version u8 = 2, reserved u8[3] = 0, from_seq u32
 *   batch header (device -> client, 20 B): "LDRB", version u8 = 2, flags u8
 *     (bit0 GAP), reserved u16 = 0, first_seq u32, count u16, reserved u16 = 0,
 *     boot_id u32 (random per ESP32 boot, never 0: the sequence restarts at 0
 *     after a reboot, the client uses boot_id to tell)
 *   then count records: seq u32, esp_time_us u64, len u16, raw[len]
 * count = 0 is a keep-alive. Not thread-safe (see rec_batch_from_ring). */
#ifndef REC_PROTO_H
#define REC_PROTO_H

#include <stddef.h>
#include <stdint.h>
#include "ringbuf.h"

#define REC_VERSION 2u
#define REC_REQ_LEN 12u
#define REC_BATCH_HDR_LEN 20u
#define REC_RECORD_HDR_LEN RB_REC_HDR
#define REC_FLAG_GAP 0x01u

int rec_req_encode(uint8_t out[REC_REQ_LEN], uint32_t from_seq);
/* Exactly 12 bytes. 0, or -EINVAL (null), -EBADMSG (length, magic, reserved
 * bytes), -ENOTSUP (version). */
int rec_req_parse(const uint8_t *buf, size_t len, uint32_t *from_seq);

typedef struct {
    uint8_t *buf;
    size_t cap;
    size_t len;
    uint16_t count;
} rec_batch_t;

/* Start a batch (header written with count 0). -EINVAL (null, boot_id 0),
 * -ENOSPC if cap < 20. */
int rec_batch_begin(rec_batch_t *b, uint8_t *buf, size_t cap,
                    uint32_t first_seq, uint8_t flags, uint32_t boot_id);
/* Append a record. -ENOSPC if it does not fit (batch unchanged) or count
 * would exceed 65535; -EINVAL for bad args or len > 65535. */
int rec_batch_add(rec_batch_t *b, uint32_t seq, uint64_t esp_time_us,
                  const uint8_t *raw, size_t len);
/* Patch the count; returns the total encoded length. */
size_t rec_batch_finish(rec_batch_t *b);

typedef struct {
    uint8_t version;
    uint8_t flags;
    uint32_t first_seq;
    uint16_t count;
    uint32_t boot_id;
} rec_batch_hdr_t;
/* 0, -EINVAL, -EBADMSG (short, magic, reserved, boot_id 0), -ENOTSUP (version). */
int rec_batch_parse_header(const uint8_t *buf, size_t len, rec_batch_hdr_t *h);

/* Build one batch from the ring starting at from_seq (GAP flag when it was
 * evicted), as many whole records as fit in cap. count 0 -> keep-alive.
 * Returns 0 with *out_len / *count set; -EMSGSIZE if records exist but not
 * even the first fits (cap too small), -ENOSPC if cap < 20, -EINVAL (null,
 * boot_id 0). Never writes beyond cap. The caller holds the ring lock for the
 * duration. */
int rec_batch_from_ring(const rb_t *rb, uint32_t from_seq, uint32_t boot_id,
                        uint8_t *buf, size_t cap, size_t *out_len, uint16_t *count);

/* Seq the client should request next after this encoded batch: first_seq +
 * count (wrap-safe). Differs from the requested seq after a GAP. 0, -EINVAL,
 * -EBADMSG (shorter than a header, bad header). */
int rec_batch_next_seq(const uint8_t *buf, size_t len, uint32_t *next);

/* True when a batch of len bytes built into cap bytes may have stopped because
 * the buffer was full (one more maximum-size record would not fit), i.e. more
 * records may be pending and the next batch should follow immediately. */
int rec_batch_maybe_truncated(size_t len, size_t cap);

#endif
