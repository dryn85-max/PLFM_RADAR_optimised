/* Host tests for ld2410_parser: split frames, resync, false headers, limits. */
#include <string.h>
#include "tinytest.h"
#include "vec_util.h"
#include "ld2410_parser.h"

#define MAXF 64
typedef struct {
    int n;
    ld_frame_kind_t kind[MAXF];
    size_t len[MAXF];
    uint8_t raw[MAXF][LD_MAX_FRAME];
    size_t plen[MAXF];
    int payload_ok[MAXF];
} got_t;

static void on_frame(const ld_frame_t *f, void *ctx)
{
    got_t *g = ctx;
    if (g->n >= MAXF) return;
    g->kind[g->n] = f->kind;
    g->len[g->n] = f->raw_len;
    memcpy(g->raw[g->n], f->raw, f->raw_len);
    g->plen[g->n] = f->payload_len;
    g->payload_ok[g->n] = f->payload == f->raw + 6 && f->payload_len + 10 == f->raw_len;
    g->n++;
}

static const uint8_t DH[4] = {0xF4, 0xF3, 0xF2, 0xF1}, DF[4] = {0xF8, 0xF7, 0xF6, 0xF5};
static const uint8_t CH[4] = {0xFD, 0xFC, 0xFB, 0xFA}, CF[4] = {0x04, 0x03, 0x02, 0x01};

static size_t mk(uint8_t *out, const uint8_t *h, const uint8_t *f, size_t plen, uint8_t fill)
{
    memcpy(out, h, 4);
    out[4] = (uint8_t)(plen & 0xFF);
    out[5] = (uint8_t)(plen >> 8);
    for (size_t i = 0; i < plen; i++) out[6 + i] = (uint8_t)(fill + i);
    memcpy(out + 6 + plen, f, 4);
    return 10 + plen;
}

static void feed(ld_parser_t *p, got_t *g, const uint8_t *d, size_t n)
{
    ld_parser_feed(p, d, n, on_frame, g);
}

static void test_single_and_kinds(void)
{
    ld_parser_t p; got_t g; uint8_t a[80], b[80];
    memset(&g, 0, sizeof g);
    ld_parser_init(&p);
    size_t na = mk(a, DH, DF, 13, 1), nb = mk(b, CH, CF, 4, 9);
    feed(&p, &g, a, na);
    feed(&p, &g, b, nb);
    TT_ASSERT_EQ(2, g.n);
    TT_ASSERT_EQ(LD_FRAME_DATA, g.kind[0]);
    TT_ASSERT_EQ(LD_FRAME_CMD, g.kind[1]);
    TT_ASSERT(g.len[0] == na && memcmp(g.raw[0], a, na) == 0);
    TT_ASSERT(g.len[1] == nb && memcmp(g.raw[1], b, nb) == 0);
    TT_ASSERT(g.payload_ok[0] && g.payload_ok[1]);
    TT_ASSERT_EQ(13, g.plen[0]);
    TT_ASSERT_EQ(2, p.frames);
    TT_ASSERT_EQ(0, p.dropped_bytes);
}

static void test_split_every_boundary(void)
{
    uint8_t raw[2][80]; size_t n[2];
    n[0] = mk(raw[0], DH, DF, 35, 3);
    n[1] = mk(raw[1], CH, CF, 4, 7);
    for (int k = 0; k < 2; k++) {
        for (size_t cut = 0; cut <= n[k]; cut++) {
            ld_parser_t p; got_t g; memset(&g, 0, sizeof g);
            ld_parser_init(&p);
            feed(&p, &g, raw[k], cut);
            feed(&p, &g, raw[k] + cut, n[k] - cut);
            TT_ASSERT_EQ(1, g.n);
            TT_ASSERT(g.n == 1 && g.len[0] == n[k] && memcmp(g.raw[0], raw[k], n[k]) == 0);
        }
        /* three-way splits too */
        for (size_t c1 = 0; c1 <= n[k]; c1++)
            for (size_t c2 = c1; c2 <= n[k]; c2++) {
                ld_parser_t p; got_t g; memset(&g, 0, sizeof g);
                ld_parser_init(&p);
                feed(&p, &g, raw[k], c1);
                feed(&p, &g, raw[k] + c1, c2 - c1);
                feed(&p, &g, raw[k] + c2, n[k] - c2);
                TT_ASSERT_EQ(1, g.n);
            }
    }
    /* byte at a time, with a following frame */
    uint8_t two[160]; memcpy(two, raw[0], n[0]); memcpy(two + n[0], raw[1], n[1]);
    ld_parser_t p; got_t g; memset(&g, 0, sizeof g);
    ld_parser_init(&p);
    for (size_t i = 0; i < n[0] + n[1]; i++) feed(&p, &g, two + i, 1);
    TT_ASSERT_EQ(2, g.n);
}

static void test_two_in_one_chunk_and_three(void)
{
    uint8_t buf[300]; size_t o = 0, n1, n2, n3;
    n1 = mk(buf + o, DH, DF, 13, 1); o += n1;
    n2 = mk(buf + o, CH, CF, 4, 2); o += n2;
    n3 = mk(buf + o, DH, DF, 35, 3); o += n3;
    ld_parser_t p; got_t g; memset(&g, 0, sizeof g);
    ld_parser_init(&p);
    feed(&p, &g, buf, o);
    TT_ASSERT_EQ(3, g.n);
    TT_ASSERT(g.n == 3 && g.len[0] == n1 && g.len[1] == n2 && g.len[2] == n3);
    TT_ASSERT(g.n == 3 && g.kind[1] == LD_FRAME_CMD);
}

static void test_garbage_and_false_headers(void)
{
    uint8_t buf[300], f[80]; size_t o = 0;
    size_t nf = mk(f, DH, DF, 13, 5);
    const uint8_t junk1[] = {0x00, 0xFF, 0x12, 0xF4, 0xF3, 0xF2, 0x00, 0xFD, 0xFC, 0xFB, 0x77};
    memcpy(buf + o, junk1, sizeof junk1); o += sizeof junk1;
    memcpy(buf + o, f, nf); o += nf;
    const uint8_t junk2[] = {0xF4, 0xF3, 0xF2, 0xF4, 0xF3, 0xF2, 0xF1}; /* false header, then real header start */
    memcpy(buf + o, junk2, sizeof junk2); o += sizeof junk2;
    /* the last 4 junk bytes F4 F3 F2 F1 are a header whose length follows: make it a real frame tail */
    buf[o++] = 0x02; buf[o++] = 0x00; buf[o++] = 0xAA; buf[o++] = 0xBB;
    memcpy(buf + o, DF, 4); o += 4;
    ld_parser_t p; got_t g; memset(&g, 0, sizeof g);
    ld_parser_init(&p);
    feed(&p, &g, buf, o);
    TT_ASSERT_EQ(2, g.n);
    TT_ASSERT(g.n == 2 && g.len[0] == nf && memcmp(g.raw[0], f, nf) == 0);
    TT_ASSERT(g.n == 2 && g.len[1] == 12 && g.plen[1] == 2);
    TT_ASSERT(p.dropped_bytes >= 11);
}

static void test_fake_header_inside_payload(void)
{
    /* A valid frame whose payload contains a complete fake data header and
     * a fake footer: it is one frame, not three. */
    uint8_t f[80]; size_t n = mk(f, DH, DF, 20, 0x40);
    memcpy(f + 6 + 2, DH, 4);
    f[6 + 6] = 0x03; f[6 + 7] = 0x00;
    memcpy(f + 6 + 12, DF, 4);
    ld_parser_t p; got_t g; memset(&g, 0, sizeof g);
    ld_parser_init(&p);
    feed(&p, &g, f, n);
    TT_ASSERT_EQ(1, g.n);
    TT_ASSERT(g.n == 1 && g.len[0] == n && memcmp(g.raw[0], f, n) == 0);
    TT_ASSERT_EQ(0, p.dropped_bytes);
}

static void test_real_frame_inside_rejected_bytes(void)
{
    /* Outer header claims 32 payload bytes; a real frame starts 6 bytes in
     * and the outer footer never comes. When the outer is rejected, the
     * inner real frame must still be found. */
    uint8_t inner[40], buf[100]; size_t ni = mk(inner, DH, DF, 13, 0x20);
    size_t o = 0;
    memcpy(buf, DH, 4); buf[4] = 32; buf[5] = 0; o = 6;
    memcpy(buf + o, inner, ni); o += ni;
    while (o < 6 + 32 + 4) buf[o++] = 0x11; /* filler, wrong footer */
    ld_parser_t p; got_t g; memset(&g, 0, sizeof g);
    ld_parser_init(&p);
    feed(&p, &g, buf, o);
    TT_ASSERT_EQ(1, g.n);
    TT_ASSERT(g.n == 1 && g.len[0] == ni && memcmp(g.raw[0], inner, ni) == 0);
    /* same, with the inner frame completing exactly on the byte that rejects the outer */
    uint8_t inner2[40]; size_t n2 = mk(inner2, CH, CF, 4, 1); /* 14 bytes */
    memcpy(buf, DH, 4); buf[4] = 20; buf[5] = 0;
    memset(buf + 6, 0x11, 7);
    memcpy(buf + 13, inner2, n2);              /* ends at index 26 = outer footer position */
    memset(&g, 0, sizeof g); ld_parser_init(&p);
    feed(&p, &g, buf, 13 + n2);
    TT_ASSERT_EQ(1, g.n);
    TT_ASSERT(g.n == 1 && g.kind[0] == LD_FRAME_CMD && memcmp(g.raw[0], inner2, n2) == 0);
}

static void test_length_limits(void)
{
    uint8_t f[100], g2[100];
    ld_parser_t p; got_t g;
    /* exactly max accepted */
    size_t n = mk(f, DH, DF, LD_MAX_PAYLOAD, 1);
    memset(&g, 0, sizeof g); ld_parser_init(&p);
    feed(&p, &g, f, n);
    TT_ASSERT_EQ(1, g.n);
    TT_ASSERT_EQ(LD_MAX_FRAME, n);
    /* max + 1 rejected, and the next real frame is still found */
    uint8_t big[LD_MAX_FRAME + 20]; size_t nb = mk(big, DH, DF, LD_MAX_PAYLOAD + 1, 1);
    size_t ng = mk(g2, DH, DF, 13, 9);
    memset(&g, 0, sizeof g); ld_parser_init(&p);
    feed(&p, &g, big, nb);
    feed(&p, &g, g2, ng);
    TT_ASSERT_EQ(1, g.n);
    TT_ASSERT(g.n == 1 && g.len[0] == ng);
    /* huge lengths */
    const uint8_t huge[] = {0xF4, 0xF3, 0xF2, 0xF1, 0xFF, 0xFF, 1, 2, 3};
    memset(&g, 0, sizeof g); ld_parser_init(&p);
    feed(&p, &g, huge, sizeof huge);
    feed(&p, &g, g2, ng);
    TT_ASSERT_EQ(1, g.n);
    /* zero length rejected */
    uint8_t z[10]; mk(z, DH, DF, 0, 0);
    memset(&g, 0, sizeof g); ld_parser_init(&p);
    feed(&p, &g, z, 10);
    TT_ASSERT_EQ(0, g.n);
}

static void test_wrong_footer(void)
{
    uint8_t f[80], g2[80]; size_t n = mk(f, DH, DF, 13, 1), ng = mk(g2, DH, DF, 13, 2);
    for (size_t i = 0; i < 4; i++) {
        uint8_t bad[160];
        memcpy(bad, f, n); bad[n - 4 + i] ^= 0x01;
        memcpy(bad + n, g2, ng);
        ld_parser_t p; got_t g; memset(&g, 0, sizeof g);
        ld_parser_init(&p);
        feed(&p, &g, bad, n + ng);
        TT_ASSERT_EQ(1, g.n);
        TT_ASSERT(g.n == 1 && memcmp(g.raw[0], g2, ng) == 0);
    }
    /* data header with command footer */
    uint8_t mix[80]; size_t nm = mk(mix, DH, CF, 13, 1);
    ld_parser_t p; got_t g; memset(&g, 0, sizeof g);
    ld_parser_init(&p);
    feed(&p, &g, mix, nm);
    TT_ASSERT_EQ(0, g.n);
}

static void count_frame(const ld_frame_t *f, void *ctx)
{
    (void)f;
    (*(int *)ctx)++;
}

static void test_noise_fuzz(void)
{
    static uint8_t stream[20000];
    size_t o = 0;
    uint32_t s = 12345;
    uint8_t f[80]; size_t nf = mk(f, DH, DF, 35, 3);
    int expected = 0, seen = 0;
    while (o + 200 < sizeof stream) {
        size_t noise = (s >> 8) % 40;
        for (size_t i = 0; i < noise; i++) { s = s * 1664525u + 1013904223u; stream[o++] = (uint8_t)(s >> 24); }
        s = s * 1664525u + 1013904223u;
        memcpy(stream + o, f, nf); o += nf; expected++;
    }
    ld_parser_t p;
    ld_parser_init(&p);
    size_t pos = 0;
    while (pos < o) {
        s = s * 1664525u + 1013904223u;
        size_t chunk = 1 + (s >> 16) % 97;
        if (chunk > o - pos) chunk = o - pos;
        ld_parser_feed(&p, stream + pos, chunk, count_frame, &seen);
        pos += chunk;
    }
    TT_ASSERT_EQ(expected, seen);
    TT_ASSERT_EQ(expected, p.frames);
    /* pure random bytes biased toward header bytes: bounded state, no crash */
    static const uint8_t hb[8] = {0xF4, 0xF3, 0xF2, 0xF1, 0xFD, 0xFC, 0xFB, 0xFA};
    ld_parser_init(&p);
    for (int i = 0; i < 200000; i++) {
        s = s * 1664525u + 1013904223u;
        uint8_t b = (uint8_t)(s >> 24);
        if ((s & 0x300) == 0) b = hb[(s >> 12) & 7];
        ld_parser_feed(&p, &b, 1, count_frame, &seen);
        TT_ASSERT(p.n < LD_MAX_FRAME);
    }
}

int main(void)
{
    TT_RUN(test_single_and_kinds);
    TT_RUN(test_split_every_boundary);
    TT_RUN(test_two_in_one_chunk_and_three);
    TT_RUN(test_garbage_and_false_headers);
    TT_RUN(test_fake_header_inside_payload);
    TT_RUN(test_real_frame_inside_rejected_bytes);
    TT_RUN(test_length_limits);
    TT_RUN(test_wrong_footer);
    TT_RUN(test_noise_fuzz);
    return TT_RESULT();
}
