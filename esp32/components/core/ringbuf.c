#include "ringbuf.h"
#include <errno.h>
#include <string.h>

static void put_le(uint8_t *p, uint64_t v, unsigned bytes)
{
    for (unsigned i = 0; i < bytes; i++)
        p[i] = (uint8_t)(v >> (8 * i));
}

static uint64_t get_le(const uint8_t *p, unsigned bytes)
{
    uint64_t v = 0;
    for (unsigned i = 0; i < bytes; i++)
        v |= (uint64_t)p[i] << (8 * i);
    return v;
}

static void copy_in(rb_t *rb, size_t off, const uint8_t *src, size_t n)
{
    size_t first = rb->cap - off;
    if (first > n)
        first = n;
    memcpy(rb->mem + off, src, first);
    if (n > first)
        memcpy(rb->mem, src + first, n - first);
}

static void copy_out(const rb_t *rb, size_t off, uint8_t *dst, size_t n)
{
    size_t first = rb->cap - off;
    if (first > n)
        first = n;
    memcpy(dst, rb->mem + off, first);
    if (n > first)
        memcpy(dst + first, rb->mem, n - first);
}

static size_t rec_len_at(const rb_t *rb, size_t off)
{
    uint8_t h[2];
    copy_out(rb, (off + 12) % rb->cap, h, 1);
    copy_out(rb, (off + 13) % rb->cap, h + 1, 1);
    return (size_t)get_le(h, 2);
}

void rb_init(rb_t *rb, void *mem, size_t cap, uint32_t first_seq)
{
    rb->mem = mem;
    rb->cap = cap;
    rb->head = 0;
    rb->used = 0;
    rb->oldest_seq = first_seq;
    rb->next_seq = first_seq;
    rb->count = 0;
}

void rb_clear(rb_t *rb)
{
    rb->head = 0;
    rb->used = 0;
    rb->oldest_seq = rb->next_seq;
    rb->count = 0;
}

static void evict_oldest(rb_t *rb)
{
    size_t total = RB_REC_HDR + rec_len_at(rb, rb->head);
    rb->head = (rb->head + total) % rb->cap;
    rb->used -= total;
    rb->oldest_seq++;
    rb->count--;
}

int rb_push(rb_t *rb, uint64_t esp_time_us, const uint8_t *raw, size_t len,
            uint32_t *seq_out)
{
    if (!rb || !rb->mem || (len > 0 && !raw))
        return -EINVAL;
    size_t need = RB_REC_HDR + len;
    if (len > RB_MAX_RAW || need > rb->cap)
        return -EMSGSIZE;
    while (rb->cap - rb->used < need)
        evict_oldest(rb); /* count > 0 here: need <= cap */
    uint8_t hdr[RB_REC_HDR];
    put_le(hdr, rb->next_seq, 4);
    put_le(hdr + 4, esp_time_us, 8);
    put_le(hdr + 12, len, 2);
    size_t off = (rb->head + rb->used) % rb->cap;
    copy_in(rb, off, hdr, RB_REC_HDR);
    if (len)
        copy_in(rb, (off + RB_REC_HDR) % rb->cap, raw, len);
    rb->used += need;
    if (seq_out)
        *seq_out = rb->next_seq;
    rb->next_seq++;
    rb->count++;
    return 0;
}

uint32_t rb_count(const rb_t *rb) { return rb->count; }
uint32_t rb_next_seq(const rb_t *rb) { return rb->next_seq; }

int rb_oldest_seq(const rb_t *rb, uint32_t *seq)
{
    if (rb->count == 0)
        return -ENODATA;
    *seq = rb->oldest_seq;
    return 0;
}

int rb_newest_seq(const rb_t *rb, uint32_t *seq)
{
    if (rb->count == 0)
        return -ENODATA;
    *seq = rb->next_seq - 1u;
    return 0;
}

int rb_cursor_open(const rb_t *rb, uint32_t from_seq, rb_cursor_t *cur, int *gap)
{
    if (!rb || !cur || !gap)
        return -EINVAL;
    uint32_t d = from_seq - rb->oldest_seq; /* wrap-safe distance from oldest */
    uint32_t target;
    if (d <= rb->count) {
        target = from_seq;
        *gap = 0;
    } else {
        /* d >= 2^31 means older than oldest (evicted); otherwise it is past
         * next_seq (producer restarted). Either way restart at the oldest. */
        d = 0;
        target = rb->oldest_seq;
        *gap = 1;
    }
    size_t off = rb->head;
    for (uint32_t i = 0; i < d; i++)
        off = (off + RB_REC_HDR + rec_len_at(rb, off)) % rb->cap;
    cur->off = off;
    cur->seq = target;
    return 0;
}

/* Position check shared by peek/advance. */
static int cursor_state(const rb_t *rb, const rb_cursor_t *cur)
{
    uint32_t d = cur->seq - rb->oldest_seq;
    if (d > rb->count)
        return -ESTALE;
    if (d == rb->count)
        return -ENOENT;
    return 0;
}

int rb_cursor_peek(const rb_t *rb, const rb_cursor_t *cur, uint32_t *seq,
                   uint64_t *esp_time_us, uint8_t *out, size_t cap, size_t *len)
{
    if (!rb || !cur || !seq || !esp_time_us || !len || (cap > 0 && !out))
        return -EINVAL;
    int st = cursor_state(rb, cur);
    if (st)
        return st;
    uint8_t hdr[RB_REC_HDR];
    copy_out(rb, cur->off, hdr, RB_REC_HDR);
    size_t n = (size_t)get_le(hdr + 12, 2);
    *len = n;
    if (n > cap)
        return -EMSGSIZE;
    *seq = (uint32_t)get_le(hdr, 4);
    *esp_time_us = get_le(hdr + 4, 8);
    if (n)
        copy_out(rb, (cur->off + RB_REC_HDR) % rb->cap, out, n);
    return 0;
}

int rb_cursor_advance(const rb_t *rb, rb_cursor_t *cur)
{
    if (!rb || !cur)
        return -EINVAL;
    int st = cursor_state(rb, cur);
    if (st)
        return st;
    cur->off = (cur->off + RB_REC_HDR + rec_len_at(rb, cur->off)) % rb->cap;
    cur->seq++;
    return 0;
}
