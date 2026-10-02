/* Host tests for the NMEA parser: line assembly, checksums, RMC/GGA decoding,
 * epoch merging, chunk-boundary independence. */
#include <stdio.h>
#include <string.h>
#include "tinytest.h"
#include "nmea.h"

#define MAXEV 16
typedef struct {
    nmea_event_t ev[MAXEV];
    int n;
    int n_rmc;
    int n_fix;
} col_t;

static void on_ev(const nmea_event_t *ev, void *vctx)
{
    col_t *c = (col_t *)vctx;
    if (ev->kind == NMEA_EV_RMC) {
        c->n_rmc++;
    } else {
        c->n_fix++;
    }
    if (c->n < MAXEV) {
        c->ev[c->n++] = *ev;
    }
}

/* "$body*HH\r\n" with a correct checksum. Returns the length. */
static size_t mk(char *out, const char *body)
{
    uint8_t x = 0;
    for (const char *s = body; *s; s++) {
        x ^= (uint8_t)*s;
    }
    return (size_t)sprintf(out, "$%s*%02X\r\n", body, x);
}

static void feed_str(nmea_t *p, col_t *c, const char *s)
{
    nmea_feed(p, (const uint8_t *)s, strlen(s), on_ev, c);
}

static void feed_body(nmea_t *p, col_t *c, const char *body)
{
    char b[160];
    mk(b, body);
    feed_str(p, c, b);
}

static const char RMC1[] = "GPRMC,123519.00,A,4807.0380,N,01131.0000,E,022.4,084.4,230326,,,A";
static const char GGA1[] = "GPGGA,123519.00,4807.0380,N,01131.0000,E,1,08,0.90,545.4,M,46.9,M,,";

static const nmea_event_t *last_fix(const col_t *c)
{
    for (int i = c->n - 1; i >= 0; i--) {
        if (c->ev[i].kind == NMEA_EV_FIX) {
            return &c->ev[i];
        }
    }
    return NULL;
}

/* k-th RMC event (events of the two kinds interleave when epochs are superseded). */
static const nmea_event_t *rmc_ev(const col_t *c, int k)
{
    for (int i = 0; i < c->n; i++) {
        if (c->ev[i].kind == NMEA_EV_RMC && k-- == 0) {
            return &c->ev[i];
        }
    }
    static const nmea_event_t none;
    return &none;
}

static void test_days_from_civil(void)
{
    TT_ASSERT_EQ(0, nmea_days_from_civil(1970, 1, 1));
    TT_ASSERT_EQ(10957, nmea_days_from_civil(2000, 1, 1));
    TT_ASSERT_EQ(11016, nmea_days_from_civil(2000, 2, 29)); /* 2000 is a leap year */
    TT_ASSERT_EQ(-1, nmea_days_from_civil(2100, 2, 29));
    TT_ASSERT_EQ(-1, nmea_days_from_civil(2027, 2, 29));
    TT_ASSERT_EQ(-1, nmea_days_from_civil(2026, 4, 31));
    TT_ASSERT_EQ(-1, nmea_days_from_civil(2026, 13, 1));
    TT_ASSERT_EQ(-1, nmea_days_from_civil(2026, 1, 0));
    TT_ASSERT_EQ(1798761599000LL / 86400000LL, nmea_days_from_civil(2026, 12, 31));
    TT_ASSERT_EQ(4102444799990LL / 86400000LL, nmea_days_from_civil(2099, 12, 31));
}

static void test_nominal_epoch(void)
{
    nmea_t p;
    col_t c = {0};
    nmea_init(&p);
    feed_body(&p, &c, RMC1);
    TT_ASSERT_EQ(1, c.n_rmc);
    TT_ASSERT_EQ(0, c.n_fix); /* GGA not seen yet */
    feed_body(&p, &c, GGA1);
    TT_ASSERT_EQ(1, c.n_fix);
    const nmea_event_t *e = last_fix(&c);
    TT_ASSERT(e != NULL);
    if (!e) return;
    TT_ASSERT(e->have_rmc && e->have_gga);
    TT_ASSERT_EQ(1774269319000LL, e->fix.utc_unix_ms);
    TT_ASSERT_EQ(481173000, e->fix.lat_e7);
    TT_ASSERT_EQ(115166667, e->fix.lon_e7);
    TT_ASSERT_EQ(54540, e->fix.alt_cm);
    TT_ASSERT_EQ(1152, e->fix.speed_cmps);
    TT_ASSERT_EQ(8440, e->fix.course_cdeg);
    TT_ASSERT_EQ(90, e->fix.hdop_x100);
    TT_ASSERT_EQ(8, e->fix.sats);
    TT_ASSERT_EQ(1, e->fix.fix_quality);
    TT_ASSERT_EQ(0x0F, e->fix.flags);
    TT_ASSERT_EQ(2, p.good_lines);
    TT_ASSERT_EQ(0, p.bad_lines);
    TT_ASSERT_EQ(0, p.dropped_lines);
    /* RMC event carries the RMC-only fix. */
    TT_ASSERT_EQ(NMEA_EV_RMC, c.ev[0].kind);
    TT_ASSERT_EQ(1774269319000LL, c.ev[0].fix.utc_unix_ms);
}

static void test_literal_sentences(void)
{
    /* Fixed text with checksums computed independently. */
    nmea_t p;
    col_t c = {0};
    nmea_init(&p);
    feed_str(&p, &c, "$GPRMC,123519.00,A,4807.0380,N,01131.0000,E,022.4,084.4,230394,,,A*52\r\n");
    TT_ASSERT_EQ(0, p.bad_lines); /* year 1994 -> 2094: valid under the 20yy rule */
    TT_ASSERT_EQ(1, p.good_lines);
    feed_str(&p, &c, "$GPGGA,123519.00,4807.0380,N,01131.0000,E,1,08,0.90,545.4,M,46.9,M,,*59\r\n");
    TT_ASSERT_EQ(2, p.good_lines);
    TT_ASSERT_EQ(1, c.n_fix);
}

static void test_gga_first_order(void)
{
    nmea_t p;
    col_t c = {0};
    nmea_init(&p);
    feed_body(&p, &c, GGA1);
    TT_ASSERT_EQ(0, c.n_fix);
    feed_body(&p, &c, RMC1);
    TT_ASSERT_EQ(1, c.n_fix);
    /* The RMC event precedes the FIX it completes. */
    TT_ASSERT_EQ(NMEA_EV_RMC, c.ev[0].kind);
    TT_ASSERT_EQ(NMEA_EV_FIX, c.ev[1].kind);
    TT_ASSERT_EQ(1774269319000LL, c.ev[1].fix.utc_unix_ms);
}

static void test_new_second_emits_partial(void)
{
    nmea_t p;
    col_t c = {0};
    nmea_init(&p);
    feed_body(&p, &c, RMC1); /* 12:35:19, no GGA follows */
    feed_body(&p, &c, "GPRMC,123520.00,A,4807.0380,N,01131.0000,E,022.4,084.4,230326,,,A");
    TT_ASSERT_EQ(1, c.n_fix); /* the first epoch, incomplete */
    TT_ASSERT(c.ev[1].kind == NMEA_EV_FIX && c.ev[1].have_rmc && !c.ev[1].have_gga);
    TT_ASSERT_EQ(1774269319000LL, c.ev[1].fix.utc_unix_ms);
    TT_ASSERT_EQ(0, c.ev[1].fix.fix_quality);
    TT_ASSERT_EQ(0, c.ev[1].fix.sats);
    TT_ASSERT_EQ(0x07, c.ev[1].fix.flags); /* position from RMC, no altitude */
    TT_ASSERT_EQ(NMEA_EV_RMC, c.ev[2].kind);
    nmea_flush(&p, on_ev, &c);
    TT_ASSERT_EQ(2, c.n_fix);
    nmea_flush(&p, on_ev, &c); /* nothing pending */
    TT_ASSERT_EQ(2, c.n_fix);
}

static void test_gga_only_epoch(void)
{
    nmea_t p;
    col_t c = {0};
    nmea_init(&p);
    feed_body(&p, &c, GGA1);
    feed_body(&p, &c, "GPGGA,123520.00,4807.0380,N,01131.0000,E,1,08,0.90,545.4,M,46.9,M,,");
    TT_ASSERT_EQ(1, c.n_fix);
    const nmea_event_t *e = last_fix(&c);
    TT_ASSERT(e && !e->have_rmc && e->have_gga);
    TT_ASSERT_EQ(0, e->fix.utc_unix_ms); /* no date without an RMC */
    TT_ASSERT_EQ(GPS_FLAG_TIME_VALID | GPS_FLAG_POS_VALID | GPS_FLAG_ALT_VALID, e->fix.flags);
    TT_ASSERT_EQ(481173000, e->fix.lat_e7); /* position falls back to GGA */
}

static void test_no_fix_empty_fields(void)
{
    nmea_t p;
    col_t c = {0};
    nmea_init(&p);
    feed_body(&p, &c, "GPRMC,,V,,,,,,,,,,N");
    feed_body(&p, &c, "GPGGA,,,,,,0,00,99.99,,,,,,");
    TT_ASSERT_EQ(1, c.n_fix); /* empty time keys match */
    const nmea_event_t *e = last_fix(&c);
    TT_ASSERT(e != NULL);
    if (!e) return;
    TT_ASSERT_EQ(0, e->fix.flags);
    TT_ASSERT_EQ(0, e->fix.utc_unix_ms);
    TT_ASSERT_EQ(0, e->fix.lat_e7);
    TT_ASSERT_EQ(0, e->fix.sats);
    TT_ASSERT_EQ(0, e->fix.fix_quality);
    TT_ASSERT_EQ(9999, e->fix.hdop_x100); /* HDOP is reported even without a fix */
    TT_ASSERT_EQ(0, p.bad_lines);
    /* Repeated no-time epochs do not merge into one. */
    feed_body(&p, &c, "GPRMC,,V,,,,,,,,,,N");
    feed_body(&p, &c, "GPRMC,,V,,,,,,,,,,N");
    TT_ASSERT_EQ(2, c.n_fix);
}

static void test_status_v_with_time_not_trusted(void)
{
    /* Cold-start receiver: time/date present but status V (GPS epoch date). */
    nmea_t p;
    col_t c = {0};
    nmea_init(&p);
    feed_body(&p, &c, "GPRMC,000012.00,V,,,,,,,060180,,,N");
    TT_ASSERT_EQ(1, c.n_rmc);
    TT_ASSERT_EQ(0, rmc_ev(&c, 0)->fix.flags);
    TT_ASSERT_EQ(0, rmc_ev(&c, 0)->fix.utc_unix_ms);
    /* Mode N with status A is not valid either. */
    feed_body(&p, &c, "GPRMC,123519.00,A,4807.0380,N,01131.0000,E,0.0,0.0,230326,,,N");
    TT_ASSERT_EQ(0, rmc_ev(&c, 1)->fix.flags);
}

static void test_time_without_date_or_bad_date(void)
{
    nmea_t p;
    col_t c = {0};
    nmea_init(&p);
    feed_body(&p, &c, "GPRMC,123519.00,A,4807.0380,N,01131.0000,E,1.0,2.0,,,,A");
    TT_ASSERT_EQ(GPS_FLAG_TIME_VALID | GPS_FLAG_POS_VALID, rmc_ev(&c, 0)->fix.flags);
    TT_ASSERT_EQ(0, rmc_ev(&c, 0)->fix.utc_unix_ms);
    feed_body(&p, &c, "GPRMC,123519.00,A,4807.0380,N,01131.0000,E,1.0,2.0,310226,,,A"); /* 31 Feb */
    TT_ASSERT_EQ(0, rmc_ev(&c, 1)->fix.utc_unix_ms);
    TT_ASSERT(!(rmc_ev(&c, 1)->fix.flags & GPS_FLAG_DATE_VALID));
    feed_body(&p, &c, "GPRMC,246100.00,A,4807.0380,N,01131.0000,E,1.0,2.0,230326,,,A"); /* hour 24 */
    TT_ASSERT(!(rmc_ev(&c, 2)->fix.flags & GPS_FLAG_TIME_VALID));
    feed_body(&p, &c, "GPRMC,235960.00,A,4807.0380,N,01131.0000,E,1.0,2.0,230326,,,A"); /* leap second */
    TT_ASSERT(!(rmc_ev(&c, 3)->fix.flags & GPS_FLAG_TIME_VALID));
}

static void test_pre_2025_date_rejected(void)
{
    nmea_t p;
    col_t c = {0};
    nmea_init(&p);
    /* week-rollover clone: status A but date 2005 */
    feed_body(&p, &c, "GPRMC,123519.00,A,4807.0380,N,01131.0000,E,1.0,2.0,230305,,,A");
    TT_ASSERT(!(rmc_ev(&c, 0)->fix.flags & (GPS_FLAG_DATE_VALID | GPS_FLAG_TIME_VALID)));
    TT_ASSERT_EQ(0, rmc_ev(&c, 0)->fix.utc_unix_ms);
    feed_body(&p, &c, "GPRMC,123519.00,A,4807.0380,N,01131.0000,E,1.0,2.0,311224,,,A"); /* 2024 */
    TT_ASSERT(!(rmc_ev(&c, 1)->fix.flags & GPS_FLAG_DATE_VALID));
    feed_body(&p, &c, "GPRMC,123519.00,A,4807.0380,N,01131.0000,E,1.0,2.0,010125,,,A"); /* 2025 ok */
    TT_ASSERT(rmc_ev(&c, 2)->fix.flags & GPS_FLAG_DATE_VALID);
}

static void test_midnight_rollover(void)
{
    nmea_t p;
    col_t c = {0};
    nmea_init(&p);
    feed_body(&p, &c, "GPRMC,235959.00,A,4807.0380,N,01131.0000,E,0.0,0.0,311226,,,A");
    feed_body(&p, &c, "GPGGA,235959.00,4807.0380,N,01131.0000,E,1,08,0.90,545.4,M,46.9,M,,");
    feed_body(&p, &c, "GPRMC,000000.00,A,4807.0380,N,01131.0000,E,0.0,0.0,010127,,,A");
    feed_body(&p, &c, "GPGGA,000000.00,4807.0380,N,01131.0000,E,1,08,0.90,545.4,M,46.9,M,,");
    TT_ASSERT_EQ(2, c.n_fix);
    const nmea_event_t *e1 = NULL, *e2 = NULL;
    for (int i = 0; i < c.n; i++) {
        if (c.ev[i].kind == NMEA_EV_FIX) {
            if (e1) e2 = &c.ev[i]; else e1 = &c.ev[i];
        }
    }
    TT_ASSERT(e1 && e2);
    if (!e1 || !e2) return;
    TT_ASSERT_EQ(1798761599000LL, e1->fix.utc_unix_ms);
    TT_ASSERT_EQ(1798761600000LL, e2->fix.utc_unix_ms);
    TT_ASSERT_EQ(1000, e2->fix.utc_unix_ms - e1->fix.utc_unix_ms);
}

static void test_leap_day(void)
{
    nmea_t p;
    col_t c = {0};
    nmea_init(&p);
    feed_body(&p, &c, "GPRMC,060708.25,A,4807.0380,N,01131.0000,E,0.0,0.0,290228,,,A");
    TT_ASSERT_EQ(1835417228250LL, rmc_ev(&c, 0)->fix.utc_unix_ms); /* fraction -> ms */
    feed_body(&p, &c, "GPRMC,000000.00,A,4807.0380,N,01131.0000,E,0.0,0.0,010328,,,A");
    TT_ASSERT_EQ(1835481600000LL, rmc_ev(&c, 1)->fix.utc_unix_ms);
    feed_body(&p, &c, "GPRMC,060708.00,A,4807.0380,N,01131.0000,E,0.0,0.0,290229,,,A"); /* 2029 not leap */
    TT_ASSERT(!(rmc_ev(&c, 2)->fix.flags & GPS_FLAG_DATE_VALID));
}

static void test_hemispheres(void)
{
    nmea_t p;
    col_t c = {0};
    nmea_init(&p);
    feed_body(&p, &c, "GNRMC,010203.00,A,3352.1234,S,15112.5000,W,0.00,000.00,230326,,,A");
    TT_ASSERT_EQ(-338687233, c.ev[0].fix.lat_e7);
    TT_ASSERT_EQ(-1512083333, c.ev[0].fix.lon_e7);
    TT_ASSERT(c.ev[0].fix.flags & GPS_FLAG_POS_VALID);
    feed_body(&p, &c, "GNGGA,010203.00,3352.1234,S,15112.5000,W,2,12,0.80,-12.3,M,0.0,M,,");
    TT_ASSERT_EQ(1, c.n_fix); /* GN talker, same time -> completes */
    const nmea_event_t *e = last_fix(&c);
    TT_ASSERT(e != NULL);
    if (!e) return;
    TT_ASSERT_EQ(-1230, e->fix.alt_cm); /* negative altitude */
    TT_ASSERT_EQ(2, e->fix.fix_quality);
    TT_ASSERT_EQ(12, e->fix.sats);
    TT_ASSERT_EQ(-338687233, e->fix.lat_e7);
}

static void test_max_values(void)
{
    nmea_t p;
    col_t c = {0};
    nmea_init(&p);
    feed_body(&p, &c, "GPRMC,235959.99,A,9000.0000,N,18000.0000,E,99999.99,360.00,311299,,,A");
    TT_ASSERT_EQ(900000000, rmc_ev(&c, 0)->fix.lat_e7);
    TT_ASSERT_EQ(1800000000, rmc_ev(&c, 0)->fix.lon_e7);
    TT_ASSERT_EQ(65535, rmc_ev(&c, 0)->fix.speed_cmps); /* saturated */
    TT_ASSERT_EQ(36000, rmc_ev(&c, 0)->fix.course_cdeg);
    TT_ASSERT_EQ(4102444799990LL, rmc_ev(&c, 0)->fix.utc_unix_ms);
    feed_body(&p, &c, "GPRMC,235959.99,A,9000.0000,S,18000.0000,W,0.0,0.0,311299,,,A");
    TT_ASSERT_EQ(-900000000, rmc_ev(&c, 1)->fix.lat_e7);
    TT_ASSERT_EQ(-1800000000, rmc_ev(&c, 1)->fix.lon_e7);
    /* Beyond the limits: position not valid. */
    feed_body(&p, &c, "GPRMC,235959.99,A,9000.0001,N,18000.0000,E,0.0,0.0,311299,,,A");
    TT_ASSERT(!(rmc_ev(&c, 2)->fix.flags & GPS_FLAG_POS_VALID));
    feed_body(&p, &c, "GPRMC,235959.99,A,4860.0000,N,01131.0000,E,0.0,0.0,311299,,,A"); /* minutes 60 */
    TT_ASSERT(!(rmc_ev(&c, 3)->fix.flags & GPS_FLAG_POS_VALID));
    feed_body(&p, &c, "GPRMC,235959.99,A,4807.0380,X,01131.0000,E,0.0,0.0,311299,,,A"); /* bad hemisphere */
    TT_ASSERT(!(rmc_ev(&c, 4)->fix.flags & GPS_FLAG_POS_VALID));
    feed_body(&p, &c, "GPRMC,235959.99,A,48070380,N,01131.0000,E,0.0,0.0,311299,,,A"); /* wrong digits */
    TT_ASSERT(!(rmc_ev(&c, 5)->fix.flags & GPS_FLAG_POS_VALID));
    feed_body(&p, &c, "GPRMC,235959.99,A,4807.0380,N,01131.0000,E,0.0,361.00,311299,,,A");
    TT_ASSERT_EQ(0, rmc_ev(&c, 6)->fix.course_cdeg);
    /* GGA extremes: altitude and HDOP saturate, sats clamp. */
    feed_body(&p, &c, "GPGGA,235959.99,4807.0380,N,01131.0000,E,8,999,999999.99,999999999.99,M,0,M,,");
    nmea_flush(&p, on_ev, &c);
    const nmea_event_t *e = last_fix(&c);
    TT_ASSERT(e != NULL);
    if (!e) return;
    TT_ASSERT_EQ(65535, e->fix.hdop_x100);
    TT_ASSERT_EQ(255, e->fix.sats);
    TT_ASSERT_EQ(INT32_MAX, e->fix.alt_cm);
}

static void test_speed_conversion(void)
{
    nmea_t p;
    col_t c = {0};
    nmea_init(&p);
    feed_body(&p, &c, "GPRMC,010203.00,A,4807.0380,N,01131.0000,E,0.50,0.00,230326,,,A");
    TT_ASSERT_EQ(26, rmc_ev(&c, 0)->fix.speed_cmps); /* 25.72 cm/s */
    feed_body(&p, &c, "GPRMC,010204.00,A,4807.0380,N,01131.0000,E,1.00,0.00,230326,,,A");
    TT_ASSERT_EQ(51, rmc_ev(&c, 1)->fix.speed_cmps); /* 51.44 */
    feed_body(&p, &c, "GPRMC,010205.00,A,4807.0380,N,01131.0000,E,,,230326,,,A");
    TT_ASSERT_EQ(0, rmc_ev(&c, 2)->fix.speed_cmps);
    TT_ASSERT_EQ(0, p.bad_lines);
}

static void test_bad_checksum_and_format(void)
{
    nmea_t p;
    col_t c = {0};
    char b[160];
    nmea_init(&p);
    mk(b, RMC1);
    b[strlen(b) - 3] ^= 1; /* corrupt the last hex digit */
    feed_str(&p, &c, b);
    TT_ASSERT_EQ(1, p.bad_lines);
    TT_ASSERT_EQ(0, c.n);

    feed_str(&p, &c, "$GPRMC,123519.00,A,4807.0380,N,01131.0000,E,022.4,084.4,230326,,,A\r\n");
    TT_ASSERT_EQ(2, p.bad_lines); /* missing checksum */

    mk(b, RMC1);
    b[strlen(b) - 4] = '\0'; /* "...*5" truncated */
    feed_str(&p, &c, b);
    feed_str(&p, &c, "\r\n");
    TT_ASSERT_EQ(3, p.bad_lines);

    feed_str(&p, &c, "$GPRMC,123519.00,A,4807.0380,N*00\r\n"); /* wrong checksum */
    TT_ASSERT_EQ(4, p.bad_lines);
    feed_str(&p, &c, "$*00\r\n"); /* empty body, checksum 00 matches, unknown talker */
    TT_ASSERT_EQ(4, p.bad_lines);
    TT_ASSERT_EQ(1, p.good_lines);
    feed_str(&p, &c, "$GPRMC,123519.00,A*GG\r\n"); /* non-hex checksum */
    TT_ASSERT_EQ(5, p.bad_lines);
    feed_body(&p, &c, "GPRMC,123519.00,A,4807.0380,N"); /* valid checksum, too few fields */
    TT_ASSERT_EQ(6, p.bad_lines);
    feed_body(&p, &c, "GPGGA,123519.00");
    TT_ASSERT_EQ(7, p.bad_lines);
    feed_body(&p, &c, "GPRMC,12\x01" "519.00,A,,,,,,,,,,"); /* control character in the body */
    TT_ASSERT_EQ(8, p.bad_lines);
    TT_ASSERT_EQ(0, c.n);
    TT_ASSERT_EQ(1, p.good_lines);
}

static void test_lowercase_checksum(void)
{
    nmea_t p;
    col_t c = {0};
    char b[160];
    nmea_init(&p);
    int tested = 0;
    /* Bodies whose checksum contains a hex letter, sent with lowercase hex. */
    for (int i = 0; i < 40 && tested < 3; i++) {
        char body[64];
        sprintf(body, "GPTXT,%d", i);
        uint8_t x = 0;
        for (const char *s = body; *s; s++) x ^= (uint8_t)*s;
        if ((x >> 4) >= 10 || (x & 15) >= 10) {
            sprintf(b, "$%s*%02x\r\n", body, x);
            feed_str(&p, &c, b);
            tested++;
        }
    }
    TT_ASSERT_EQ(3, tested);
    TT_ASSERT_EQ(3, p.good_lines);
    TT_ASSERT_EQ(0, p.bad_lines);
    /* A real RMC with lowercase hex still decodes. */
    const char *body = "GPRMC,123519.00,A,4807.0380,N,01131.0000,E,022.4,084.4,230326,,,A";
    uint8_t x = 0;
    for (const char *s = body; *s; s++) x ^= (uint8_t)*s;
    sprintf(b, "$%s*%02x\r\n", body, x);
    feed_str(&p, &c, b);
    TT_ASSERT_EQ(1, c.n_rmc);
    TT_ASSERT_EQ(0, p.bad_lines);
}

static void test_overlong_line(void)
{
    nmea_t p;
    col_t c = {0};
    nmea_init(&p);
    /* Exactly NMEA_MAX_LINE characters ("$" ... "*HH") is accepted. */
    char body[NMEA_MAX_LINE];
    memset(body, 'A', sizeof(body));
    size_t blen = NMEA_MAX_LINE - 4; /* '$' + body + '*' + 2 hex */
    body[blen] = '\0';
    memcpy(body, "GPTXT,", 6);
    char b[256];
    size_t n = mk(b, body);
    TT_ASSERT_EQ(NMEA_MAX_LINE + 2, n);
    feed_str(&p, &c, b);
    TT_ASSERT_EQ(1, p.good_lines);
    TT_ASSERT_EQ(0, p.dropped_lines);
    /* One more character: dropped, counted once, parser recovers on the next line. */
    body[blen] = 'A';
    body[blen + 1] = '\0';
    mk(b, body);
    feed_str(&p, &c, b);
    TT_ASSERT_EQ(1, p.dropped_lines);
    TT_ASSERT_EQ(1, p.good_lines);
    TT_ASSERT_EQ(0, p.bad_lines);
    /* A very long garbage line without terminator, then a good pair. */
    feed_str(&p, &c, "$");
    for (int i = 0; i < 5000; i++) {
        nmea_feed(&p, (const uint8_t *)"x", 1, on_ev, &c);
    }
    TT_ASSERT_EQ(2, p.dropped_lines);
    feed_str(&p, &c, "\r\n");
    feed_body(&p, &c, RMC1);
    feed_body(&p, &c, GGA1);
    TT_ASSERT_EQ(1, c.n_fix);
    TT_ASSERT_EQ(2, p.dropped_lines);
    /* A '$' in skip mode starts a new line (and does not double count). */
    feed_str(&p, &c, "$GPTXT,");
    for (int i = 0; i < 100; i++) {
        nmea_feed(&p, (const uint8_t *)"y", 1, on_ev, &c);
    }
    TT_ASSERT_EQ(3, p.dropped_lines);
    feed_body(&p, &c, "GPRMC,123520.00,A,4807.0380,N,01131.0000,E,022.4,084.4,230326,,,A");
    TT_ASSERT_EQ(3, p.dropped_lines);
    TT_ASSERT_EQ(2, c.n_rmc);
}

static void test_garbage_between_lines(void)
{
    nmea_t p;
    col_t c = {0};
    nmea_init(&p);
    static const uint8_t junk[] = {0xFF, 0x00, 0x7F, 'a', '*', ',', 0x80, '\n', '\r', 0xE0};
    nmea_feed(&p, junk, sizeof(junk), on_ev, &c);
    feed_body(&p, &c, RMC1);
    nmea_feed(&p, junk, sizeof(junk), on_ev, &c);
    feed_body(&p, &c, GGA1);
    nmea_feed(&p, junk, sizeof(junk), on_ev, &c);
    TT_ASSERT_EQ(1, c.n_fix);
    TT_ASSERT_EQ(0, p.bad_lines);
    TT_ASSERT_EQ(0, p.dropped_lines);
    /* A truncated line followed directly by a new '$' is dropped and counted. */
    feed_str(&p, &c, "$GPRMC,123521.00,A,4807.03$GPTXT,ok");
    feed_str(&p, &c, "\r\n");
    TT_ASSERT_EQ(1, p.dropped_lines);
    TT_ASSERT_EQ(1, p.bad_lines); /* "$GPTXT,ok" has no checksum */
    /* A line cut mid-way by garbage including a CR is a bad line, not a crash. */
    feed_str(&p, &c, "$GPGGA,1235\r");
    TT_ASSERT_EQ(2, p.bad_lines);
}

static void test_unknown_ignored(void)
{
    nmea_t p;
    col_t c = {0};
    nmea_init(&p);
    feed_body(&p, &c, "GPVTG,084.4,T,,M,022.4,N,041.5,K,A");
    feed_body(&p, &c, "GPGSA,A,3,04,05,,09,12,,,24,,,,,2.5,1.3,2.1");
    feed_body(&p, &c, "GPGSV,3,1,11,03,03,111,00,04,15,270,00,06,01,010,00,13,06,292,00");
    feed_body(&p, &c, "GPGLL,4807.038,N,01131.000,E,123519,A,A");
    feed_body(&p, &c, "GLRMC,123519.00,A,4807.0380,N,01131.0000,E,022.4,084.4,230326,,,A"); /* GLONASS talker */
    feed_body(&p, &c, "BDGGA,123519.00,4807.0380,N,01131.0000,E,1,08,0.90,545.4,M,46.9,M,,");
    feed_body(&p, &c, "GPRMCX,123519.00,A,4807.0380,N,01131.0000,E,022.4,084.4,230326,,,A");
    feed_body(&p, &c, "PUBX,00,whatever");
    TT_ASSERT_EQ(0, c.n);
    TT_ASSERT_EQ(8, p.good_lines);
    TT_ASSERT_EQ(0, p.bad_lines);
}

static void test_chunk_boundaries(void)
{
    char stream[1024];
    size_t n = 0;
    n += mk(stream + n, RMC1);
    n += mk(stream + n, "GPVTG,084.4,T,,M,022.4,N,041.5,K,A");
    n += mk(stream + n, GGA1);
    n += mk(stream + n, "GPRMC,123520.00,A,4807.0380,N,01131.0000,E,022.4,084.4,230326,,,A");
    n += mk(stream + n, "GPGGA,123520.00,4807.0380,N,01131.0000,E,1,08,0.90,545.4,M,46.9,M,,");

    nmea_t pr;
    col_t ref = {0};
    nmea_init(&pr);
    nmea_feed(&pr, (const uint8_t *)stream, n, on_ev, &ref);
    TT_ASSERT_EQ(2, ref.n_fix);

    for (size_t chunk = 1; chunk <= 41; chunk++) {
        nmea_t p;
        col_t c = {0};
        nmea_init(&p);
        for (size_t i = 0; i < n; i += chunk) {
            size_t m = n - i < chunk ? n - i : chunk;
            nmea_feed(&p, (const uint8_t *)stream + i, m, on_ev, &c);
        }
        TT_ASSERT_EQ(ref.n, c.n);
        TT_ASSERT_EQ(ref.n_fix, c.n_fix);
        TT_ASSERT_EQ(pr.good_lines, p.good_lines);
        TT_ASSERT_EQ(0, p.bad_lines);
        for (int i = 0; i < c.n && i < ref.n; i++) {
            TT_ASSERT(memcmp(&c.ev[i].fix, &ref.ev[i].fix, sizeof(c.ev[i].fix)) == 0 &&
                      c.ev[i].kind == ref.ev[i].kind);
        }
    }
}

static void test_crlf_variants(void)
{
    nmea_t p;
    col_t c = {0};
    char b[160];
    nmea_init(&p);
    mk(b, RMC1);
    b[strlen(b) - 2] = '\0'; /* LF only */
    feed_str(&p, &c, b);
    feed_str(&p, &c, "\n");
    mk(b, GGA1);
    b[strlen(b) - 1] = '\0'; /* CR only */
    feed_str(&p, &c, b);
    TT_ASSERT_EQ(1, c.n_fix);
    TT_ASSERT_EQ(0, p.bad_lines);
}

int main(void)
{
    TT_RUN(test_days_from_civil);
    TT_RUN(test_nominal_epoch);
    TT_RUN(test_literal_sentences);
    TT_RUN(test_gga_first_order);
    TT_RUN(test_new_second_emits_partial);
    TT_RUN(test_gga_only_epoch);
    TT_RUN(test_no_fix_empty_fields);
    TT_RUN(test_status_v_with_time_not_trusted);
    TT_RUN(test_time_without_date_or_bad_date);
    TT_RUN(test_pre_2025_date_rejected);
    TT_RUN(test_midnight_rollover);
    TT_RUN(test_leap_day);
    TT_RUN(test_hemispheres);
    TT_RUN(test_max_values);
    TT_RUN(test_speed_conversion);
    TT_RUN(test_bad_checksum_and_format);
    TT_RUN(test_lowercase_checksum);
    TT_RUN(test_overlong_line);
    TT_RUN(test_garbage_between_lines);
    TT_RUN(test_unknown_ignored);
    TT_RUN(test_chunk_boundaries);
    TT_RUN(test_crlf_variants);
    return TT_RESULT();
}
