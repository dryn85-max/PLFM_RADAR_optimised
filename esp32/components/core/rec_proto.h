/* Recording protocol codec (plain C11), all integers little-endian.
 *   request (client -> device, 12 B): "LDRQ", version u8 = 1, reserved u8[3] = 0, from_seq u32
 *   batch header (device -> client, 16 B): "LDRB", version u8 = 1, flags u8
 *     (bit0 GAP), reserved u16 = 0, first_seq u32, count u16, reserved u16 = 0
 *   then count records: seq u32, esp_time_us u64, len u16, raw[len]
 * count = 0 is a keep-alive. Not thread-safe (see rec_batch_from_ring). */
#ifndef REC_PROTO_H
#define REC_PROTO_H

#include <stddef.h>
#include <stdint.h>
#include "ringbuf.h"

#define REC_VERSION 1u
#define REC_REQ_LEN 12u
#define REC_BATCH_HDR_LEN 16u
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

/* Start a batch (header written with count 0). -EINVAL, -ENOSPC if cap < 16. */
int rec_batch_begin(rec_batch_t *b, uint8_t *buf, size_t cap,
                    uint32_t first_seq, uint8_t flags);
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
} rec_batch_hdr_t;
/* 0, -EINVAL, -EBADMSG (short, magic, reserved), -ENOTSUP (version). */
int rec_batch_parse_header(const uint8_t *buf, size_t len, rec_batch_hdr_t *h);

/* Build one batch from the ring starting at from_seq (GAP flag when it was
 * evicted), as many whole records as fit in cap. count 0 -> keep-alive.
 * Returns 0 with *out_len / *count set; -EMSGSIZE if records exist but not
 * even the first fits (cap too small), -ENOSPC if cap < 16. Never writes
 * beyond cap. The caller holds the ring lock for the duration. */
int rec_batch_from_ring(const rb_t *rb, uint32_t from_seq, uint8_t *buf,
                        size_t cap, size_t *out_len, uint16_t *count);

#endif
