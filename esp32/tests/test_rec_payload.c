/* Host tests for rec_payload: gps_fix / imu / time_sync codecs, record checks,
 * every payload vector shared with host/test_ld2410_rec.py. */
#include <errno.h>
#include <string.h>
#include "tinytest.h"
#include "vec_util.h"
#include "rec_payload.h"
#include "ringbuf.h"

static long long jint(const char *j, const char *key)
{
    const char *pos = NULL;
    const char *v = json_find(j, key, &pos);
    return v ? strtoll(v, NULL, 10) : -999999999999LL;
}

static void jarr(const char *j, const char *key, int16_t *out, int n)
{
    const char *pos = NULL;
    const char *v = json_find(j, key, &pos);
    for (int i = 0; v && i < n; i++) {
        while (*v == '[' || *v == ',' || *v == ' ' || *v == '\n') v++;
        out[i] = (int16_t)strtol(v, (char **)&v, 10);
    }
}

static const char *const GPS_VECS[] = {"gps_fix_nominal", "gps_fix_southwest", "gps_fix_nofix", "gps_fix_extremes",
                                     "gps_fix_utc_verified"};
static const char *const IMU_VECS[] = {"imu_nominal", "imu_extremes"};
static const char *const SYNC_VECS[] = {"time_sync_gps", "time_sync_sntp", "time_sync_edge"};

static void test_gps_vectors(void)
{
    for (unsigned k = 0; k < sizeof GPS_VECS / sizeof *GPS_VECS; k++) {
        static uint8_t bin[64], json[2048];
        char n[64];
        snprintf(n, sizeof n, "%s.bin", GPS_VECS[k]);
        size_t len = vec_read(n, bin, sizeof bin);
        snprintf(n, sizeof n, "%s.json", GPS_VECS[k]);
        vec_read(n, json, sizeof json);
        const char *j = (const char *)json;
        TT_ASSERT_EQ(1, jint(j, "record_type"));
        TT_ASSERT_EQ(REC_GPS_FIX_LEN, len);
        rec_gps_fix_t g;
        TT_ASSERT_EQ(0, rec_gps_fix_decode(bin, len, &g));
        TT_ASSERT(g.utc_unix_ms == (int64_t)strtoll(strstr(j, "\"utc_unix_ms\"") + 14, NULL, 10));
        TT_ASSERT_EQ(jint(j, "lat_e7"), g.lat_e7);
        TT_ASSERT_EQ(jint(j, "lon_e7"), g.lon_e7);
        TT_ASSERT_EQ(jint(j, "alt_cm"), g.alt_cm);
        TT_ASSERT_EQ(jint(j, "speed_cmps"), g.speed_cmps);
        TT_ASSERT_EQ(jint(j, "course_cdeg"), g.course_cdeg);
        TT_ASSERT_EQ(jint(j, "hdop_x100"), g.hdop_x100);
        TT_ASSERT_EQ(jint(j, "sats"), g.sats);
        TT_ASSERT_EQ(jint(j, "fix_quality"), g.fix_quality);
        TT_ASSERT_EQ(jint(j, "flags"), g.flags);
        uint8_t out[REC_GPS_FIX_LEN];
        TT_ASSERT_EQ(0, rec_gps_fix_encode(out, &g));
        TT_ASSERT(memcmp(out, bin, len) == 0);
        TT_ASSERT_EQ(0, rec_record_check(REC_TYPE_GPS_FIX, len));
    }
}

static void test_imu_vectors(void)
{
    for (unsigned k = 0; k < sizeof IMU_VECS / sizeof *IMU_VECS; k++) {
        static uint8_t bin[64], json[2048];
        char n[64];
        snprintf(n, sizeof n, "%s.bin", IMU_VECS[k]);
        size_t len = vec_read(n, bin, sizeof bin);
        snprintf(n, sizeof n, "%s.json", IMU_VECS[k]);
        vec_read(n, json, sizeof json);
        const char *j = (const char *)json;
        TT_ASSERT_EQ(2, jint(j, "record_type"));
        TT_ASSERT_EQ(REC_IMU_LEN, len);
        rec_imu_t m; int16_t acc[3], gyr[3];
        TT_ASSERT_EQ(0, rec_imu_decode(bin, len, &m));
        jarr(j, "acc_mg", acc, 3); jarr(j, "gyr_ddps", gyr, 3);
        for (int i = 0; i < 3; i++) { TT_ASSERT_EQ(acc[i], m.acc_mg[i]); TT_ASSERT_EQ(gyr[i], m.gyr_ddps[i]); }
        TT_ASSERT_EQ(jint(j, "pitch_cdeg"), m.pitch_cdeg);
        TT_ASSERT_EQ(jint(j, "roll_cdeg"), m.roll_cdeg);
        TT_ASSERT_EQ(jint(j, "n_samples"), m.n_samples);
        TT_ASSERT_EQ(jint(j, "status"), m.status);
        uint8_t out[REC_IMU_LEN];
        TT_ASSERT_EQ(0, rec_imu_encode(out, &m));
        TT_ASSERT(memcmp(out, bin, len) == 0);
    }
}

static void test_sync_vectors(void)
{
    for (unsigned k = 0; k < sizeof SYNC_VECS / sizeof *SYNC_VECS; k++) {
        static uint8_t bin[64], json[2048];
        char n[64];
        snprintf(n, sizeof n, "%s.bin", SYNC_VECS[k]);
        size_t len = vec_read(n, bin, sizeof bin);
        snprintf(n, sizeof n, "%s.json", SYNC_VECS[k]);
        vec_read(n, json, sizeof json);
        const char *j = (const char *)json;
        TT_ASSERT_EQ(3, jint(j, "record_type"));
        TT_ASSERT_EQ(REC_TIME_SYNC_LEN, len);
        rec_time_sync_t t;
        TT_ASSERT_EQ(0, rec_time_sync_decode(bin, len, &t));
        TT_ASSERT(t.utc_unix_us == (int64_t)strtoll(strstr(j, "\"utc_unix_us\"") + 14, NULL, 10));
        TT_ASSERT_EQ(jint(j, "source"), t.source);
        uint8_t out[REC_TIME_SYNC_LEN];
        TT_ASSERT_EQ(0, rec_time_sync_encode(out, &t));
        TT_ASSERT(memcmp(out, bin, len) == 0);
    }
}

static void test_explicit_layout(void)
{
    /* hand-derived bytes, independent of the vectors */
    rec_gps_fix_t g = {.utc_unix_ms = -2, .lat_e7 = -1, .lon_e7 = 0x01020304, .alt_cm = -256,
                       .speed_cmps = 0x0506, .course_cdeg = 0x0708, .hdop_x100 = 0x090A,
                       .sats = 0x0B, .fix_quality = 0x0C, .flags = 0x0D};
    uint8_t o[REC_GPS_FIX_LEN];
    static const uint8_t exp[32] = {
        0xFE, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0x04, 0x03, 0x02, 0x01,
        0x00, 0xFF, 0xFF, 0xFF, 0x06, 0x05, 0x08, 0x07, 0x0A, 0x09, 0x0B, 0x0C, 0x0D, 0, 0, 0};
    memset(o, 0xEE, sizeof o);
    TT_ASSERT_EQ(0, rec_gps_fix_encode(o, &g));
    TT_ASSERT(memcmp(o, exp, 32) == 0);
    rec_gps_fix_t d;
    TT_ASSERT_EQ(0, rec_gps_fix_decode(exp, 32, &d));
    TT_ASSERT(d.utc_unix_ms == -2); TT_ASSERT_EQ(-1, d.lat_e7); TT_ASSERT_EQ(-256, d.alt_cm);
    TT_ASSERT_EQ(0x0506, d.speed_cmps); TT_ASSERT_EQ(0x0D, d.flags);

    rec_imu_t m = {.acc_mg = {1, -2, 3}, .gyr_ddps = {-4, 5, -6}, .pitch_cdeg = -7, .roll_cdeg = 0x0809,
                   .n_samples = 10, .status = 1};
    uint8_t oi[REC_IMU_LEN];
    static const uint8_t expi[18] = {1, 0, 0xFE, 0xFF, 3, 0, 0xFC, 0xFF, 5, 0, 0xFA, 0xFF,
                                     0xF9, 0xFF, 0x09, 0x08, 10, 1};
    TT_ASSERT_EQ(0, rec_imu_encode(oi, &m));
    TT_ASSERT(memcmp(oi, expi, 18) == 0);

    rec_time_sync_t ts = {.utc_unix_us = -3, .source = 2};
    uint8_t ot[REC_TIME_SYNC_LEN];
    static const uint8_t expt[9] = {0xFD, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 2};
    TT_ASSERT_EQ(0, rec_time_sync_encode(ot, &ts));
    TT_ASSERT(memcmp(ot, expt, 9) == 0);
}

static void test_saturation(void)
{
    rec_gps_fix_t g; memset(&g, 0, sizeof g);
    uint8_t o[REC_GPS_FIX_LEN]; rec_gps_fix_t d;
    static const uint32_t in[] = {0, 1, 65534, 65535, 65536, 70000, 0xFFFFFFFFu};
    static const uint32_t want[] = {0, 1, 65534, 65535, 65535, 65535, 65535};
    for (unsigned i = 0; i < sizeof in / sizeof *in; i++) {
        g.speed_cmps = in[i];
        g.course_cdeg = 0xABCD; g.hdop_x100 = 0x1234; /* neighbours must not be disturbed */
        TT_ASSERT_EQ(0, rec_gps_fix_encode(o, &g));
        TT_ASSERT_EQ(0, rec_gps_fix_decode(o, sizeof o, &d));
        TT_ASSERT_EQ(want[i], d.speed_cmps);
        TT_ASSERT_EQ(0xABCD, d.course_cdeg); TT_ASSERT_EQ(0x1234, d.hdop_x100);
    }
}

/* flags are a plain byte: every value, incl. GPS_FLAG_UTC_VERIFIED, is passed unchanged */
static void test_flags_passthrough(void)
{
    TT_ASSERT_EQ(0x10, GPS_FLAG_UTC_VERIFIED);
    TT_ASSERT_EQ(0, GPS_FLAG_UTC_VERIFIED & (GPS_FLAG_TIME_VALID | GPS_FLAG_DATE_VALID |
                                             GPS_FLAG_POS_VALID | GPS_FLAG_ALT_VALID));
    rec_gps_fix_t g, d;
    uint8_t o[REC_GPS_FIX_LEN];
    memset(&g, 0, sizeof g);
    for (unsigned f = 0; f < 256; f++) {
        g.flags = (uint8_t)f;
        TT_ASSERT_EQ(0, rec_gps_fix_encode(o, &g));
        TT_ASSERT_EQ(f, o[28]);
        TT_ASSERT_EQ(0, rec_gps_fix_decode(o, sizeof o, &d));
        TT_ASSERT_EQ(f, d.flags);
    }
}

static void test_bad_input(void)
{
    uint8_t buf[64]; memset(buf, 0, sizeof buf);
    rec_gps_fix_t g; rec_imu_t m; rec_time_sync_t t;
    for (size_t n = 0; n < sizeof buf; n++) {
        TT_ASSERT_EQ(n == REC_GPS_FIX_LEN ? 0 : -EBADMSG, rec_gps_fix_decode(buf, n, &g));
        TT_ASSERT_EQ(n == REC_IMU_LEN ? 0 : -EBADMSG, rec_imu_decode(buf, n, &m));
        TT_ASSERT_EQ(n == REC_TIME_SYNC_LEN ? 0 : -EBADMSG, rec_time_sync_decode(buf, n, &t));
    }
    for (unsigned i = 29; i < 32; i++) { /* reserved bytes must be zero */
        buf[i] = 1;
        TT_ASSERT_EQ(-EBADMSG, rec_gps_fix_decode(buf, REC_GPS_FIX_LEN, &g));
        buf[i] = 0;
    }
    TT_ASSERT_EQ(-EINVAL, rec_gps_fix_decode(NULL, 32, &g));
    TT_ASSERT_EQ(-EINVAL, rec_gps_fix_decode(buf, 32, NULL));
    TT_ASSERT_EQ(-EINVAL, rec_imu_decode(NULL, 18, &m));
    TT_ASSERT_EQ(-EINVAL, rec_imu_decode(buf, 18, NULL));
    TT_ASSERT_EQ(-EINVAL, rec_time_sync_decode(NULL, 9, &t));
    TT_ASSERT_EQ(-EINVAL, rec_time_sync_decode(buf, 9, NULL));
    TT_ASSERT_EQ(-EINVAL, rec_gps_fix_encode(NULL, &g));
    TT_ASSERT_EQ(-EINVAL, rec_gps_fix_encode(buf, NULL));
    TT_ASSERT_EQ(-EINVAL, rec_imu_encode(NULL, &m));
    TT_ASSERT_EQ(-EINVAL, rec_imu_encode(buf, NULL));
    TT_ASSERT_EQ(-EINVAL, rec_time_sync_encode(NULL, &t));
    TT_ASSERT_EQ(-EINVAL, rec_time_sync_encode(buf, NULL));
    /* a decode failure leaves the output untouched */
    g.sats = 77; memset(buf, 0, sizeof buf);
    TT_ASSERT_EQ(-EBADMSG, rec_gps_fix_decode(buf, 31, &g));
    TT_ASSERT_EQ(77, g.sats);
}

static void test_record_check(void)
{
    TT_ASSERT_EQ(0, rec_record_check(REC_TYPE_GPS_FIX, 32));
    TT_ASSERT_EQ(0, rec_record_check(REC_TYPE_IMU, 18));
    TT_ASSERT_EQ(0, rec_record_check(REC_TYPE_TIME_SYNC, 9));
    for (size_t n = 0; n < 40; n++) {
        TT_ASSERT_EQ(n == 32 ? 0 : -EBADMSG, rec_record_check(REC_TYPE_GPS_FIX, n));
        TT_ASSERT_EQ(n == 18 ? 0 : -EBADMSG, rec_record_check(REC_TYPE_IMU, n));
        TT_ASSERT_EQ(n == 9 ? 0 : -EBADMSG, rec_record_check(REC_TYPE_TIME_SYNC, n));
    }
    TT_ASSERT_EQ(-EBADMSG, rec_record_check(REC_TYPE_LD2410_FRAME, 0));
    TT_ASSERT_EQ(-EBADMSG, rec_record_check(REC_TYPE_LD2410_FRAME, 9));
    TT_ASSERT_EQ(0, rec_record_check(REC_TYPE_LD2410_FRAME, 10));
    TT_ASSERT_EQ(0, rec_record_check(REC_TYPE_LD2410_FRAME, RB_MAX_RAW));
    TT_ASSERT_EQ(-EBADMSG, rec_record_check(REC_TYPE_LD2410_FRAME, RB_MAX_RAW + 1));
    /* unknown types: not an error of the stream, the caller keeps or skips them */
    for (unsigned ty = 4; ty < 256; ty++)
        TT_ASSERT_EQ(-ENOTSUP, rec_record_check((uint8_t)ty, 9));
}

int main(void)
{
    TT_RUN(test_gps_vectors);
    TT_RUN(test_imu_vectors);
    TT_RUN(test_sync_vectors);
    TT_RUN(test_explicit_layout);
    TT_RUN(test_saturation);
    TT_RUN(test_flags_passthrough);
    TT_RUN(test_bad_input);
    TT_RUN(test_record_check);
    return TT_RESULT();
}
