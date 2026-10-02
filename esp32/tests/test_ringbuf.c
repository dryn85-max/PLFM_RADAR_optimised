/* Host tests for ringbuf: wrap, eviction, gap detection, seq wrap, model check. */
#include <errno.h>
#include <string.h>
#include "tinytest.h"
#include "ringbuf.h"

static uint8_t mem[512];

static void fill(uint8_t *b, size_t n, uint32_t tag)
{
    for (size_t i = 0; i < n; i++) b[i] = (uint8_t)(tag * 7 + i);
}

static int push_tag(rb_t *rb, uint32_t tag, size_t len)
{
    uint8_t raw[RB_MAX_RAW];
    fill(raw, len, tag);
    return rb_push(rb, 1000u + tag, raw, len, NULL);
}

static void test_empty_and_basic(void)
{
    rb_t rb; uint32_t s;
    rb_init(&rb, mem, sizeof mem, 0);
    TT_ASSERT_EQ(0, rb_count(&rb));
    TT_ASSERT_EQ(-ENODATA, rb_oldest_seq(&rb, &s));
    TT_ASSERT_EQ(-ENODATA, rb_newest_seq(&rb, &s));
    rb_cursor_t c; int gap = 9;
    TT_ASSERT_EQ(0, rb_cursor_open(&rb, 0, &c, &gap));
    TT_ASSERT_EQ(0, gap);
    uint8_t out[RB_MAX_RAW]; size_t len; uint64_t t;
    TT_ASSERT_EQ(-ENOENT, rb_cursor_peek(&rb, &c, &s, &t, out, sizeof out, &len));

    uint32_t seq = 99;
    uint8_t raw[20]; fill(raw, 20, 1);
    TT_ASSERT_EQ(0, rb_push(&rb, 5, raw, 20, &seq));
    TT_ASSERT_EQ(0, seq);
    TT_ASSERT_EQ(0, rb_push(&rb, 6, raw, 0, &seq)); /* zero-length record */
    TT_ASSERT_EQ(1, seq);
    TT_ASSERT_EQ(2, rb_count(&rb));
    TT_ASSERT_EQ(0, rb_oldest_seq(&rb, &s)); TT_ASSERT_EQ(0, s);
    TT_ASSERT_EQ(0, rb_newest_seq(&rb, &s)); TT_ASSERT_EQ(1, s);
    TT_ASSERT_EQ(2, rb_next_seq(&rb));

    TT_ASSERT_EQ(0, rb_cursor_open(&rb, 0, &c, &gap));
    TT_ASSERT_EQ(0, rb_cursor_peek(&rb, &c, &s, &t, out, sizeof out, &len));
    TT_ASSERT_EQ(0, s); TT_ASSERT_EQ(5, t); TT_ASSERT_EQ(20, len);
    TT_ASSERT(memcmp(out, raw, 20) == 0);
    TT_ASSERT_EQ(0, rb_cursor_advance(&rb, &c));
    TT_ASSERT_EQ(0, rb_cursor_peek(&rb, &c, &s, &t, out, sizeof out, &len));
    TT_ASSERT_EQ(1, s); TT_ASSERT_EQ(0, len);
    TT_ASSERT_EQ(0, rb_cursor_advance(&rb, &c));
    TT_ASSERT_EQ(-ENOENT, rb_cursor_peek(&rb, &c, &s, &t, out, sizeof out, &len));
    TT_ASSERT_EQ(-ENOENT, rb_cursor_advance(&rb, &c));
    /* too-small output buffer reports the needed size and does not advance */
    TT_ASSERT_EQ(0, rb_cursor_open(&rb, 0, &c, &gap));
    TT_ASSERT_EQ(-EMSGSIZE, rb_cursor_peek(&rb, &c, &s, &t, out, 19, &len));
    TT_ASSERT_EQ(20, len);
    TT_ASSERT_EQ(0, rb_cursor_peek(&rb, &c, &s, &t, out, 20, &len));
}

static void test_invalid_pushes(void)
{
    rb_t rb; uint8_t raw[RB_MAX_RAW + 1] = {0};
    rb_init(&rb, mem, sizeof mem, 0);
    TT_ASSERT_EQ(-EMSGSIZE, rb_push(&rb, 0, raw, RB_MAX_RAW + 1, NULL));
    TT_ASSERT_EQ(-EINVAL, rb_push(&rb, 0, NULL, 4, NULL));
    TT_ASSERT_EQ(-EINVAL, rb_push(NULL, 0, raw, 4, NULL));
    TT_ASSERT_EQ(0, rb_count(&rb));
    uint8_t small[30]; rb_t sm;
    rb_init(&sm, small, sizeof small, 0);
    TT_ASSERT_EQ(-EMSGSIZE, rb_push(&sm, 0, raw, 17, NULL)); /* 14 + 17 > 30 */
    TT_ASSERT_EQ(0, rb_push(&sm, 0, raw, 16, NULL));          /* exactly fits */
    TT_ASSERT_EQ(0, rb_push(&sm, 0, raw, 16, NULL));          /* evicts the first */
    TT_ASSERT_EQ(1, rb_count(&sm));
}

static void test_wrap_and_eviction(void)
{
    uint8_t m[100]; rb_t rb; uint32_t s;
    rb_init(&rb, m, sizeof m, 0);
    /* 34-byte records: two fit, third evicts seq 0 and wraps physically */
    for (uint32_t i = 0; i < 7; i++) {
        TT_ASSERT_EQ(0, push_tag(&rb, i, 20));
        TT_ASSERT(rb.used <= rb.cap);
    }
    TT_ASSERT_EQ(2, rb_count(&rb));
    TT_ASSERT_EQ(0, rb_oldest_seq(&rb, &s)); TT_ASSERT_EQ(5, s);
    TT_ASSERT_EQ(0, rb_newest_seq(&rb, &s)); TT_ASSERT_EQ(6, s);
    rb_cursor_t c; int gap; uint8_t out[RB_MAX_RAW], exp[RB_MAX_RAW]; size_t len; uint64_t t;
    TT_ASSERT_EQ(0, rb_cursor_open(&rb, 5, &c, &gap));
    TT_ASSERT_EQ(0, gap);
    for (uint32_t i = 5; i < 7; i++) {
        TT_ASSERT_EQ(0, rb_cursor_peek(&rb, &c, &s, &t, out, sizeof out, &len));
        fill(exp, 20, i);
        TT_ASSERT_EQ(i, s); TT_ASSERT_EQ(1000 + i, t); TT_ASSERT_EQ(20, len);
        TT_ASSERT(memcmp(out, exp, 20) == 0);
        TT_ASSERT_EQ(0, rb_cursor_advance(&rb, &c));
    }
    TT_ASSERT_EQ(-ENOENT, rb_cursor_peek(&rb, &c, &s, &t, out, sizeof out, &len));
    /* evicted seq: starts at oldest with GAP */
    for (uint32_t from = 0; from < 5; from++) {
        TT_ASSERT_EQ(0, rb_cursor_open(&rb, from, &c, &gap));
        TT_ASSERT_EQ(1, gap);
        TT_ASSERT_EQ(0, rb_cursor_peek(&rb, &c, &s, &t, out, sizeof out, &len));
        TT_ASSERT_EQ(5, s);
    }
    /* seq == next: empty, no gap; beyond next: restart => oldest + gap */
    TT_ASSERT_EQ(0, rb_cursor_open(&rb, 7, &c, &gap)); TT_ASSERT_EQ(0, gap);
    TT_ASSERT_EQ(-ENOENT, rb_cursor_peek(&rb, &c, &s, &t, out, sizeof out, &len));
    TT_ASSERT_EQ(0, rb_cursor_open(&rb, 8, &c, &gap)); TT_ASSERT_EQ(1, gap);
    TT_ASSERT_EQ(0, rb_cursor_peek(&rb, &c, &s, &t, out, sizeof out, &len)); TT_ASSERT_EQ(5, s);
    TT_ASSERT_EQ(0, rb_cursor_open(&rb, 0x80000000u, &c, &gap)); TT_ASSERT_EQ(1, gap);
}

static void test_stale_cursor(void)
{
    uint8_t m[100]; rb_t rb; uint32_t s;
    rb_init(&rb, m, sizeof m, 0);
    push_tag(&rb, 0, 20); push_tag(&rb, 1, 20);
    rb_cursor_t c; int gap; uint8_t out[64]; size_t len; uint64_t t;
    TT_ASSERT_EQ(0, rb_cursor_open(&rb, 0, &c, &gap));
    push_tag(&rb, 2, 20); /* evicts seq 0 under the cursor */
    TT_ASSERT_EQ(-ESTALE, rb_cursor_peek(&rb, &c, &s, &t, out, sizeof out, &len));
    TT_ASSERT_EQ(-ESTALE, rb_cursor_advance(&rb, &c));
    TT_ASSERT_EQ(0, rb_cursor_open(&rb, 0, &c, &gap));
    TT_ASSERT_EQ(1, gap);
    TT_ASSERT_EQ(0, rb_cursor_peek(&rb, &c, &s, &t, out, sizeof out, &len));
    TT_ASSERT_EQ(1, s);
}

static void test_seq_wrap(void)
{
    uint8_t m[400]; rb_t rb; uint32_t s, seq;
    rb_init(&rb, m, sizeof m, 0xFFFFFFFEu);
    for (uint32_t i = 0; i < 5; i++) {
        TT_ASSERT_EQ(0, rb_push(&rb, i, (const uint8_t *)"ab", 2, &seq));
        TT_ASSERT_EQ(0xFFFFFFFEu + i, seq);
    }
    TT_ASSERT_EQ(0, rb_oldest_seq(&rb, &s)); TT_ASSERT_EQ(0xFFFFFFFEu, s);
    TT_ASSERT_EQ(0, rb_newest_seq(&rb, &s)); TT_ASSERT_EQ(2, s);
    TT_ASSERT_EQ(3, rb_next_seq(&rb));
    rb_cursor_t c; int gap; uint8_t out[8]; size_t len; uint64_t t;
    /* from_seq just before, at, and after the wrap */
    TT_ASSERT_EQ(0, rb_cursor_open(&rb, 0xFFFFFFFFu, &c, &gap));
    TT_ASSERT_EQ(0, gap);
    TT_ASSERT_EQ(0, rb_cursor_peek(&rb, &c, &s, &t, out, sizeof out, &len));
    TT_ASSERT_EQ(0xFFFFFFFFu, s);
    TT_ASSERT_EQ(0, rb_cursor_advance(&rb, &c));
    TT_ASSERT_EQ(0, rb_cursor_peek(&rb, &c, &s, &t, out, sizeof out, &len));
    TT_ASSERT_EQ(0, s);
    TT_ASSERT_EQ(0, rb_cursor_open(&rb, 1, &c, &gap)); TT_ASSERT_EQ(0, gap);
    TT_ASSERT_EQ(0, rb_cursor_open(&rb, 3, &c, &gap)); TT_ASSERT_EQ(0, gap);
    TT_ASSERT_EQ(-ENOENT, rb_cursor_peek(&rb, &c, &s, &t, out, sizeof out, &len));
    /* a numerically larger but wrap-older seq is evicted, not future */
    TT_ASSERT_EQ(0, rb_cursor_open(&rb, 0xFFFFFFF0u, &c, &gap)); TT_ASSERT_EQ(1, gap);
    TT_ASSERT_EQ(0, rb_cursor_peek(&rb, &c, &s, &t, out, sizeof out, &len));
    TT_ASSERT_EQ(0xFFFFFFFEu, s);
    /* eviction across the wrap moves oldest correctly */
    uint8_t m2[100]; rb_t r2;
    rb_init(&r2, m2, sizeof m2, 0xFFFFFFFFu);
    for (uint32_t i = 0; i < 4; i++) push_tag(&r2, i, 20);
    TT_ASSERT_EQ(0, rb_oldest_seq(&r2, &s)); TT_ASSERT_EQ(1, s);
    TT_ASSERT_EQ(0, rb_cursor_open(&r2, 0xFFFFFFFFu, &c, &gap)); TT_ASSERT_EQ(1, gap);
}

/* Randomised model check: ring contents == last N pushed records (data, time, seq). */
typedef struct { uint32_t seq; uint32_t idx; uint16_t len; } mrec_t;
static void test_random_model(void)
{
    static uint8_t m[700];
    static mrec_t model[20000];
    rb_t rb;
    rb_init(&rb, m, sizeof m, 0xFFFFFF00u);
    uint32_t rnd = 7;
    size_t nm = 0;
    for (uint32_t i = 0; i < 20000; i++) {
        rnd = rnd * 1664525u + 1013904223u;
        size_t len = (rnd >> 8) % 90;
        uint32_t seq;
        uint8_t raw[RB_MAX_RAW];
        fill(raw, len, i);
        TT_ASSERT_EQ(0, rb_push(&rb, (uint64_t)i << 33, raw, len, &seq));
        model[nm].seq = seq; model[nm].idx = i; model[nm].len = (uint16_t)len; nm++;
        if (i % 37 != 0) continue;
        rb_cursor_t c; int gap; uint32_t s, oldest; uint64_t t;
        uint8_t out[RB_MAX_RAW], exp[RB_MAX_RAW]; size_t l;
        TT_ASSERT_EQ(0, rb_oldest_seq(&rb, &oldest));
        TT_ASSERT_EQ(0, rb_cursor_open(&rb, oldest, &c, &gap));
        TT_ASSERT_EQ(0, gap);
        size_t k0 = nm - rb_count(&rb);
        TT_ASSERT_EQ(model[k0].seq, oldest);
        for (uint32_t k = 0; k < rb_count(&rb); k++) {
            const mrec_t *r = &model[k0 + k];
            if (rb_cursor_peek(&rb, &c, &s, &t, out, sizeof out, &l) != 0) { TT_ASSERT(0); break; }
            fill(exp, r->len, r->idx);
            TT_ASSERT_EQ(r->seq, s);
            TT_ASSERT_EQ((uint64_t)r->idx << 33, t);
            TT_ASSERT_EQ(r->len, l);
            TT_ASSERT(memcmp(out, exp, l) == 0);
            rb_cursor_advance(&rb, &c);
        }
        TT_ASSERT_EQ(-ENOENT, rb_cursor_peek(&rb, &c, &s, &t, out, sizeof out, &l));
    }
}

int main(void)
{
    TT_RUN(test_empty_and_basic);
    TT_RUN(test_invalid_pushes);
    TT_RUN(test_wrap_and_eviction);
    TT_RUN(test_stale_cursor);
    TT_RUN(test_seq_wrap);
    TT_RUN(test_random_model);
    return TT_RESULT();
}
