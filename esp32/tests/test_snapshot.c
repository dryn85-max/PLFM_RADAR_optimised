/* Host tests for snapshot_json: exact output, truncation behaviour. */
#include <errno.h>
#include <string.h>
#include "tinytest.h"
#include "snapshot_json.h"

static snapshot_t eng_snap(void)
{
    snapshot_t s; memset(&s, 0, sizeof s);
    s.seq = 4000000000u; s.esp_time_us = 0x100000005ull; s.link = SNAP_LINK_OK; s.have_frame = 1;
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
    char out[512];
    int n = snapshot_json(out, sizeof out, &s);
    const char *exp =
        "{\"seq\":4000000000,\"esp_time_us\":4294967301,\"link\":\"ok\",\"data\":{"
        "\"engineering\":1,\"target_state\":3,\"moving_dist_cm\":80,\"moving_energy\":64,"
        "\"still_dist_cm\":120,\"still_energy\":50,\"detect_dist_cm\":85,"
        "\"max_moving_gate\":8,\"max_still_gate\":7,"
        "\"moving_gate_energy\":[100,90,80,70,60,50,40,30,20],"
        "\"still_gate_energy\":[0,3,6,9,12,15,18,21,24]}}";
    TT_ASSERT_EQ(strlen(exp), n);
    TT_ASSERT(strcmp(out, exp) == 0);
}

static void test_normal_and_no_data(void)
{
    snapshot_t s = eng_snap(); char out[512];
    s.data.engineering = 0; s.data.data_type = 2; s.link = SNAP_LINK_LOST; s.seq = 5; s.esp_time_us = 9;
    int n = snapshot_json(out, sizeof out, &s);
    const char *exp =
        "{\"seq\":5,\"esp_time_us\":9,\"link\":\"lost\",\"data\":{"
        "\"engineering\":0,\"target_state\":3,\"moving_dist_cm\":80,\"moving_energy\":64,"
        "\"still_dist_cm\":120,\"still_energy\":50,\"detect_dist_cm\":85}}";
    TT_ASSERT_EQ(strlen(exp), n);
    TT_ASSERT(strcmp(out, exp) == 0);
    memset(&s, 0, sizeof s);
    n = snapshot_json(out, sizeof out, &s);
    exp = "{\"seq\":0,\"esp_time_us\":0,\"link\":\"no_data\",\"data\":null}";
    TT_ASSERT_EQ(strlen(exp), n);
    TT_ASSERT(strcmp(out, exp) == 0);
    s.link = (snap_link_t)77; /* unknown value must not produce garbage */
    TT_ASSERT_EQ(-EINVAL, snapshot_json(out, sizeof out, &s));
}

static void test_never_truncates(void)
{
    snapshot_t s = eng_snap();
    char ref[512];
    int need = snapshot_json(ref, sizeof ref, &s);
    TT_ASSERT(need > 0);
    char buf[600];
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
    /* worst-case size stays small enough for a 512-byte buffer */
    snapshot_t w = eng_snap();
    w.seq = 0xFFFFFFFFu; w.esp_time_us = ~0ull;
    memset(w.data.moving_gate_energy, 255, LD_GATES); memset(w.data.still_gate_energy, 255, LD_GATES);
    w.data.moving_dist_cm = w.data.still_dist_cm = w.data.detect_dist_cm = 65535;
    w.data.moving_energy = w.data.still_energy = 255; w.data.max_moving_gate = w.data.max_still_gate = 255;
    TT_ASSERT(snapshot_json(buf, 512, &w) > 0);
}

int main(void)
{
    TT_RUN(test_engineering_exact);
    TT_RUN(test_normal_and_no_data);
    TT_RUN(test_never_truncates);
    return TT_RESULT();
}
