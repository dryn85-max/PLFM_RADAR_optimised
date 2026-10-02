/* Host tests for ld2410_frame: decode of normal / engineering payloads, shared vectors. */
#include <errno.h>
#include <string.h>
#include "tinytest.h"
#include "vec_util.h"
#include "ld2410_frame.h"
#include "ld2410_parser.h"

static uint8_t g_raw[LD_MAX_FRAME];
static size_t g_raw_len;
static ld_frame_t g_frame;
static int g_frames;
static void grab(const ld_frame_t *f, void *ctx)
{
    (void)ctx;
    g_frame = *f;
    memcpy(g_raw, f->raw, f->raw_len);
    g_frame.raw = g_raw;
    g_frame.payload = g_raw + 6;
    g_raw_len = f->raw_len;
    g_frames++;
}

/* Run a vector's .bin through the parser; checks the JSON raw_hex matches the file. */
static int load_frame(const char *name)
{
    char bn[64], jn[64];
    uint8_t bin[256]; static uint8_t json[4096];
    snprintf(bn, sizeof bn, "%s.bin", name);
    snprintf(jn, sizeof jn, "%s.json", name);
    size_t n = vec_read(bn, bin, sizeof bin);
    vec_read(jn, json, sizeof json);
    uint8_t hex[256]; const char *pos = NULL;
    size_t hn = json_hex((const char *)json, "raw_hex", &pos, hex, sizeof hex);
    TT_ASSERT(hn == n && memcmp(hex, bin, n) == 0);
    ld_parser_t p; ld_parser_init(&p);
    g_frames = 0;
    ld_parser_feed(&p, bin, n, grab, NULL);
    TT_ASSERT_EQ(1, g_frames);
    return g_frames == 1;
}

static void check_vector_data(const char *name, int engineering)
{
    static uint8_t json[4096];
    char jn[64]; snprintf(jn, sizeof jn, "%s.json", name);
    vec_read(jn, json, sizeof json);
    const char *j = (const char *)json;
    if (!load_frame(name)) return;
    TT_ASSERT_EQ(LD_FRAME_DATA, g_frame.kind);
    ld_data_t d;
    TT_ASSERT_EQ(0, ld_frame_decode(g_frame.payload, g_frame.payload_len, &d));
    TT_ASSERT_EQ(json_u64(j, "data_type"), d.data_type);
    TT_ASSERT_EQ(json_u64(j, "engineering"), d.engineering);
    TT_ASSERT_EQ(engineering, d.engineering);
    TT_ASSERT_EQ(json_u64(j, "target_state"), d.target_state);
    TT_ASSERT_EQ(json_u64(j, "moving_dist_cm"), d.moving_dist_cm);
    TT_ASSERT_EQ(json_u64(j, "moving_energy"), d.moving_energy);
    TT_ASSERT_EQ(json_u64(j, "still_dist_cm"), d.still_dist_cm);
    TT_ASSERT_EQ(json_u64(j, "still_energy"), d.still_energy);
    TT_ASSERT_EQ(json_u64(j, "detect_dist_cm"), d.detect_dist_cm);
    TT_ASSERT_EQ(json_u64(j, "max_moving_gate"), d.max_moving_gate);
    TT_ASSERT_EQ(json_u64(j, "max_still_gate"), d.max_still_gate);
    uint8_t m[LD_GATES], s[LD_GATES];
    json_arr_u8(j, "moving_gate_energy", m, LD_GATES);
    json_arr_u8(j, "still_gate_energy", s, LD_GATES);
    TT_ASSERT(memcmp(m, d.moving_gate_energy, LD_GATES) == 0);
    TT_ASSERT(memcmp(s, d.still_gate_energy, LD_GATES) == 0);
}

static void test_vectors_data(void)
{
    check_vector_data("frame_normal", 0);
    check_vector_data("frame_engineering", 1);
    check_vector_data("frame_engineering_real_0", 1);
    check_vector_data("frame_engineering_real_1", 1);
}

static void test_known_offsets(void)
{
    /* Hand-written payload, independent of the vector generator. */
    uint8_t p[] = {0x01, 0xAA, 0x02, 0x34, 0x12, 0x07, 0x78, 0x56, 0x09, 0xBC, 0x9A,
                   5, 6,
                   10, 11, 12, 13, 14, 15, 16, 17, 18,
                   20, 21, 22, 23, 24, 25, 26, 27, 28,
                   0x55, 0x00};
    ld_data_t d;
    TT_ASSERT_EQ(0, ld_frame_decode(p, sizeof p, &d));
    TT_ASSERT_EQ(1, d.engineering);
    TT_ASSERT_EQ(2, d.target_state);
    TT_ASSERT_EQ(0x1234, d.moving_dist_cm);
    TT_ASSERT_EQ(7, d.moving_energy);
    TT_ASSERT_EQ(0x5678, d.still_dist_cm);
    TT_ASSERT_EQ(9, d.still_energy);
    TT_ASSERT_EQ(0x9ABC, d.detect_dist_cm);
    TT_ASSERT_EQ(5, d.max_moving_gate);
    TT_ASSERT_EQ(6, d.max_still_gate);
    TT_ASSERT_EQ(10, d.moving_gate_energy[0]);
    TT_ASSERT_EQ(18, d.moving_gate_energy[8]);
    TT_ASSERT_EQ(20, d.still_gate_energy[0]);
    TT_ASSERT_EQ(28, d.still_gate_energy[8]);
    /* module-specific extra bytes before the tail are accepted */
    uint8_t q[40]; memcpy(q, p, 31); q[31] = 1; q[32] = 2; q[33] = 3; q[34] = 0x55; q[35] = 0x00;
    TT_ASSERT_EQ(0, ld_frame_decode(q, 36, &d));
    TT_ASSERT_EQ(28, d.still_gate_energy[8]);
}

static void test_truncated_engineering(void)
{
    uint8_t p[] = {0x01, 0xAA, 0x03, 80, 0, 64, 120, 0, 50, 85, 0,
                   8, 8, 1, 2, 3, 4, 5, 6, 7, 8, 9, 1, 2, 3, 4, 5, 6, 7, 8, 9, 0x55, 0x00};
    ld_data_t d;
    TT_ASSERT_EQ(0, ld_frame_decode(p, sizeof p, &d));
    for (size_t n = 0; n < sizeof p; n++) {
        int r = ld_frame_decode(p, n, &d);
        TT_ASSERT(r < 0);
        /* truncated payload that still ends with a plausible tail */
        if (n >= 2) {
            uint8_t t[40]; memcpy(t, p, n); t[n - 2] = 0x55; t[n - 1] = 0x00;
            TT_ASSERT(ld_frame_decode(t, n, &d) < 0);
        }
    }
    TT_ASSERT_EQ(-EMSGSIZE, ld_frame_decode(p, sizeof p - 1, &d));
    TT_ASSERT_EQ(0, d.engineering); /* zeroed on error */
}

static void test_invalid_fields(void)
{
    uint8_t n13[] = {0x02, 0xAA, 0x01, 1, 0, 2, 3, 0, 4, 5, 0, 0x55, 0x00};
    ld_data_t d;
    TT_ASSERT_EQ(0, ld_frame_decode(n13, sizeof n13, &d));
    TT_ASSERT_EQ(0, d.engineering);
    TT_ASSERT_EQ(0, d.max_moving_gate);
    uint8_t t[13];
    memcpy(t, n13, 13); t[0] = 0x03; TT_ASSERT_EQ(-EBADMSG, ld_frame_decode(t, 13, &d));
    memcpy(t, n13, 13); t[0] = 0x00; TT_ASSERT_EQ(-EBADMSG, ld_frame_decode(t, 13, &d));
    memcpy(t, n13, 13); t[1] = 0xAB; TT_ASSERT_EQ(-EBADMSG, ld_frame_decode(t, 13, &d));
    memcpy(t, n13, 13); t[11] = 0x54; TT_ASSERT_EQ(-EBADMSG, ld_frame_decode(t, 13, &d));
    memcpy(t, n13, 13); t[12] = 0x01; TT_ASSERT_EQ(-EBADMSG, ld_frame_decode(t, 13, &d));
    memcpy(t, n13, 13); t[2] = 4; TT_ASSERT_EQ(-EBADMSG, ld_frame_decode(t, 13, &d));
    TT_ASSERT_EQ(-EINVAL, ld_frame_decode(NULL, 13, &d));
    TT_ASSERT_EQ(-EINVAL, ld_frame_decode(n13, 13, NULL));
    TT_ASSERT_EQ(-EMSGSIZE, ld_frame_decode(n13, 12, &d));
    TT_ASSERT_EQ(-EMSGSIZE, ld_frame_decode(n13, 0, &d));
    /* an engineering type byte with a normal-sized payload is truncated */
    memcpy(t, n13, 13); t[0] = 0x01; TT_ASSERT_EQ(-EMSGSIZE, ld_frame_decode(t, 13, &d));
}

int main(void)
{
    TT_RUN(test_vectors_data);
    TT_RUN(test_known_offsets);
    TT_RUN(test_truncated_engineering);
    TT_RUN(test_invalid_fields);
    return TT_RESULT();
}
