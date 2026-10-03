/* Host tests for the UBX framer: checksum, poll encoder, NAV-TIMEUTC decoder,
 * chunk independence, resync, oversized frames. */
#include <string.h>
#include "tinytest.h"
#include "ubx.h"

#define MAXF 8
typedef struct {
    int n;
    uint8_t cls[MAXF], id[MAXF];
    size_t len[MAXF];
    uint8_t pl[MAXF][UBX_MAX_PAYLOAD];
} col_t;

static void on_ubx(uint8_t cls, uint8_t id, const uint8_t *pl, size_t len, void *vctx)
{
    col_t *c = (col_t *)vctx;
    if (c->n < MAXF) {
        c->cls[c->n] = cls;
        c->id[c->n] = id;
        c->len[c->n] = len;
        memcpy(c->pl[c->n], pl, len);
        c->n++;
    }
}

/* Build a frame; returns its length. */
static size_t mkframe(uint8_t *out, uint8_t cls, uint8_t id, const uint8_t *pl, size_t n)
{
    out[0] = 0xB5;
    out[1] = 0x62;
    out[2] = cls;
    out[3] = id;
    out[4] = (uint8_t)(n & 0xFF);
    out[5] = (uint8_t)(n >> 8);
    if (n) {
        memcpy(out + 6, pl, n);
    }
    ubx_checksum(out + 2, 4 + n, &out[6 + n], &out[7 + n]);
    return 8 + n;
}

static void put32(uint8_t *p, uint32_t v)
{
    for (int i = 0; i < 4; i++) {
        p[i] = (uint8_t)(v >> (8 * i));
    }
}

static void mk_timeutc(uint8_t *pl, uint8_t valid)
{
    put32(pl + 0, 345678123u);
    put32(pl + 4, 50u);
    put32(pl + 8, (uint32_t)(int32_t)-123456789);
    pl[12] = 0xEA; /* 2026 = 0x07EA */
    pl[13] = 0x07;
    pl[14] = 10;
    pl[15] = 3;
    pl[16] = 12;
    pl[17] = 34;
    pl[18] = 56;
    pl[19] = valid;
}

static size_t mk_nav(uint8_t *out, uint8_t valid)
{
    uint8_t pl[20];
    mk_timeutc(pl, valid);
    return mkframe(out, 0x01, 0x21, pl, 20);
}

static void test_checksum_and_poll(void)
{
    uint8_t a, b;
    const uint8_t v[] = {0x01, 0x21, 0x00, 0x00};
    ubx_checksum(v, sizeof v, &a, &b);
    TT_ASSERT_EQ(0x22, a);
    TT_ASSERT_EQ(0x67, b);
    ubx_checksum(v, 0, &a, &b);
    TT_ASSERT_EQ(0, a);
    TT_ASSERT_EQ(0, b);
    const uint8_t w[] = {0xFF, 0xFF, 0xFF};
    ubx_checksum(w, sizeof w, &a, &b); /* a: FF, FE, FD; b: FF, FD, FA */
    TT_ASSERT_EQ(0xFD, a);
    TT_ASSERT_EQ(0xFA, b);

    uint8_t out[16];
    const uint8_t exp[] = {0xB5, 0x62, 0x01, 0x21, 0x00, 0x00, 0x22, 0x67};
    TT_ASSERT_EQ(8, ubx_encode_poll(0x01, 0x21, out, sizeof out));
    TT_ASSERT(memcmp(out, exp, 8) == 0);
    TT_ASSERT_EQ(-1, ubx_encode_poll(0x01, 0x21, out, 7));
    TT_ASSERT_EQ(8, ubx_encode_poll(0x01, 0x21, out, 8));
}

static void test_decode(void)
{
    uint8_t pl[20];
    ubx_timeutc_t t;
    mk_timeutc(pl, 0x07);
    TT_ASSERT_EQ(0, ubx_decode_nav_timeutc(pl, 20, &t));
    TT_ASSERT_EQ(345678123u, t.itow);
    TT_ASSERT_EQ(50, t.t_acc);
    TT_ASSERT_EQ(-123456789, t.nano);
    TT_ASSERT_EQ(2026, t.year);
    TT_ASSERT_EQ(10, t.month);
    TT_ASSERT_EQ(3, t.day);
    TT_ASSERT_EQ(12, t.hour);
    TT_ASSERT_EQ(34, t.min);
    TT_ASSERT_EQ(56, t.sec);
    TT_ASSERT_EQ(0x07, t.valid);
    TT_ASSERT(ubx_timeutc_valid_tow(&t));
    TT_ASSERT(ubx_timeutc_valid_wkn(&t));
    TT_ASSERT(ubx_timeutc_valid_utc(&t));

    mk_timeutc(pl, 0x03);
    TT_ASSERT_EQ(0, ubx_decode_nav_timeutc(pl, 20, &t));
    TT_ASSERT_EQ(0x03, t.valid);
    TT_ASSERT(ubx_timeutc_valid_tow(&t));
    TT_ASSERT(ubx_timeutc_valid_wkn(&t));
    TT_ASSERT(!ubx_timeutc_valid_utc(&t));

    mk_timeutc(pl, 0x04);
    ubx_decode_nav_timeutc(pl, 20, &t);
    TT_ASSERT(!ubx_timeutc_valid_tow(&t) && !ubx_timeutc_valid_wkn(&t) && ubx_timeutc_valid_utc(&t));

    /* extremes: nano +1e9, max itow */
    put32(pl, 0xFFFFFFFFu);
    put32(pl + 8, 1000000000u);
    ubx_decode_nav_timeutc(pl, 20, &t);
    TT_ASSERT(t.itow == 0xFFFFFFFFu);
    TT_ASSERT_EQ(1000000000, t.nano);

    TT_ASSERT_EQ(-1, ubx_decode_nav_timeutc(pl, 19, &t));
    TT_ASSERT_EQ(-1, ubx_decode_nav_timeutc(pl, 21, &t));
    TT_ASSERT_EQ(-1, ubx_decode_nav_timeutc(pl, 0, &t));
}

static void test_frame_and_every_split(void)
{
    uint8_t f[64];
    size_t n = mk_nav(f, 0x07);
    TT_ASSERT_EQ(28, n);
    for (size_t split = 0; split <= n; split++) {
        ubx_t p;
        col_t c = {0};
        ubx_init(&p);
        ubx_feed(&p, f, split, on_ubx, &c);
        ubx_feed(&p, f + split, n - split, on_ubx, &c);
        TT_ASSERT_EQ(1, c.n);
        TT_ASSERT_EQ(0x01, c.cls[0]);
        TT_ASSERT_EQ(0x21, c.id[0]);
        TT_ASSERT_EQ(20, c.len[0]);
        uint8_t pl[20];
        mk_timeutc(pl, 0x07);
        TT_ASSERT(memcmp(c.pl[0], pl, 20) == 0);
        TT_ASSERT_EQ(1, p.good);
        TT_ASSERT_EQ(0, p.bad_ck);
        TT_ASSERT(ubx_idle(&p));
    }
    /* byte by byte */
    ubx_t p;
    col_t c = {0};
    ubx_init(&p);
    for (size_t i = 0; i < n; i++) {
        ubx_feed(&p, f + i, 1, on_ubx, &c);
    }
    TT_ASSERT_EQ(1, c.n);
}

static void test_two_frames_and_garbage(void)
{
    uint8_t buf[128];
    size_t n = 0;
    const uint8_t junk[] = {0x00, 0xB5, 0x00, 0xB5, 0xB5, 0x12, 0x62, 0xFF};
    memcpy(buf, junk, sizeof junk);
    n += sizeof junk;
    n += mk_nav(buf + n, 0x07);
    n += mk_nav(buf + n, 0x03);
    ubx_t p;
    col_t c = {0};
    ubx_init(&p);
    ubx_feed(&p, buf, n, on_ubx, &c);
    TT_ASSERT_EQ(2, c.n);
    TT_ASSERT_EQ(0x07, c.pl[0][19]);
    TT_ASSERT_EQ(0x03, c.pl[1][19]);
    TT_ASSERT_EQ(2, p.good);
    TT_ASSERT_EQ(0, p.bad_ck);

    /* empty payload frame (the poll itself) parses with len 0 */
    uint8_t poll[8];
    ubx_encode_poll(0x01, 0x21, poll, 8);
    ubx_init(&p);
    memset(&c, 0, sizeof c);
    ubx_feed(&p, poll, 8, on_ubx, &c);
    TT_ASSERT_EQ(1, c.n);
    TT_ASSERT_EQ(0, c.len[0]);
}

static void test_bad_checksum_then_good(void)
{
    uint8_t buf[128];
    size_t n = mk_nav(buf, 0x07);
    buf[10] ^= 0x01; /* corrupt payload */
    n += mk_nav(buf + n, 0x03);
    ubx_t p;
    col_t c = {0};
    ubx_init(&p);
    ubx_feed(&p, buf, n, on_ubx, &c);
    TT_ASSERT_EQ(1, c.n);
    TT_ASSERT_EQ(0x03, c.pl[0][19]);
    TT_ASSERT_EQ(1, p.bad_ck);
    TT_ASSERT_EQ(1, p.good);

    /* bad checksum byte only */
    n = mk_nav(buf, 0x07);
    buf[n - 1] ^= 0xFF;
    n += mk_nav(buf + n, 0x03);
    ubx_init(&p);
    memset(&c, 0, sizeof c);
    ubx_feed(&p, buf, n, on_ubx, &c);
    TT_ASSERT_EQ(1, c.n);
    TT_ASSERT_EQ(1, p.bad_ck);
}

static void test_resync_inside_corrupt_frame(void)
{
    /* A truncated frame header claims a long payload and swallows a good frame; the
     * checksum fails and the search restarts after the first sync byte, so the good
     * frame inside is found. */
    uint8_t good[28];
    size_t g = mk_nav(good, 0x07);
    uint8_t buf[128];
    size_t n = 0;
    buf[n++] = 0xB5;
    buf[n++] = 0x62;
    buf[n++] = 0x01;
    buf[n++] = 0x30;
    buf[n++] = 28; /* length 28: the payload is exactly the good frame */
    buf[n++] = 0;
    memcpy(buf + n, good, g);
    n += g;
    buf[n++] = 0x11; /* wrong checksum bytes at this position */
    buf[n++] = 0x22;
    for (size_t split = 0; split <= n; split += 7) {
        ubx_t p;
        col_t c = {0};
        ubx_init(&p);
        ubx_feed(&p, buf, split, on_ubx, &c);
        ubx_feed(&p, buf + split, n - split, on_ubx, &c);
        TT_ASSERT_EQ(1, c.n);
        TT_ASSERT_EQ(1, p.bad_ck);
        TT_ASSERT_EQ(1, p.good);
    }
}

static void test_oversized_then_good(void)
{
    uint8_t buf[256];
    size_t n = 0;
    uint8_t big[UBX_MAX_PAYLOAD + 1];
    memset(big, 0xB5, sizeof big); /* payload full of sync bytes: must not be re-parsed */
    big[1] = 0x62;
    n += mkframe(buf, 0x05, 0x01, big, sizeof big);
    n += mk_nav(buf + n, 0x07);
    for (size_t split = 0; split <= n; split++) {
        ubx_t p;
        col_t c = {0};
        ubx_init(&p);
        ubx_feed(&p, buf, split, on_ubx, &c);
        ubx_feed(&p, buf + split, n - split, on_ubx, &c);
        TT_ASSERT_EQ(1, c.n);
        TT_ASSERT_EQ(0x21, c.id[0]);
        TT_ASSERT_EQ(1, p.skipped);
        TT_ASSERT_EQ(0, p.bad_ck);
        TT_ASSERT_EQ(1, p.good);
    }
    /* exactly UBX_MAX_PAYLOAD is accepted */
    uint8_t ok[UBX_MAX_PAYLOAD];
    memset(ok, 0x5A, sizeof ok);
    n = mkframe(buf, 0x05, 0x01, ok, sizeof ok);
    ubx_t p;
    col_t c = {0};
    ubx_init(&p);
    ubx_feed(&p, buf, n, on_ubx, &c);
    TT_ASSERT_EQ(1, c.n);
    TT_ASSERT_EQ(UBX_MAX_PAYLOAD, c.len[0]);
    TT_ASSERT_EQ(0, p.skipped);
}

/* Length above UBX_SKIP_MAX: false sync, nothing is swallowed. */
static void test_huge_length_is_false_sync(void)
{
    static const uint16_t lens[] = {UBX_SKIP_MAX + 1u, 1000u, 0xFFFFu};
    for (size_t k = 0; k < sizeof lens / sizeof lens[0]; k++) {
        uint8_t buf[64];
        size_t n = 0;
        buf[n++] = 0xB5;
        buf[n++] = 0x62;
        buf[n++] = 0x01;
        buf[n++] = 0x02;
        buf[n++] = (uint8_t)(lens[k] & 0xFF);
        buf[n++] = (uint8_t)(lens[k] >> 8);
        n += mk_nav(buf + n, 0x07);
        for (size_t split = 0; split <= n; split++) {
            ubx_t p;
            col_t c = {0};
            ubx_init(&p);
            ubx_feed(&p, buf, split, on_ubx, &c);
            ubx_feed(&p, buf + split, n - split, on_ubx, &c);
            TT_ASSERT_EQ(1, c.n);
            TT_ASSERT_EQ(0x07, c.pl[0][19]);
            TT_ASSERT_EQ(1, p.bad_len);
            TT_ASSERT_EQ(0, p.skipped);
            TT_ASSERT_EQ(0, p.bad_ck);
            TT_ASSERT_EQ(1, p.good);
            TT_ASSERT(ubx_idle(&p));
        }
    }
    /* the replayed bytes hold a real frame: B5 62 + (B5 62 01 21 ...) reads as
     * class B5, id 62, length 0x2101 -> false sync, the frame behind is found */
    uint8_t buf[64];
    size_t n = 0;
    buf[n++] = 0xB5;
    buf[n++] = 0x62;
    n += mk_nav(buf + n, 0x03);
    ubx_t p;
    col_t c = {0};
    ubx_init(&p);
    ubx_feed(&p, buf, n, on_ubx, &c);
    TT_ASSERT_EQ(1, p.bad_len);
    TT_ASSERT_EQ(1, c.n);
    TT_ASSERT_EQ(1, p.good);
}

/* Exactly UBX_SKIP_MAX is still skipped by length. */
static void test_skip_max_is_skipped(void)
{
    uint8_t big[UBX_SKIP_MAX];
    uint8_t buf[UBX_SKIP_MAX + 64];
    memset(big, 0xB5, sizeof big);
    size_t n = mkframe(buf, 0x05, 0x01, big, sizeof big);
    n += mk_nav(buf + n, 0x07);
    ubx_t p;
    col_t c = {0};
    ubx_init(&p);
    ubx_feed(&p, buf, n, on_ubx, &c);
    TT_ASSERT_EQ(1, c.n);
    TT_ASSERT_EQ(1, p.skipped);
    TT_ASSERT_EQ(0, p.bad_len);
    TT_ASSERT_EQ(0, p.bad_ck);
}

static void test_truncated_then_new_sync(void)
{
    uint8_t buf[128];
    size_t n = mk_nav(buf, 0x07);
    /* cut after 15 bytes; the next frame's sync lands inside the payload of the
     * first and is consumed by it, the checksum fails, resync finds the 2nd frame. */
    size_t cut = 15;
    uint8_t all[128];
    memcpy(all, buf, cut);
    size_t m = cut;
    m += mk_nav(all + m, 0x03);
    m += mk_nav(all + m, 0x07);
    ubx_t p;
    col_t c = {0};
    ubx_init(&p);
    ubx_feed(&p, all, m, on_ubx, &c);
    TT_ASSERT(c.n >= 1);
    TT_ASSERT_EQ(0x07, c.pl[c.n - 1][19]);
    TT_ASSERT(p.good >= 1);
    (void)n;
}

static void test_state_resets(void)
{
    ubx_t p;
    col_t c = {0};
    uint8_t f[28];
    size_t n = mk_nav(f, 0x07);
    ubx_init(&p);
    ubx_feed(&p, f, 10, on_ubx, &c);
    TT_ASSERT(!ubx_idle(&p));
    ubx_abort(&p);
    TT_ASSERT(ubx_idle(&p));
    ubx_feed(&p, f, n, on_ubx, &c);
    TT_ASSERT_EQ(1, c.n);
    ubx_feed(&p, f, 1, NULL, NULL); /* NULL callback tolerated */
    TT_ASSERT(ubx_wait_sync2(&p));
}

int main(void)
{
    TT_RUN(test_checksum_and_poll);
    TT_RUN(test_decode);
    TT_RUN(test_frame_and_every_split);
    TT_RUN(test_two_frames_and_garbage);
    TT_RUN(test_bad_checksum_then_good);
    TT_RUN(test_resync_inside_corrupt_frame);
    TT_RUN(test_oversized_then_good);
    TT_RUN(test_huge_length_is_false_sync);
    TT_RUN(test_skip_max_is_skipped);
    TT_RUN(test_truncated_then_new_sync);
    TT_RUN(test_state_resets);
    return TT_RESULT();
}
