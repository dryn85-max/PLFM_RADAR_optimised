/* Host tests for ld2410_cmd: command encoding, ACK decoding, ACK vectors. */
#include <errno.h>
#include <string.h>
#include "tinytest.h"
#include "vec_util.h"
#include "ld2410_cmd.h"
#include "ld2410_parser.h"

static void test_golden_commands(void)
{
    static const uint8_t enable[] = {0xFD, 0xFC, 0xFB, 0xFA, 0x04, 0x00, 0xFF, 0x00, 0x01, 0x00, 0x04, 0x03, 0x02, 0x01};
    static const uint8_t eng[]    = {0xFD, 0xFC, 0xFB, 0xFA, 0x02, 0x00, 0x62, 0x00, 0x04, 0x03, 0x02, 0x01};
    static const uint8_t endc[]   = {0xFD, 0xFC, 0xFB, 0xFA, 0x02, 0x00, 0xFE, 0x00, 0x04, 0x03, 0x02, 0x01};
    uint8_t out[LD_CMD_MAX_FRAME];
    TT_ASSERT_EQ(sizeof enable, ld_cmd_encode_enable_config(out, sizeof out));
    TT_ASSERT(memcmp(out, enable, sizeof enable) == 0);
    TT_ASSERT_EQ(sizeof eng, ld_cmd_encode_enable_engineering(out, sizeof out));
    TT_ASSERT(memcmp(out, eng, sizeof eng) == 0);
    TT_ASSERT_EQ(sizeof endc, ld_cmd_encode_end_config(out, sizeof out));
    TT_ASSERT(memcmp(out, endc, sizeof endc) == 0);
}

static void test_encode_bounds(void)
{
    uint8_t out[32], guard[32];
    const uint8_t v[] = {1, 2, 3};
    TT_ASSERT_EQ(15, ld_cmd_encode(0x1234, v, 3, out, sizeof out));
    TT_ASSERT_EQ(5, out[4]);
    TT_ASSERT_EQ(0x34, out[6]);
    TT_ASSERT_EQ(0x12, out[7]);
    /* every too-small capacity fails and writes nothing past cap */
    for (size_t cap = 0; cap < 15; cap++) {
        memset(guard, 0xEE, sizeof guard);
        TT_ASSERT_EQ(-ENOSPC, ld_cmd_encode(0x1234, v, 3, guard, cap));
        for (size_t i = cap; i < sizeof guard; i++) TT_ASSERT(guard[i] == 0xEE);
    }
    TT_ASSERT_EQ(-EINVAL, ld_cmd_encode(1, NULL, 2, out, sizeof out));
    TT_ASSERT_EQ(-EINVAL, ld_cmd_encode(1, v, LD_CMD_MAX_VALUE + 1, out, sizeof out));
    TT_ASSERT_EQ(-EINVAL, ld_cmd_encode(1, v, 0, NULL, 10));
    TT_ASSERT_EQ(12, ld_cmd_encode(1, NULL, 0, out, sizeof out)); /* no value */
    TT_ASSERT_EQ(-ENOSPC, ld_cmd_encode_enable_config(out, 13));
}

static void test_ack_decode(void)
{
    ld_ack_t a;
    const uint8_t ok[] = {0xFF, 0x01, 0x00, 0x00};
    TT_ASSERT_EQ(0, ld_cmd_decode_ack(ok, sizeof ok, &a));
    TT_ASSERT_EQ(0x00FF, a.cmd);
    TT_ASSERT_EQ(0, a.status);
    const uint8_t extra[] = {0xFF, 0x01, 0x00, 0x00, 0x01, 0x00, 0x40, 0x00};
    TT_ASSERT_EQ(0, ld_cmd_decode_ack(extra, sizeof extra, &a));
    TT_ASSERT_EQ(0, a.status);
    const uint8_t fail[] = {0x62, 0x01, 0x01, 0x00};
    TT_ASSERT_EQ(0, ld_cmd_decode_ack(fail, sizeof fail, &a));
    TT_ASSERT_EQ(0x0062, a.cmd);
    TT_ASSERT_EQ(1, a.status);
    const uint8_t noflag[] = {0x62, 0x00, 0x00, 0x00};
    TT_ASSERT_EQ(-EBADMSG, ld_cmd_decode_ack(noflag, sizeof noflag, &a));
    for (size_t n = 0; n < 4; n++) TT_ASSERT_EQ(-EMSGSIZE, ld_cmd_decode_ack(ok, n, &a));
    TT_ASSERT_EQ(-EINVAL, ld_cmd_decode_ack(NULL, 4, &a));
    TT_ASSERT_EQ(-EINVAL, ld_cmd_decode_ack(ok, 4, NULL));
}

static ld_frame_t g_f; static uint8_t g_raw[LD_MAX_FRAME];
static void grab(const ld_frame_t *f, void *ctx)
{
    (void)ctx;
    g_f = *f; memcpy(g_raw, f->raw, f->raw_len);
    g_f.payload = g_raw + 6; g_f.raw = g_raw;
}

static void check_ack_vector(const char *base, int ok_expected)
{
    char bn[64], jn[64];
    uint8_t bin[128]; static uint8_t json[2048];
    snprintf(bn, sizeof bn, "%s.bin", base);
    snprintf(jn, sizeof jn, "%s.json", base);
    size_t n = vec_read(bn, bin, sizeof bin);
    vec_read(jn, json, sizeof json);
    ld_parser_t p; ld_parser_init(&p);
    g_f.raw_len = 0;
    ld_parser_feed(&p, bin, n, grab, NULL);
    TT_ASSERT_EQ(LD_FRAME_CMD, g_f.kind);
    ld_ack_t a;
    TT_ASSERT_EQ(0, ld_cmd_decode_ack(g_f.payload, g_f.payload_len, &a));
    const char *j = (const char *)json;
    TT_ASSERT_EQ(json_u64(j, "cmd"), a.cmd);
    TT_ASSERT_EQ(json_u64(j, "status"), a.status);
    TT_ASSERT_EQ(json_u64(j, "ok"), a.status == 0);
    TT_ASSERT_EQ(ok_expected, a.status == 0);
}

static void test_ack_vectors(void)
{
    check_ack_vector("ack_ok", 1);
    check_ack_vector("ack_fail", 0);
}

int main(void)
{
    TT_RUN(test_golden_commands);
    TT_RUN(test_encode_bounds);
    TT_RUN(test_ack_decode);
    TT_RUN(test_ack_vectors);
    return TT_RESULT();
}
