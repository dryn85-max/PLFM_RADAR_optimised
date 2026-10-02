/* Host tests for the 0x0060..0x00A4 encoders/decoders of ld2410_cmd and for ld_settings.
 * Frames are the examples of the HLK-LD2410C protocol document (pages cited per test). */
#include <errno.h>
#include <stdio.h>
#include <string.h>
#include "tinytest.h"
#include "ld2410_cmd.h"
#include "ld_settings.h"

#define HDR 0xFD, 0xFC, 0xFB, 0xFA
#define FTR 0x04, 0x03, 0x02, 0x01

static void expect_frame(int n, const uint8_t *out, const uint8_t *exp, size_t elen)
{
    TT_ASSERT_EQ(elen, n);
    if (n == (int)elen) TT_ASSERT(memcmp(out, exp, elen) == 0);
}

static void test_encoders_golden(void)
{
    uint8_t o[LD_CMD_MAX_FRAME];
    /* p.10 2.2.3: gates 8/8, duration 5 s */
    static const uint8_t g[] = {HDR, 0x14, 0x00, 0x60, 0x00, 0x00, 0x00, 0x08, 0x00, 0x00, 0x00,
        0x01, 0x00, 0x08, 0x00, 0x00, 0x00, 0x02, 0x00, 0x05, 0x00, 0x00, 0x00, FTR};
    expect_frame(ld_cmd_encode_set_gates(8, 8, 5, o, sizeof o), o, g, sizeof g);
    TT_ASSERT_EQ(LD_CMD_MAX_FRAME, sizeof g);
    /* p.11 2.2.4 */
    static const uint8_t rp[] = {HDR, 0x02, 0x00, 0x61, 0x00, FTR};
    expect_frame(ld_cmd_encode_read_params(o, sizeof o), o, rp, sizeof rp);
    /* p.13 2.2.7: gate 3, move 40, still 40 */
    static const uint8_t s3[] = {HDR, 0x14, 0x00, 0x64, 0x00, 0x00, 0x00, 0x03, 0x00, 0x00, 0x00,
        0x01, 0x00, 0x28, 0x00, 0x00, 0x00, 0x02, 0x00, 0x28, 0x00, 0x00, 0x00, FTR};
    expect_frame(ld_cmd_encode_set_sens(3, 40, 40, o, sizeof o), o, s3, sizeof s3);
    /* p.13 2.2.7: all gates */
    static const uint8_t sa[] = {HDR, 0x14, 0x00, 0x64, 0x00, 0x00, 0x00, 0xFF, 0xFF, 0x00, 0x00,
        0x01, 0x00, 0x28, 0x00, 0x00, 0x00, 0x02, 0x00, 0x28, 0x00, 0x00, 0x00, FTR};
    expect_frame(ld_cmd_encode_set_sens(LD_GATE_ALL, 40, 40, o, sizeof o), o, sa, sizeof sa);
    /* p.13 2.2.8, p.14 2.2.10, p.15 2.2.11 */
    static const uint8_t rv[] = {HDR, 0x02, 0x00, 0xA0, 0x00, FTR};
    static const uint8_t fr[] = {HDR, 0x02, 0x00, 0xA2, 0x00, FTR};
    static const uint8_t rs[] = {HDR, 0x02, 0x00, 0xA3, 0x00, FTR};
    expect_frame(ld_cmd_encode_read_version(o, sizeof o), o, rv, sizeof rv);
    expect_frame(ld_cmd_encode_factory_reset(o, sizeof o), o, fr, sizeof fr);
    expect_frame(ld_cmd_encode_restart(o, sizeof o), o, rs, sizeof rs);
    /* p.16 2.2.12: the example shows "on" (01 00); "off" is value 00 00 */
    static const uint8_t bt[] = {HDR, 0x04, 0x00, 0xA4, 0x00, 0x00, 0x00, FTR};
    expect_frame(ld_cmd_encode_bluetooth_off(o, sizeof o), o, bt, sizeof bt);
}

static void test_encoder_ranges(void)
{
    uint8_t o[LD_CMD_MAX_FRAME];
    TT_ASSERT_EQ(-EINVAL, ld_cmd_encode_set_gates(1, 8, 5, o, sizeof o));
    TT_ASSERT_EQ(2, ld_cmd_encode_set_gates(2, 8, 5, o, sizeof o) > 0 ? 2 : 0);
    TT_ASSERT(ld_cmd_encode_set_gates(8, 2, 0, o, sizeof o) > 0);
    TT_ASSERT_EQ(-EINVAL, ld_cmd_encode_set_gates(9, 8, 5, o, sizeof o));
    TT_ASSERT_EQ(-EINVAL, ld_cmd_encode_set_gates(8, 1, 5, o, sizeof o));
    TT_ASSERT_EQ(-EINVAL, ld_cmd_encode_set_gates(8, 9, 5, o, sizeof o));
    TT_ASSERT_EQ(-EINVAL, ld_cmd_encode_set_gates(0, 8, 5, o, sizeof o));
    TT_ASSERT_EQ(-EINVAL, ld_cmd_encode_set_gates(-1, 8, 5, o, sizeof o));
    TT_ASSERT(ld_cmd_encode_set_gates(8, 8, 65535, o, sizeof o) > 0);
    TT_ASSERT_EQ(0xFF, o[8 + 12 + 2]);
    TT_ASSERT_EQ(0xFF, o[8 + 12 + 3]);
    TT_ASSERT_EQ(-EINVAL, ld_cmd_encode_set_gates(8, 8, 65536, o, sizeof o));
    TT_ASSERT_EQ(-EINVAL, ld_cmd_encode_set_gates(8, 8, -1, o, sizeof o));
    TT_ASSERT(ld_cmd_encode_set_sens(0, 0, 0, o, sizeof o) > 0);
    TT_ASSERT(ld_cmd_encode_set_sens(8, 100, 100, o, sizeof o) > 0);
    TT_ASSERT_EQ(-EINVAL, ld_cmd_encode_set_sens(9, 40, 40, o, sizeof o));
    TT_ASSERT_EQ(-EINVAL, ld_cmd_encode_set_sens(-1, 40, 40, o, sizeof o));
    TT_ASSERT_EQ(-EINVAL, ld_cmd_encode_set_sens(0xFFFE, 40, 40, o, sizeof o));
    TT_ASSERT_EQ(-EINVAL, ld_cmd_encode_set_sens(0x10000, 40, 40, o, sizeof o));
    TT_ASSERT_EQ(-EINVAL, ld_cmd_encode_set_sens(3, 101, 40, o, sizeof o));
    TT_ASSERT_EQ(-EINVAL, ld_cmd_encode_set_sens(3, 40, 101, o, sizeof o));
    TT_ASSERT_EQ(-EINVAL, ld_cmd_encode_set_sens(3, -1, 40, o, sizeof o));
    TT_ASSERT_EQ(-EINVAL, ld_cmd_encode_set_sens(3, 40, -1, o, sizeof o));
}

static void test_encoder_caps(void)
{
    uint8_t g[40];
    for (size_t cap = 0; cap < 30; cap++) {
        memset(g, 0xEE, sizeof g);
        TT_ASSERT_EQ(-ENOSPC, ld_cmd_encode_set_gates(8, 8, 5, g, cap));
        TT_ASSERT_EQ(-ENOSPC, ld_cmd_encode_set_sens(3, 1, 1, g, cap));
        for (size_t i = cap; i < sizeof g; i++) TT_ASSERT(g[i] == 0xEE);
    }
    TT_ASSERT_EQ(-ENOSPC, ld_cmd_encode_read_params(g, 11));
    TT_ASSERT_EQ(12, ld_cmd_encode_read_params(g, 12));
    TT_ASSERT_EQ(-ENOSPC, ld_cmd_encode_read_version(g, 11));
    TT_ASSERT_EQ(-ENOSPC, ld_cmd_encode_factory_reset(g, 11));
    TT_ASSERT_EQ(-ENOSPC, ld_cmd_encode_restart(g, 11));
    TT_ASSERT_EQ(-ENOSPC, ld_cmd_encode_bluetooth_off(g, 13));
    TT_ASSERT_EQ(14, ld_cmd_encode_bluetooth_off(g, 14));
    TT_ASSERT_EQ(-EINVAL, ld_cmd_encode_restart(NULL, 20));
}

/* p.11 2.2.4 ACK payload: bytes 7..34 of the frame */
static const uint8_t PARAMS_ACK[28] = {
    0x61, 0x01, 0x00, 0x00, 0xAA, 0x08, 0x08, 0x08,
    0x14, 0x14, 0x14, 0x14, 0x14, 0x14, 0x14, 0x14, 0x14,
    0x19, 0x19, 0x19, 0x19, 0x19, 0x19, 0x19, 0x19, 0x19,
    0x05, 0x00};

static void test_params_decode(void)
{
    ld_params_t p;
    TT_ASSERT_EQ(0, ld_cmd_decode_params(PARAMS_ACK, sizeof PARAMS_ACK, &p));
    TT_ASSERT_EQ(8, p.max_gate_n);
    TT_ASSERT_EQ(8, p.max_move_gate);
    TT_ASSERT_EQ(8, p.max_still_gate);
    for (int g = 0; g < 9; g++) {
        TT_ASSERT_EQ(20, p.move_sens[g]);
        TT_ASSERT_EQ(25, p.still_sens[g]);
    }
    TT_ASSERT_EQ(5, p.duration_s);

    uint8_t b[40];
    /* distinct values: field order and little-endian duration */
    memcpy(b, PARAMS_ACK, 28);
    b[6] = 7; b[7] = 3; b[8] = 1; b[16] = 9; b[17] = 11; b[25] = 99; b[26] = 0x34; b[27] = 0x12;
    TT_ASSERT_EQ(0, ld_cmd_decode_params(b, 28, &p));
    TT_ASSERT_EQ(7, p.max_move_gate);
    TT_ASSERT_EQ(3, p.max_still_gate);
    TT_ASSERT_EQ(1, p.move_sens[0]);
    TT_ASSERT_EQ(9, p.move_sens[8]);
    TT_ASSERT_EQ(11, p.still_sens[0]);
    TT_ASSERT_EQ(99, p.still_sens[8]);
    TT_ASSERT_EQ(0x1234, p.duration_s);

    TT_ASSERT_EQ(-EINVAL, ld_cmd_decode_params(NULL, 28, &p));
    TT_ASSERT_EQ(-EINVAL, ld_cmd_decode_params(PARAMS_ACK, 28, NULL));
    for (size_t l = 0; l < 4; l++) TT_ASSERT_EQ(-EMSGSIZE, ld_cmd_decode_params(PARAMS_ACK, l, &p));
    for (size_t l = 4; l < 28; l++) {
        int r = ld_cmd_decode_params(PARAMS_ACK, l, &p);
        TT_ASSERT_EQ(-EMSGSIZE, r);
    }
    memcpy(b, PARAMS_ACK, 28); b[28] = 0;
    TT_ASSERT_EQ(-EMSGSIZE, ld_cmd_decode_params(b, 29, &p));
    memcpy(b, PARAMS_ACK, 28); b[4] = 0xAB;
    TT_ASSERT_EQ(-EBADMSG, ld_cmd_decode_params(b, 28, &p));
    memcpy(b, PARAMS_ACK, 28); b[2] = 1; /* status failure */
    TT_ASSERT_EQ(-EBADMSG, ld_cmd_decode_params(b, 28, &p));
    TT_ASSERT_EQ(-EBADMSG, ld_cmd_decode_params(b, 4, &p)); /* failure ACK is short */
    memcpy(b, PARAMS_ACK, 28); b[3] = 1;
    TT_ASSERT_EQ(-EBADMSG, ld_cmd_decode_params(b, 28, &p));
    memcpy(b, PARAMS_ACK, 28); b[0] = 0x62; /* wrong command */
    TT_ASSERT_EQ(-EBADMSG, ld_cmd_decode_params(b, 28, &p));
    memcpy(b, PARAMS_ACK, 28); b[1] = 0x00; /* no ACK flag */
    TT_ASSERT_EQ(-EBADMSG, ld_cmd_decode_params(b, 28, &p));
    /* N mismatch: N != 8 is rejected whatever the length */
    memcpy(b, PARAMS_ACK, 28); b[5] = 7;
    TT_ASSERT_EQ(-EBADMSG, ld_cmd_decode_params(b, 28, &p));
    TT_ASSERT_EQ(-EBADMSG, ld_cmd_decode_params(b, 26, &p));
    b[5] = 9;
    TT_ASSERT_EQ(-EBADMSG, ld_cmd_decode_params(b, 30, &p));
}

static void test_version(void)
{
    /* p.13 2.2.8: type bytes "00 01" and "07 01", minor 16 15 09 22; the document omits the
     * command word A0 01 although its length field 0x0C counts it */
    static const uint8_t ack[12] = {0xA0, 0x01, 0x00, 0x00, 0x00, 0x01, 0x07, 0x01,
                                    0x16, 0x15, 0x09, 0x22};
    ld_version_t v;
    TT_ASSERT_EQ(0, ld_cmd_decode_version(ack, sizeof ack, &v));
    TT_ASSERT_EQ(0x0100, v.type);
    TT_ASSERT_EQ(0x0107, v.major);
    TT_ASSERT_EQ(0x22091516u, v.minor);
    char s[LD_VERSION_STR_MAX];
    TT_ASSERT_EQ(14, ld_version_format(&v, s, sizeof s));
    TT_ASSERT(strcmp(s, "V1.07.22091516") == 0);

    uint8_t b[16];
    for (size_t l = 0; l < 12; l++) TT_ASSERT(ld_cmd_decode_version(ack, l, &v) < 0);
    TT_ASSERT_EQ(-EMSGSIZE, ld_cmd_decode_version(ack, 3, &v));
    TT_ASSERT_EQ(-EMSGSIZE, ld_cmd_decode_version(ack, 11, &v));
    memcpy(b, ack, 12); b[12] = 0;
    TT_ASSERT_EQ(-EMSGSIZE, ld_cmd_decode_version(b, 13, &v));
    memcpy(b, ack, 12); b[2] = 1;
    TT_ASSERT_EQ(-EBADMSG, ld_cmd_decode_version(b, 12, &v));
    memcpy(b, ack, 12); b[0] = 0xA1;
    TT_ASSERT_EQ(-EBADMSG, ld_cmd_decode_version(b, 12, &v));
    memcpy(b, ack, 12); b[1] = 0;
    TT_ASSERT_EQ(-EBADMSG, ld_cmd_decode_version(b, 12, &v));
    TT_ASSERT_EQ(-EINVAL, ld_cmd_decode_version(NULL, 12, &v));
    TT_ASSERT_EQ(-EINVAL, ld_cmd_decode_version(ack, 12, NULL));
}

static void test_version_format(void)
{
    char s[LD_VERSION_STR_MAX];
    ld_version_t v = {1, 0x0107, 0x22091516u};
    TT_ASSERT_EQ(-ENOSPC, ld_version_format(&v, s, 14)); /* needs 15 with NUL */
    TT_ASSERT_EQ(14, ld_version_format(&v, s, 15));
    TT_ASSERT_EQ(-EINVAL, ld_version_format(&v, s, 0));
    TT_ASSERT_EQ(-EINVAL, ld_version_format(NULL, s, 20));
    v.major = 0xFFFF; v.minor = 0xFFFFFFFFu;
    TT_ASSERT_EQ(16, ld_version_format(&v, s, sizeof s));
    TT_ASSERT(strcmp(s, "V255.ff.ffffffff") == 0);
    v.major = 0; v.minor = 0;
    TT_ASSERT_EQ(14, ld_version_format(&v, s, sizeof s));
    TT_ASSERT(strcmp(s, "V0.00.00000000") == 0);
}

static ld_settings_t defaults(void)
{
    ld_params_t p;
    ld_settings_t s;
    ld_cmd_decode_params(PARAMS_ACK, sizeof PARAMS_ACK, &p);
    ld_settings_from_params(&p, &s);
    return s;
}

static void test_settings_basic(void)
{
    ld_settings_t s = defaults();
    TT_ASSERT_EQ(8, s.max_move_gate);
    TT_ASSERT_EQ(5, s.duration_s);
    TT_ASSERT_EQ(25, s.still_sens[8]);
    TT_ASSERT_EQ(0, ld_settings_check(&s));
    TT_ASSERT_EQ(-EINVAL, ld_settings_check(NULL));
    TT_ASSERT_EQ(-EINVAL, ld_settings_from_params(NULL, &s));
    s.max_move_gate = 1; TT_ASSERT_EQ(-EINVAL, ld_settings_check(&s));
    s.max_move_gate = 9; TT_ASSERT_EQ(-EINVAL, ld_settings_check(&s));
    s.max_move_gate = 2; TT_ASSERT_EQ(0, ld_settings_check(&s));
    s.max_still_gate = 1; TT_ASSERT_EQ(-EINVAL, ld_settings_check(&s));
    s.max_still_gate = 9; TT_ASSERT_EQ(-EINVAL, ld_settings_check(&s));
    s.max_still_gate = 8;
    s.move_sens[0] = 101; TT_ASSERT_EQ(-EINVAL, ld_settings_check(&s));
    s.move_sens[0] = 100; TT_ASSERT_EQ(0, ld_settings_check(&s));
    s.still_sens[2] = 101; TT_ASSERT_EQ(-EINVAL, ld_settings_check(&s));
    s.still_sens[2] = 100;
    s.still_sens[0] = 255; s.still_sens[1] = 200; /* not settable: ignored */
    TT_ASSERT_EQ(0, ld_settings_check(&s));
}

#define FULL_ND "mg=8&sg=7&m0=50&m1=50&m2=40&m3=30&m4=20&m5=15&m6=15&m7=15&m8=15" \
             "&s2=40&s3=40&s4=30&s5=30&s6=20&s7=20&s8=20"
#define FULL "mg=8&sg=7&dur=30&m0=50&m1=50&m2=40&m3=30&m4=20&m5=15&m6=15&m7=15&m8=15" \
             "&s2=40&s3=40&s4=30&s5=30&s6=20&s7=20&s8=20"

static int parse(const char *body, ld_settings_t *out)
{
    ld_settings_t cur = defaults();
    return ld_settings_parse_form(body, strlen(body), &cur, out);
}

static void test_form_ok(void)
{
    ld_settings_t cur = defaults(), o;
    cur.still_sens[0] = 77; cur.still_sens[1] = 66;
    TT_ASSERT_EQ(LDS_OK, ld_settings_parse_form(FULL, strlen(FULL), &cur, &o));
    TT_ASSERT_EQ(8, o.max_move_gate);
    TT_ASSERT_EQ(7, o.max_still_gate);
    TT_ASSERT_EQ(30, o.duration_s);
    TT_ASSERT_EQ(50, o.move_sens[0]);
    TT_ASSERT_EQ(40, o.move_sens[2]);
    TT_ASSERT_EQ(15, o.move_sens[8]);
    TT_ASSERT_EQ(77, o.still_sens[0]);
    TT_ASSERT_EQ(66, o.still_sens[1]);
    TT_ASSERT_EQ(40, o.still_sens[2]);
    TT_ASSERT_EQ(20, o.still_sens[8]);
    TT_ASSERT_EQ(0, ld_settings_check(&o));
    /* no NUL needed: pass exact length of a non-terminated copy */
    char buf[sizeof FULL - 1];
    memcpy(buf, FULL, sizeof buf);
    TT_ASSERT_EQ(LDS_OK, ld_settings_parse_form(buf, sizeof buf, &cur, &o));
    /* order, unknown fields, key without '=', empty segments, trailing '&' */
    TT_ASSERT_EQ(LDS_OK, parse("x=1&junk&&save=Save&" FULL "&", &o));
    TT_ASSERT_EQ(LDS_OK, parse("s8=20&s7=20&s6=20&s5=30&s4=30&s3=40&s2=40&m8=15&m7=15&m6=15&m5=15&"
                               "m4=20&m3=30&m2=40&m1=50&m0=50&dur=0&sg=2&mg=2", &o));
    TT_ASSERT_EQ(0, o.duration_s);
    TT_ASSERT_EQ(2, o.max_move_gate);
    /* leading zeros, 5 chars max, %-encoded digits ('%38' = '8', '%30' = '0') */
    TT_ASSERT_EQ(LDS_OK, parse("mg=%38&sg=08&dur=65535&m0=100&m1=000&m2=0&m3=1&m4=2&m5=3&m6=4&m7=5&m8=6"
                               "&s2=%31%30&s3=1&s4=1&s5=1&s6=1&s7=1&s8=1", &o));
    TT_ASSERT_EQ(8, o.max_move_gate);
    TT_ASSERT_EQ(8, o.max_still_gate);
    TT_ASSERT_EQ(65535, o.duration_s);
    TT_ASSERT_EQ(100, o.move_sens[0]);
    TT_ASSERT_EQ(10, o.still_sens[2]);
    TT_ASSERT_EQ(LDS_OK, parse(FULL "&s0=999&s1=abc", &o)); /* s0, s1 are unknown fields */
    TT_ASSERT_EQ(25, o.still_sens[0]);
}

static void test_form_errors(void)
{
    ld_settings_t o, keep = defaults();
    memset(&o, 0x5A, sizeof o);
    ld_settings_t o_before = o;
    TT_ASSERT_EQ(LDS_E_MISSING, parse("", &o));
    TT_ASSERT_EQ(LDS_E_MISSING, parse("&", &o));
    TT_ASSERT_EQ(LDS_E_MISSING, parse("mg=8", &o));
    TT_ASSERT_EQ(LDS_E_MISSING, parse("no equals sign at all", &o));
    TT_ASSERT_EQ(LDS_E_MISSING, parse("mg&sg&dur", &o));
    TT_ASSERT_EQ(LDS_E_MISSING, parse("m0=1&m1=1&m2=1&m3=1&m4=1&m5=1&m6=1&m7=1&m8=1&s2=1&s3=1&s4=1&s5=1&s6=1&s7=1&s8=1&sg=8", &o));
    TT_ASSERT_EQ(LDS_E_MISSING, parse("mg=8&sg=7&dur=30&m0=50&m1=50&m2=40&m3=30&m4=20&m5=15&m6=15&m7=15&m8=15"
                                      "&s2=40&s3=40&s4=30&s5=30&s6=20&s7=20", &o)); /* s8 missing */
    /* missing each field in turn */
    {
        static const char *keys[] = {"mg","sg","dur","m0","m1","m2","m3","m4","m5","m6","m7","m8",
                                     "s2","s3","s4","s5","s6","s7","s8"};
        for (unsigned skip = 0; skip < 19; skip++) {
            char b[256] = "";
            for (unsigned i = 0; i < 19; i++) {
                if (i == skip) continue;
                strcat(b, keys[i]); strcat(b, "=5&");
                if (i == 0 || i == 1) b[strlen(b) - 2] = '5';
            }
            TT_ASSERT_EQ(LDS_E_MISSING, parse(b, &o));
        }
    }
    TT_ASSERT_EQ(LDS_E_DUPLICATE, parse(FULL "&mg=8", &o));
    TT_ASSERT_EQ(LDS_E_DUPLICATE, parse(FULL "&s8=20", &o));
    TT_ASSERT_EQ(LDS_E_DUPLICATE, parse("mg=8&mg=9&" FULL, &o)); /* dup before range check */
    TT_ASSERT_EQ(LDS_E_BAD_VALUE, parse("mg=&sg=7&dur=30&m0=50&m1=50&m2=40&m3=30&m4=20&m5=15&m6=15&m7=15&m8=15"
                                        "&s2=40&s3=40&s4=30&s5=30&s6=20&s7=20&s8=20", &o));
    TT_ASSERT_EQ(LDS_E_BAD_VALUE, parse("dur=+5&mg=8&sg=7&m0=50&m1=50&m2=40&m3=30&m4=20&m5=15&m6=15&m7=15&m8=15"
                                        "&s2=40&s3=40&s4=30&s5=30&s6=20&s7=20&s8=20", &o)); /* '+' = space */
    TT_ASSERT_EQ(LDS_E_BAD_VALUE, parse("dur=-5&" FULL_ND, &o));
    TT_ASSERT_EQ(LDS_E_BAD_VALUE, parse("dur=%2B5&" FULL_ND, &o));
    TT_ASSERT_EQ(LDS_E_BAD_VALUE, parse("dur=5%20&" FULL_ND, &o));
    TT_ASSERT_EQ(LDS_E_BAD_VALUE, parse("dur=%205&" FULL_ND, &o));
    TT_ASSERT_EQ(LDS_E_BAD_VALUE, parse("dur=5a&" FULL_ND, &o));
    TT_ASSERT_EQ(LDS_E_BAD_VALUE, parse("dur=1.5&" FULL_ND, &o));
    TT_ASSERT_EQ(LDS_E_BAD_VALUE, parse("dur=0x10&" FULL_ND, &o));
    TT_ASSERT_EQ(LDS_E_BAD_VALUE, parse("dur=%&" FULL_ND, &o));
    TT_ASSERT_EQ(LDS_E_BAD_VALUE, parse("dur=%3&" FULL_ND, &o));
    TT_ASSERT_EQ(LDS_E_BAD_VALUE, parse("dur=%zz&" FULL_ND, &o));
    TT_ASSERT_EQ(LDS_E_BAD_VALUE, parse("dur=%00&" FULL_ND, &o));
    TT_ASSERT_EQ(LDS_E_TOO_LONG, parse("dur=123456&" FULL_ND, &o));
    TT_ASSERT_EQ(LDS_E_TOO_LONG, parse("dur=99999999999999999999&" FULL_ND, &o));
    TT_ASSERT_EQ(LDS_E_TOO_LONG, parse("dur=000005&" FULL_ND, &o));
    TT_ASSERT_EQ(LDS_E_RANGE, parse("dur=65536&" FULL_ND, &o));
    TT_ASSERT_EQ(LDS_E_RANGE, parse("dur=99999&" FULL_ND, &o));
    TT_ASSERT_EQ(LDS_E_RANGE, parse("mg=1&sg=7&dur=30&m0=50&m1=50&m2=40&m3=30&m4=20&m5=15&m6=15&m7=15&m8=15"
                                    "&s2=40&s3=40&s4=30&s5=30&s6=20&s7=20&s8=20", &o));
    TT_ASSERT_EQ(LDS_E_RANGE, parse("mg=8&sg=9&dur=30&m0=50&m1=50&m2=40&m3=30&m4=20&m5=15&m6=15&m7=15&m8=15"
                                    "&s2=40&s3=40&s4=30&s5=30&s6=20&s7=20&s8=20", &o));
    TT_ASSERT_EQ(LDS_E_RANGE, parse("mg=0&sg=8&dur=30&m0=50&m1=50&m2=40&m3=30&m4=20&m5=15&m6=15&m7=15&m8=15"
                                    "&s2=40&s3=40&s4=30&s5=30&s6=20&s7=20&s8=20", &o));
    TT_ASSERT_EQ(LDS_E_RANGE, parse("mg=8&sg=7&dur=30&m0=101&m1=50&m2=40&m3=30&m4=20&m5=15&m6=15&m7=15&m8=15"
                                    "&s2=40&s3=40&s4=30&s5=30&s6=20&s7=20&s8=20", &o));
    TT_ASSERT_EQ(LDS_E_RANGE, parse("mg=8&sg=7&dur=30&m0=50&m1=50&m2=40&m3=30&m4=20&m5=15&m6=15&m7=15&m8=15"
                                    "&s2=40&s3=40&s4=30&s5=30&s6=20&s7=20&s8=101", &o));
    /* a body over the limit is rejected before parsing */
    {
        char big[LDS_BODY_MAX + 2];
        memset(big, '&', sizeof big);
        TT_ASSERT_EQ(LDS_E_TOO_LONG, ld_settings_parse_form(big, LDS_BODY_MAX + 1, &keep, &o));
        TT_ASSERT_EQ(LDS_E_MISSING, ld_settings_parse_form(big, LDS_BODY_MAX, &keep, &o));
    }
    TT_ASSERT_EQ(LDS_E_ARG, ld_settings_parse_form(NULL, 0, &keep, &o));
    TT_ASSERT_EQ(LDS_E_ARG, ld_settings_parse_form("", 0, NULL, &o));
    TT_ASSERT_EQ(LDS_E_ARG, ld_settings_parse_form("", 0, &keep, NULL));
    /* errors never modify the output */
    TT_ASSERT(memcmp(&o, &o_before, sizeof o) == 0);
}

static void test_diff(void)
{
    ld_settings_t a = defaults(), b = a;
    ld_diff_t d;
    a.still_sens[0] = 11; a.still_sens[1] = 12; b = a;
    TT_ASSERT_EQ(0, ld_settings_diff(&a, &b, &d));
    TT_ASSERT_EQ(0, d.need_gates);
    TT_ASSERT_EQ(0, d.n_sens);
    b.duration_s = 6;
    ld_settings_diff(&a, &b, &d);
    TT_ASSERT_EQ(1, d.need_gates); TT_ASSERT_EQ(0, d.n_sens);
    b = a; b.max_move_gate = 7;
    ld_settings_diff(&a, &b, &d);
    TT_ASSERT_EQ(1, d.need_gates); TT_ASSERT_EQ(0, d.n_sens);
    b = a; b.max_still_gate = 3;
    ld_settings_diff(&a, &b, &d);
    TT_ASSERT_EQ(1, d.need_gates);
    /* gate 0: only move changes, still sent is the old still[0] */
    b = a; b.move_sens[0] = 99;
    ld_settings_diff(&a, &b, &d);
    TT_ASSERT_EQ(0, d.need_gates); TT_ASSERT_EQ(1, d.n_sens);
    TT_ASSERT_EQ(0, d.sens[0].gate); TT_ASSERT_EQ(99, d.sens[0].move); TT_ASSERT_EQ(11, d.sens[0].still);
    /* still[0..1] differences alone are ignored */
    b = a; b.still_sens[0] = 1; b.still_sens[1] = 2;
    ld_settings_diff(&a, &b, &d);
    TT_ASSERT_EQ(0, d.n_sens);
    /* gate 1 move change with a (ignored) still change: old still still sent */
    b = a; b.move_sens[1] = 3; b.still_sens[1] = 100;
    ld_settings_diff(&a, &b, &d);
    TT_ASSERT_EQ(1, d.n_sens); TT_ASSERT_EQ(1, d.sens[0].gate); TT_ASSERT_EQ(12, d.sens[0].still);
    /* gate 4: only still changes, move sent unchanged */
    b = a; b.still_sens[4] = 1;
    ld_settings_diff(&a, &b, &d);
    TT_ASSERT_EQ(1, d.n_sens); TT_ASSERT_EQ(4, d.sens[0].gate);
    TT_ASSERT_EQ(20, d.sens[0].move); TT_ASSERT_EQ(1, d.sens[0].still);
    /* everything changes: nine sens commands in gate order */
    for (int g = 0; g < 9; g++) { b.move_sens[g] = (uint8_t)(a.move_sens[g] + 1); b.still_sens[g] = (uint8_t)(a.still_sens[g] + 1); }
    b.duration_s = 0; b.max_move_gate = 2;
    ld_settings_diff(&a, &b, &d);
    TT_ASSERT_EQ(1, d.need_gates); TT_ASSERT_EQ(9, d.n_sens);
    for (unsigned i = 0; i < 9; i++) TT_ASSERT_EQ(i, d.sens[i].gate);
    TT_ASSERT_EQ(a.still_sens[0], d.sens[0].still);
    TT_ASSERT_EQ(b.still_sens[8], d.sens[8].still);
    TT_ASSERT_EQ(-EINVAL, ld_settings_diff(NULL, &b, &d));
    TT_ASSERT_EQ(-EINVAL, ld_settings_diff(&a, NULL, &d));
    TT_ASSERT_EQ(-EINVAL, ld_settings_diff(&a, &b, NULL));
}

int main(void)
{
    TT_RUN(test_encoders_golden);
    TT_RUN(test_encoder_ranges);
    TT_RUN(test_encoder_caps);
    TT_RUN(test_params_decode);
    TT_RUN(test_version);
    TT_RUN(test_version_format);
    TT_RUN(test_settings_basic);
    TT_RUN(test_form_ok);
    TT_RUN(test_form_errors);
    TT_RUN(test_diff);
    return TT_RESULT();
}
