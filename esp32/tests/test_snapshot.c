/* Host tests for snapshot_json: exact output, truncation behaviour. */
#include <errno.h>
#include <stdio.h>
#include <string.h>
#include "tinytest.h"
#include "snapshot_json.h"

static snapshot_t eng_snap(void)
{
    snapshot_t s; memset(&s, 0, sizeof s);
    s.seq = 4000000000u; s.frame_no = 123456u; s.esp_time_us = 0x100000005ull; s.link = SNAP_LINK_OK; s.have_frame = 1;
    s.data.data_type = 1; s.data.engineering = 1; s.data.target_state = 3;
    s.data.moving_dist_cm = 80; s.data.moving_energy = 64;
    s.data.still_dist_cm = 120; s.data.still_energy = 50; s.data.detect_dist_cm = 85;
    s.data.max_moving_gate = 8; s.data.max_still_gate = 7;
    for (unsigned i = 0; i < LD_GATES; i++) { s.data.moving_gate_energy[i] = (uint8_t)(100 - i * 10); s.data.still_gate_energy[i] = (uint8_t)(i * 3); }
    return s;
}

static void test_engineering_exact(void)
{
    snapshot_t s = eng_snap();
    char out[SNAPSHOT_JSON_MAX];
    int n = snapshot_json(out, sizeof out, &s);
    const char *exp =
        "{\"seq\":4000000000,\"frame_no\":123456,\"esp_time_us\":4294967301,\"link\":\"ok\",\"data\":{"
        "\"engineering\":1,\"target_state\":3,\"moving_dist_cm\":80,\"moving_energy\":64,"
        "\"still_dist_cm\":120,\"still_energy\":50,\"detect_dist_cm\":85,"
        "\"max_moving_gate\":8,\"max_still_gate\":7,"
        "\"moving_gate_energy\":[100,90,80,70,60,50,40,30,20],"
        "\"still_gate_energy\":[0,3,6,9,12,15,18,21,24]},"
        "\"gps\":{\"status\":\"absent\",\"fix\":false,\"fix_quality\":null,\"sats\":null,\"hdop\":null,\"lat\":null,\"lon\":null,\"alt_m\":null,\"speed_kmh\":null,\"course_deg\":null,\"utc\":null,\"time_valid\":false,\"pos_valid\":false,\"utc_state\":null},\"imu\":{\"status\":\"absent\",\"pitch_deg\":null,\"roll_deg\":null,\"valid\":false},\"motion\":{\"en\":0,\"active\":0,\"n\":0,\"dist\":null},\"time_source\":\"none\"}";
    TT_ASSERT_EQ(strlen(exp), n);
    TT_ASSERT(strcmp(out, exp) == 0);
}

static void test_normal_and_no_data(void)
{
    snapshot_t s = eng_snap(); char out[SNAPSHOT_JSON_MAX];
    s.data.engineering = 0; s.data.data_type = 2; s.link = SNAP_LINK_LOST; s.seq = 5; s.frame_no = 4294967295u; s.esp_time_us = 9;
    int n = snapshot_json(out, sizeof out, &s);
    const char *exp =
        "{\"seq\":5,\"frame_no\":4294967295,\"esp_time_us\":9,\"link\":\"lost\",\"data\":{"
        "\"engineering\":0,\"target_state\":3,\"moving_dist_cm\":80,\"moving_energy\":64,"
        "\"still_dist_cm\":120,\"still_energy\":50,\"detect_dist_cm\":85},"
        "\"gps\":{\"status\":\"absent\",\"fix\":false,\"fix_quality\":null,\"sats\":null,\"hdop\":null,\"lat\":null,\"lon\":null,\"alt_m\":null,\"speed_kmh\":null,\"course_deg\":null,\"utc\":null,\"time_valid\":false,\"pos_valid\":false,\"utc_state\":null},\"imu\":{\"status\":\"absent\",\"pitch_deg\":null,\"roll_deg\":null,\"valid\":false},\"motion\":{\"en\":0,\"active\":0,\"n\":0,\"dist\":null},\"time_source\":\"none\"}";
    TT_ASSERT_EQ(strlen(exp), n);
    TT_ASSERT(strcmp(out, exp) == 0);
    memset(&s, 0, sizeof s);
    n = snapshot_json(out, sizeof out, &s);
    exp = "{\"seq\":0,\"frame_no\":0,\"esp_time_us\":0,\"link\":\"no_data\",\"data\":null,"
        "\"gps\":{\"status\":\"absent\",\"fix\":false,\"fix_quality\":null,\"sats\":null,\"hdop\":null,\"lat\":null,\"lon\":null,\"alt_m\":null,\"speed_kmh\":null,\"course_deg\":null,\"utc\":null,\"time_valid\":false,\"pos_valid\":false,\"utc_state\":null},\"imu\":{\"status\":\"absent\",\"pitch_deg\":null,\"roll_deg\":null,\"valid\":false},\"motion\":{\"en\":0,\"active\":0,\"n\":0,\"dist\":null},\"time_source\":\"none\"}";
    TT_ASSERT_EQ(strlen(exp), n);
    TT_ASSERT(strcmp(out, exp) == 0);
    s.link = (snap_link_t)77; /* unknown value must not produce garbage */
    TT_ASSERT_EQ(-EINVAL, snapshot_json(out, sizeof out, &s));
}

static void test_never_truncates(void)
{
    snapshot_t s = eng_snap();
    char ref[SNAPSHOT_JSON_MAX];
    int need = snapshot_json(ref, sizeof ref, &s);
    TT_ASSERT(need > 0);
    char buf[SNAPSHOT_JSON_MAX + 64];
    for (size_t cap = 1; cap <= (size_t)need + 1; cap++) {
        memset(buf, 0x7E, sizeof buf);
        int r = snapshot_json(buf, cap, &s);
        if (cap < (size_t)need + 1) {
            TT_ASSERT_EQ(-ENOSPC, r);
            TT_ASSERT_EQ(0, buf[0]);
        } else {
            TT_ASSERT_EQ(need, r);
            TT_ASSERT(strcmp(buf, ref) == 0);
        }
        for (size_t i = cap; i < sizeof buf; i++) if (buf[i] != 0x7E) { TT_ASSERT(0); break; }
    }
    TT_ASSERT_EQ(-ENOSPC, snapshot_json(buf, 0, &s));
    TT_ASSERT_EQ(-EINVAL, snapshot_json(NULL, 10, &s));
    TT_ASSERT_EQ(-EINVAL, snapshot_json(buf, sizeof buf, NULL));
    TT_ASSERT(snapshot_json(buf, SNAPSHOT_JSON_MAX, &s) > 0);
}

static snapshot_t fix_snap(void)
{
    snapshot_t s = eng_snap();
    s.gps_status = SNAP_GPS_OK; s.have_gps = 1;
    s.gps.flags = GPS_FLAG_TIME_VALID | GPS_FLAG_DATE_VALID | GPS_FLAG_POS_VALID | GPS_FLAG_ALT_VALID;
    s.gps.utc_unix_ms = 1759406706123LL; /* 2025-10-02T12:05:06.123Z */
    s.gps.lat_e7 = 478500000; s.gps.lon_e7 = 87500000;
    s.gps.alt_cm = 41230; s.gps.speed_cmps = 278; s.gps.course_cdeg = 18050;
    s.gps.hdop_x100 = 120; s.gps.sats = 9; s.gps.fix_quality = 1;
    s.imu_status = SNAP_IMU_OK; s.have_imu = 1;
    s.imu.status = IMU_STATUS_VALID; s.imu.pitch_cdeg = 1234; s.imu.roll_cdeg = -5;
    s.time_source = TSRC_GPS;
    return s;
}

static const char *tail_of(const char *out)
{
    const char *p = strstr(out, "\"gps\":");
    return p ? p : "";
}

static void test_valid_fix(void)
{
    snapshot_t s = fix_snap(); s.have_frame = 0;
    char out[SNAPSHOT_JSON_MAX];
    int n = snapshot_json(out, sizeof out, &s);
    const char *exp =
        "\"gps\":{\"status\":\"ok\",\"fix\":true,\"fix_quality\":1,\"sats\":9,\"hdop\":1.20,"
        "\"lat\":47.8500000,\"lon\":8.7500000,\"alt_m\":412.30,\"speed_kmh\":10.01,"
        "\"course_deg\":180.50,\"utc\":\"2025-10-02T12:05:06.123Z\",\"time_valid\":true,"
        "\"pos_valid\":true,\"utc_state\":null},\"imu\":{\"status\":\"ok\",\"pitch_deg\":12.34,\"roll_deg\":-0.05,"
        "\"valid\":true},\"motion\":{\"en\":0,\"active\":0,\"n\":0,\"dist\":null},\"time_source\":\"gps\"}";
    TT_ASSERT(n > 0);
    TT_ASSERT(strcmp(tail_of(out), exp) == 0);
}

static void test_negative_coordinates(void)
{
    snapshot_t s = fix_snap();
    char out[SNAPSHOT_JSON_MAX];
    s.gps.lat_e7 = -338688197; s.gps.lon_e7 = -1234567; /* S, W; lon between -1 and 0 */
    s.gps.alt_cm = -1205; s.imu.pitch_cdeg = -9000; s.imu.roll_cdeg = -18000;
    TT_ASSERT(snapshot_json(out, sizeof out, &s) > 0);
    TT_ASSERT(strstr(out, "\"lat\":-33.8688197,\"lon\":-0.1234567,\"alt_m\":-12.05,"));
    TT_ASSERT(strstr(out, "\"pitch_deg\":-90.00,\"roll_deg\":-180.00,"));
    s.gps.lat_e7 = -1; s.gps.lon_e7 = 1;
    TT_ASSERT(snapshot_json(out, sizeof out, &s) > 0);
    TT_ASSERT(strstr(out, "\"lat\":-0.0000001,\"lon\":0.0000001,"));
    s.gps.lat_e7 = INT32_MIN; s.gps.alt_cm = INT32_MIN;
    TT_ASSERT(snapshot_json(out, sizeof out, &s) > 0);
    TT_ASSERT(strstr(out, "\"lat\":-214.7483648,"));
    TT_ASSERT(strstr(out, "\"alt_m\":-21474836.48,"));
}

static void test_no_fix_and_partial_flags(void)
{
    snapshot_t s = fix_snap();
    char out[SNAPSHOT_JSON_MAX];
    /* module alive, time known but no position (typical cold start) */
    s.gps.flags = GPS_FLAG_TIME_VALID | GPS_FLAG_DATE_VALID;
    s.gps.fix_quality = 0; s.gps.sats = 0; s.gps.hdop_x100 = 0;
    TT_ASSERT(snapshot_json(out, sizeof out, &s) > 0);
    TT_ASSERT(strstr(out, "\"gps\":{\"status\":\"ok\",\"fix\":false,\"fix_quality\":0,\"sats\":0,\"hdop\":null,"
                          "\"lat\":null,\"lon\":null,\"alt_m\":null,\"speed_kmh\":null,\"course_deg\":null,"
                          "\"utc\":\"2025-10-02T12:05:06.123Z\",\"time_valid\":true,\"pos_valid\":false,\"utc_state\":null}"));
    /* time without date: utc stays null */
    s.gps.flags = GPS_FLAG_TIME_VALID;
    TT_ASSERT(snapshot_json(out, sizeof out, &s) > 0);
    TT_ASSERT(strstr(out, "\"utc\":null,\"time_valid\":false,"));
    /* position valid (RMC A only, no GGA yet, quality 0): shown position means fix */
    s.gps.flags = GPS_FLAG_POS_VALID;
    TT_ASSERT(snapshot_json(out, sizeof out, &s) > 0);
    TT_ASSERT(strstr(out, "\"fix\":true,\"fix_quality\":0,"));
    TT_ASSERT(strstr(out, "\"lat\":47.8500000,"));
    /* out-of-range utc is not formatted */
    s.gps.flags = GPS_FLAG_TIME_VALID | GPS_FLAG_DATE_VALID;
    s.gps.utc_unix_ms = -1;
    TT_ASSERT(snapshot_json(out, sizeof out, &s) > 0);
    TT_ASSERT(strstr(out, "\"utc\":null,"));
    s.gps.utc_unix_ms = INT64_MAX;
    TT_ASSERT(snapshot_json(out, sizeof out, &s) > 0);
    TT_ASSERT(strstr(out, "\"utc\":null,"));
    s.gps.utc_unix_ms = 0;
    TT_ASSERT(snapshot_json(out, sizeof out, &s) > 0);
    TT_ASSERT(strstr(out, "\"utc\":\"1970-01-01T00:00:00.000Z\","));
    s.gps.utc_unix_ms = 951782400000LL + 86399999; /* 2000-02-29T23:59:59.999Z */
    TT_ASSERT(snapshot_json(out, sizeof out, &s) > 0);
    TT_ASSERT(strstr(out, "\"utc\":\"2000-02-29T23:59:59.999Z\","));
    s.gps.utc_unix_ms = 253402300799999LL;
    TT_ASSERT(snapshot_json(out, sizeof out, &s) > 0);
    TT_ASSERT(strstr(out, "\"utc\":\"9999-12-31T23:59:59.999Z\","));
}

static void test_silent_error_and_sources(void)
{
    snapshot_t s = fix_snap();
    char out[SNAPSHOT_JSON_MAX];
    s.gps_status = SNAP_GPS_SILENT; s.imu_status = SNAP_IMU_ERROR; s.time_source = TSRC_SNTP;
    TT_ASSERT(snapshot_json(out, sizeof out, &s) > 0);
    TT_ASSERT(strstr(out, "\"gps\":{\"status\":\"silent\",\"fix\":false,\"fix_quality\":null,\"sats\":null,"
                          "\"hdop\":null,\"lat\":null,"));
    TT_ASSERT(strstr(out, "\"time_valid\":false,\"pos_valid\":false,\"utc_state\":null}"));
    TT_ASSERT(strstr(out, "\"imu\":{\"status\":\"error\",\"pitch_deg\":null,\"roll_deg\":null,\"valid\":false}"));
    TT_ASSERT(strstr(out, "\"time_source\":\"sntp\"}"));
    /* IMU ok but filter not valid yet, or no record yet */
    s = fix_snap();
    s.imu.status = 0;
    TT_ASSERT(snapshot_json(out, sizeof out, &s) > 0);
    TT_ASSERT(strstr(out, "\"imu\":{\"status\":\"ok\",\"pitch_deg\":null,\"roll_deg\":null,\"valid\":false}"));
    s = fix_snap(); s.have_gps = 0; s.have_imu = 0;
    TT_ASSERT(snapshot_json(out, sizeof out, &s) > 0);
    TT_ASSERT(strstr(out, "\"gps\":{\"status\":\"ok\",\"fix\":false,\"fix_quality\":null"));
    TT_ASSERT(strstr(out, "\"valid\":false}"));
    s = fix_snap(); s.time_source = TSRC_NONE;
    TT_ASSERT(snapshot_json(out, sizeof out, &s) > 0);
    TT_ASSERT(strstr(out, "\"time_source\":\"none\"}"));
    s.time_source = (time_source_t)9;
    TT_ASSERT_EQ(-EINVAL, snapshot_json(out, sizeof out, &s));
    s = fix_snap(); s.gps_status = (snap_gps_t)5;
    TT_ASSERT_EQ(-EINVAL, snapshot_json(out, sizeof out, &s));
    s = fix_snap(); s.imu_status = (snap_imu_t)5;
    TT_ASSERT_EQ(-EINVAL, snapshot_json(out, sizeof out, &s));
}

static void test_motion(void)
{
    snapshot_t s = fix_snap();
    char out[SNAPSHOT_JSON_MAX];
    s.motion_en = 1; s.motion_active = 1; s.motion_n = 42; s.have_motion_dist = 1; s.motion_dist_cm = 312;
    TT_ASSERT(snapshot_json(out, sizeof out, &s) > 0);
    TT_ASSERT(strstr(out, "\"valid\":true},\"motion\":{\"en\":1,\"active\":1,\"n\":42,\"dist\":312},\"time_source\":\"gps\"}"));
    s.have_motion_dist = 0; s.motion_dist_cm = 99; s.motion_active = 0;
    TT_ASSERT(snapshot_json(out, sizeof out, &s) > 0);
    TT_ASSERT(strstr(out, "\"motion\":{\"en\":1,\"active\":0,\"n\":42,\"dist\":null}"));
    s.motion_en = 7; s.motion_active = 9; /* any nonzero flag prints as 1 */
    TT_ASSERT(snapshot_json(out, sizeof out, &s) > 0);
    TT_ASSERT(strstr(out, "\"motion\":{\"en\":1,\"active\":1,"));
}

static void test_utc_state(void)
{
    static const struct { snap_utc_t st; const char *txt; } c[] = {
        {SNAP_UTC_UNKNOWN, ",\"utc_state\":null}"},
        {SNAP_UTC_NO_UBX, ",\"utc_state\":\"no_ubx\"}"},
        {SNAP_UTC_NOT_VALID, ",\"utc_state\":\"not_valid\"}"},
        {SNAP_UTC_VALID, ",\"utc_state\":\"valid\"}"},
        {(snap_utc_t)99, ",\"utc_state\":null}"}, /* out of range -> unknown */
    };
    for (unsigned i = 0; i < sizeof c / sizeof *c; i++) {
        snapshot_t s = fix_snap();
        char out[SNAPSHOT_JSON_MAX];
        s.gps_utc_state = c[i].st;
        TT_ASSERT(snapshot_json(out, sizeof out, &s) > 0);
        /* last key of the gps object, right before the imu object */
        char exp[96];
        snprintf(exp, sizeof exp, "\"pos_valid\":true%s,\"imu\"", c[i].txt);
        TT_ASSERT(strstr(out, exp) != NULL);
        /* independent of the gps status: still reported when the module is silent */
        s.gps_status = SNAP_GPS_SILENT;
        TT_ASSERT(snapshot_json(out, sizeof out, &s) > 0);
        snprintf(exp, sizeof exp, "\"pos_valid\":false%s,\"imu\"", c[i].txt);
        TT_ASSERT(strstr(out, exp) != NULL);
    }
}

static void test_fix_never_truncates_and_worst_case(void)
{
    snapshot_t s = fix_snap();
    char ref[SNAPSHOT_JSON_MAX], buf[SNAPSHOT_JSON_MAX + 64];
    int need = snapshot_json(ref, sizeof ref, &s);
    TT_ASSERT(need > 0);
    for (size_t cap = 1; cap <= (size_t)need; cap++) {
        memset(buf, 0x7E, sizeof buf);
        TT_ASSERT_EQ(-ENOSPC, snapshot_json(buf, cap, &s));
        TT_ASSERT_EQ(0, buf[0]);
    }
    /* worst case: every field at its longest text */
    snapshot_t w = fix_snap();
    w.seq = 0xFFFFFFFFu; w.frame_no = 0xFFFFFFFFu; w.esp_time_us = ~0ull; w.link = SNAP_LINK_NO_DATA;
    memset(w.data.moving_gate_energy, 255, LD_GATES); memset(w.data.still_gate_energy, 255, LD_GATES);
    w.data.moving_dist_cm = w.data.still_dist_cm = w.data.detect_dist_cm = 65535;
    w.data.moving_energy = w.data.still_energy = 255; w.data.max_moving_gate = w.data.max_still_gate = 255;
    w.data.target_state = 255;
    w.gps.lat_e7 = INT32_MIN; w.gps.lon_e7 = INT32_MIN; w.gps.alt_cm = INT32_MIN;
    w.gps.speed_cmps = 65535; w.gps.course_cdeg = 65535; w.gps.hdop_x100 = 65535;
    w.gps.sats = 255; w.gps.fix_quality = 255; w.gps.utc_unix_ms = 253402300799999LL;
    w.imu.pitch_cdeg = INT16_MIN; w.imu.roll_cdeg = INT16_MIN;
    w.time_source = TSRC_SNTP;
    w.motion_en = 1; w.motion_active = 1; w.motion_n = 0xFFFFFFFFu; w.have_motion_dist = 1; w.motion_dist_cm = 65535;
    w.gps_utc_state = SNAP_UTC_NOT_VALID; /* longest utc_state text */
    int n = snapshot_json(buf, sizeof buf, &w);
    TT_ASSERT(n > 0);
    printf("worst-case snapshot JSON: %d bytes + NUL, limit %d\n", n, SNAPSHOT_JSON_MAX);
    TT_ASSERT(n + 1 <= SNAPSHOT_JSON_MAX);
    TT_ASSERT(n + 1 + 64 <= SNAPSHOT_JSON_MAX); /* keep at least 64 bytes of headroom */
    TT_ASSERT_EQ(-ENOSPC, snapshot_json(buf, (size_t)n, &w));
    TT_ASSERT_EQ(n, snapshot_json(buf, (size_t)n + 1, &w));
}

int main(void)
{
    TT_RUN(test_engineering_exact);
    TT_RUN(test_normal_and_no_data);
    TT_RUN(test_never_truncates);
    TT_RUN(test_valid_fix);
    TT_RUN(test_negative_coordinates);
    TT_RUN(test_no_fix_and_partial_flags);
    TT_RUN(test_silent_error_and_sources);
    TT_RUN(test_utc_state);
    TT_RUN(test_motion);
    TT_RUN(test_fix_never_truncates_and_worst_case);
    return TT_RESULT();
}
