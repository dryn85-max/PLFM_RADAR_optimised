#include "rec_proto.h"
#include <errno.h>
#include <string.h>

static void put16(uint8_t *p, uint32_t v) { p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8); }
static void put32(uint8_t *p, uint32_t v) { put16(p, v & 0xFFFF); put16(p + 2, v >> 16); }
static void put64(uint8_t *p, uint64_t v) { put32(p, (uint32_t)v); put32(p + 4, (uint32_t)(v >> 32)); }
static uint16_t get16(const uint8_t *p) { return (uint16_t)(p[0] | (p[1] << 8)); }
static uint32_t get32(const uint8_t *p) { return get16(p) | ((uint32_t)get16(p + 2) << 16); }

int rec_req_encode(uint8_t out[REC_REQ_LEN], uint32_t from_seq)
{
    if (!out)
        return -EINVAL;
    memcpy(out, "LDRQ", 4);
    out[4] = REC_VERSION;
    out[5] = out[6] = out[7] = 0;
    put32(out + 8, from_seq);
    return 0;
}

int rec_req_parse(const uint8_t *buf, size_t len, uint32_t *from_seq)
{
    if (!buf || !from_seq)
        return -EINVAL;
    if (len != REC_REQ_LEN || memcmp(buf, "LDRQ", 4) != 0 ||
        buf[5] || buf[6] || buf[7])
        return -EBADMSG;
    if (buf[4] != REC_VERSION)
        return -ENOTSUP;
    *from_seq = get32(buf + 8);
    return 0;
}

int rec_batch_begin(rec_batch_t *b, uint8_t *buf, size_t cap,
                    uint32_t first_seq, uint8_t flags)
{
    if (!b || !buf)
        return -EINVAL;
    if (cap < REC_BATCH_HDR_LEN)
        return -ENOSPC;
    memcpy(buf, "LDRB", 4);
    buf[4] = REC_VERSION;
    buf[5] = flags;
    put16(buf + 6, 0);
    put32(buf + 8, first_seq);
    put16(buf + 12, 0);
    put16(buf + 14, 0);
    b->buf = buf;
    b->cap = cap;
    b->len = REC_BATCH_HDR_LEN;
    b->count = 0;
    return 0;
}

int rec_batch_add(rec_batch_t *b, uint32_t seq, uint64_t esp_time_us,
                  const uint8_t *raw, size_t len)
{
    if (!b || !b->buf || (len > 0 && !raw) || len > 0xFFFF)
        return -EINVAL;
    if (b->count == 0xFFFF || b->cap - b->len < REC_RECORD_HDR_LEN + len)
        return -ENOSPC;
    uint8_t *p = b->buf + b->len;
    put32(p, seq);
    put64(p + 4, esp_time_us);
    put16(p + 12, (uint32_t)len);
    if (len)
        memcpy(p + REC_RECORD_HDR_LEN, raw, len);
    b->len += REC_RECORD_HDR_LEN + len;
    b->count++;
    return 0;
}

size_t rec_batch_finish(rec_batch_t *b)
{
    put16(b->buf + 12, b->count);
    return b->len;
}

int rec_batch_parse_header(const uint8_t *buf, size_t len, rec_batch_hdr_t *h)
{
    if (!buf || !h)
        return -EINVAL;
    if (len < REC_BATCH_HDR_LEN || memcmp(buf, "LDRB", 4) != 0 ||
        get16(buf + 6) != 0 || get16(buf + 14) != 0)
        return -EBADMSG;
    if (buf[4] != REC_VERSION)
        return -ENOTSUP;
    h->version = buf[4];
    h->flags = buf[5];
    h->first_seq = get32(buf + 8);
    h->count = get16(buf + 12);
    return 0;
}

int rec_batch_from_ring(const rb_t *rb, uint32_t from_seq, uint8_t *buf,
                        size_t cap, size_t *out_len, uint16_t *count)
{
    if (!rb || !buf || !out_len || !count)
        return -EINVAL;
    rb_cursor_t cur;
    int gap = 0;
    int rc = rb_cursor_open(rb, from_seq, &cur, &gap);
    if (rc)
        return rc;
    rec_batch_t b;
    rc = rec_batch_begin(&b, buf, cap, cur.seq, gap ? REC_FLAG_GAP : 0);
    if (rc)
        return rc;
    uint8_t raw[RB_MAX_RAW];
    for (;;) {
        uint32_t seq;
        uint64_t t;
        size_t len;
        rc = rb_cursor_peek(rb, &cur, &seq, &t, raw, sizeof raw, &len);
        if (rc == -ENOENT)
            break;
        if (rc)
            return rc;
        rc = rec_batch_add(&b, seq, t, raw, len);
        if (rc == -ENOSPC) {
            if (b.count == 0)
                return -EMSGSIZE;
            break;
        }
        if (rc)
            return rc;
        rb_cursor_advance(rb, &cur);
    }
    *out_len = rec_batch_finish(&b);
    *count = b.count;
    return 0;
}
