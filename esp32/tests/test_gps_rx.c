/* Host tests for the NMEA/UBX router: demultiplexing, chunk independence,
 * NMEA results unchanged by UBX traffic. */
#include <stdio.h>
#include <string.h>
#include "tinytest.h"
#include "gps_rx.h"

#define MAXEV 16
typedef struct {
    nmea_event_t ev[MAXEV];
    int n;
} ncol_t;
typedef struct {
    int n;
    uint8_t cls[MAXEV], id[MAXEV];
    size_t len[MAXEV];
    uint8_t pl[MAXEV][UBX_MAX_PAYLOAD];
} ucol_t;

static void on_nmea(const nmea_event_t *ev, void *ctx)
{
    ncol_t *c = (ncol_t *)ctx;
    if (c->n < MAXEV) {
        c->ev[c->n++] = *ev;
    }
}

static void on_ubx(uint8_t cls, uint8_t id, const uint8_t *pl, size_t len, void *ctx)
{
    ucol_t *c = (ucol_t *)ctx;
    if (c->n < MAXEV) {
        c->cls[c->n] = cls;
        c->id[c->n] = id;
        c->len[c->n] = len;
        memcpy(c->pl[c->n], pl, len);
        c->n++;
    }
}

static size_t mk_nmea(char *out, const char *body)
{
    uint8_t x = 0;
    for (const char *s = body; *s; s++) {
        x ^= (uint8_t)*s;
    }
    return (size_t)sprintf(out, "$%s*%02X\r\n", body, x);
}

static size_t mkframe(uint8_t *out, uint8_t cls, uint8_t id, const uint8_t *pl, size_t n)
{
    out[0] = 0xB5;
    out[1] = 0x62;
    out[2] = cls;
    out[3] = id;
    out[4] = (uint8_t)n;
    out[5] = 0;
    memcpy(out + 6, pl, n);
    ubx_checksum(out + 2, 4 + n, &out[6 + n], &out[7 + n]);
    return 8 + n;
}

static const char RMC1[] = "GPRMC,123519.00,A,4807.0380,N,01131.0000,E,022.4,084.4,230326,,,A";
static const char GGA1[] = "GPGGA,123519.00,4807.0380,N,01131.0000,E,1,08,0.90,545.4,M,46.9,M,,";
static const char RMC2[] = "GPRMC,123520.00,A,4807.0390,N,01131.0010,E,022.4,084.4,230326,,,A";

static void run(const uint8_t *buf, size_t n, size_t split, gps_rx_t *rx, ncol_t *nc, ucol_t *uc)
{
    gps_rx_init(rx);
    memset(nc, 0, sizeof *nc);
    memset(uc, 0, sizeof *uc);
    if (split > n) {
        split = n;
    }
    gps_rx_feed(rx, buf, split, on_nmea, nc, on_ubx, uc);
    gps_rx_feed(rx, buf + split, n - split, on_nmea, nc, on_ubx, uc);
}

static bool same_events(const ncol_t *a, const ncol_t *b)
{
    if (a->n != b->n) {
        return false;
    }
    for (int i = 0; i < a->n; i++) {
        if (a->ev[i].kind != b->ev[i].kind || memcmp(&a->ev[i].fix, &b->ev[i].fix, sizeof a->ev[i].fix) != 0 ||
            a->ev[i].have_rmc != b->ev[i].have_rmc || a->ev[i].have_gga != b->ev[i].have_gga) {
            return false;
        }
    }
    return true;
}

static void assemble(uint8_t *buf, size_t *n, const uint8_t *ubx, size_t ulen)
{
    char l[160];
    size_t m = 0;
    size_t k = mk_nmea(l, RMC1);
    memcpy(buf, l, k);
    m = k;
    if (ubx) {
        memcpy(buf + m, ubx, ulen);
        m += ulen;
    }
    k = mk_nmea(l, GGA1);
    memcpy(buf + m, l, k);
    m += k;
    k = mk_nmea(l, RMC2);
    memcpy(buf + m, l, k);
    m += k;
    *n = m;
}

static void test_mixed_every_split(void)
{
    uint8_t pl[20] = {1, 2, 3};
    uint8_t u[28];
    size_t ul = mkframe(u, 0x01, 0x21, pl, 20);
    uint8_t ref[512], mix[512];
    size_t rn, mn;
    assemble(ref, &rn, NULL, 0);
    assemble(mix, &mn, u, ul);
    TT_ASSERT_EQ(rn + ul, mn);

    gps_rx_t r0;
    ncol_t n0;
    ucol_t u0;
    run(ref, rn, rn, &r0, &n0, &u0);
    TT_ASSERT(n0.n >= 3);
    TT_ASSERT_EQ(0, u0.n);

    for (size_t s = 0; s <= mn; s++) {
        gps_rx_t r;
        ncol_t nc;
        ucol_t uc;
        run(mix, mn, s, &r, &nc, &uc);
        TT_ASSERT(same_events(&n0, &nc));
        TT_ASSERT_EQ(1, uc.n);
        TT_ASSERT_EQ(0x21, uc.id[0]);
        TT_ASSERT_EQ(20, uc.len[0]);
        TT_ASSERT(memcmp(uc.pl[0], pl, 20) == 0);
        TT_ASSERT_EQ(r0.nmea.good_lines, r.nmea.good_lines);
        TT_ASSERT_EQ(r0.nmea.bad_lines, r.nmea.bad_lines);
        TT_ASSERT_EQ(r0.nmea.dropped_lines, r.nmea.dropped_lines);
        TT_ASSERT_EQ(1, r.ubx.good);
    }
    /* byte by byte */
    gps_rx_t r;
    ncol_t nc = {0};
    ucol_t uc = {0};
    gps_rx_init(&r);
    for (size_t i = 0; i < mn; i++) {
        gps_rx_feed(&r, mix + i, 1, on_nmea, &nc, on_ubx, &uc);
    }
    TT_ASSERT(same_events(&n0, &nc));
    TT_ASSERT_EQ(1, uc.n);
}

static void test_dollar_and_sync_inside_payload(void)
{
    /* Payload holds a complete NMEA-looking line, "$", and B5 62 sequences. */
    char l[160];
    size_t k = mk_nmea(l, "GPGLL,,,,,,V"); /* short: must fit the payload buffer */
    uint8_t pl[UBX_MAX_PAYLOAD];
    memset(pl, 0, sizeof pl);
    memcpy(pl, l, k);
    pl[k] = 0xB5;
    pl[k + 1] = 0x62;
    pl[k + 2] = '$';
    uint8_t u[8 + UBX_MAX_PAYLOAD];
    size_t ul = mkframe(u, 0x01, 0x21, pl, 60);
    uint8_t buf[256];
    size_t n = 0;
    memcpy(buf, u, ul);
    n = ul;
    k = mk_nmea(l, GGA1);
    memcpy(buf + n, l, k);
    n += k;
    for (size_t s = 0; s <= n; s++) {
        gps_rx_t r;
        ncol_t nc;
        ucol_t uc;
        run(buf, n, s, &r, &nc, &uc);
        TT_ASSERT_EQ(1, uc.n);
        TT_ASSERT_EQ(60, uc.len[0]);
        TT_ASSERT(memcmp(uc.pl[0], pl, 60) == 0);
        TT_ASSERT_EQ(1, r.nmea.good_lines); /* only the GGA after the frame */
        TT_ASSERT_EQ(0, r.nmea.bad_lines);
        TT_ASSERT_EQ(0, r.nmea.dropped_lines);
    }
}

static void test_lone_b5_and_b5_in_nmea_line(void)
{
    char l[160];
    size_t k = mk_nmea(l, RMC1);
    uint8_t buf[256];
    size_t n = 0;
    /* lone 0xB5 between lines (not followed by 0x62): dropped, next NMEA fine */
    buf[n++] = 0xB5;
    memcpy(buf + n, l, k);
    n += k;
    buf[n++] = 0xB5;
    buf[n++] = 0xB5; /* B5 B5 62 .. : second one is the sync */
    memcpy(buf + n, l, k);
    n += k;
    for (size_t s = 0; s <= n; s++) {
        gps_rx_t r;
        ncol_t nc;
        ucol_t uc;
        run(buf, n, s, &r, &nc, &uc);
        TT_ASSERT_EQ(0, uc.n);
        TT_ASSERT_EQ(2, r.nmea.good_lines);
        TT_ASSERT_EQ(0, r.nmea.bad_lines);
    }
    /* 0xB5 inside an NMEA line: bad NMEA line, no UBX start, parser recovers */
    n = 0;
    memcpy(buf, l, 10);
    n = 10;
    buf[n++] = 0xB5;
    buf[n++] = 0x62; /* would-be sync inside the line */
    memcpy(buf + n, l + 10, k - 10);
    n += k - 10;
    memcpy(buf + n, l, k);
    n += k;
    for (size_t s = 0; s <= n; s++) {
        gps_rx_t r;
        ncol_t nc;
        ucol_t uc;
        run(buf, n, s, &r, &nc, &uc);
        TT_ASSERT_EQ(0, uc.n);
        TT_ASSERT_EQ(1, r.nmea.bad_lines);
        TT_ASSERT_EQ(1, r.nmea.good_lines);
    }
}

static void test_ubx_directly_after_nmea_and_bad_ubx(void)
{
    /* UBX immediately after CR LF, and a corrupt UBX frame followed by NMEA. */
    char l[160];
    size_t k = mk_nmea(l, RMC1);
    uint8_t pl[20] = {9};
    uint8_t u[28];
    size_t ul = mkframe(u, 0x01, 0x21, pl, 20);
    uint8_t buf[256];
    size_t n = 0;
    memcpy(buf, l, k);
    n = k;
    memcpy(buf + n, u, ul);
    n += ul;
    u[27] ^= 0x01; /* bad checksum */
    memcpy(buf + n, u, ul);
    n += ul;
    memcpy(buf + n, l, k);
    n += k;
    for (size_t s = 0; s <= n; s++) {
        gps_rx_t r;
        ncol_t nc;
        ucol_t uc;
        run(buf, n, s, &r, &nc, &uc);
        TT_ASSERT_EQ(1, uc.n);
        TT_ASSERT_EQ(1, r.ubx.bad_ck);
        TT_ASSERT_EQ(2, r.nmea.good_lines);
        TT_ASSERT_EQ(0, r.nmea.bad_lines);
    }
}

int main(void)
{
    TT_RUN(test_mixed_every_split);
    TT_RUN(test_dollar_and_sync_inside_payload);
    TT_RUN(test_lone_b5_and_b5_in_nmea_line);
    TT_RUN(test_ubx_directly_after_nmea_and_bad_ubx);
    return TT_RESULT();
}
