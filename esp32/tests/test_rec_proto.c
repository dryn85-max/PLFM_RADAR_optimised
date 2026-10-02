/* Host tests for rec_proto: request, batch builder, ring batches, shared batch vector. */
#include <errno.h>
#include <string.h>
#include "tinytest.h"
#include "vec_util.h"
#include "rec_proto.h"

static uint32_t rd32(const uint8_t *p) { return p[0] | (p[1] << 8) | (p[2] << 16) | ((uint32_t)p[3] << 24); }
static uint16_t rd16(const uint8_t *p) { return (uint16_t)(p[0] | (p[1] << 8)); }

static void test_request(void)
{
    uint8_t r[REC_REQ_LEN]; uint32_t from = 0;
    TT_ASSERT_EQ(0, rec_req_encode(r, 0xDEADBEEFu));
    static const uint8_t exp[12] = {'L', 'D', 'R', 'Q', 2, 0, 0, 0, 0xEF, 0xBE, 0xAD, 0xDE};
    TT_ASSERT(memcmp(r, exp, 12) == 0);
    TT_ASSERT_EQ(0, rec_req_parse(r, 12, &from));
    TT_ASSERT_EQ(0xDEADBEEFu, from);
    TT_ASSERT_EQ(0, rec_req_parse(exp, 12, &from));
    TT_ASSERT_EQ(-EBADMSG, rec_req_parse(r, 11, &from));
    TT_ASSERT_EQ(-EBADMSG, rec_req_parse(r, 13, &from));
    TT_ASSERT_EQ(-EBADMSG, rec_req_parse(r, 0, &from));
    for (size_t i = 0; i < 4; i++) { uint8_t t[12]; memcpy(t, r, 12); t[i] ^= 0x20; TT_ASSERT_EQ(-EBADMSG, rec_req_parse(t, 12, &from)); }
    for (size_t i = 5; i < 8; i++) { uint8_t t[12]; memcpy(t, r, 12); t[i] = 1; TT_ASSERT_EQ(-EBADMSG, rec_req_parse(t, 12, &from)); }
    { uint8_t t[12]; memcpy(t, r, 12); t[4] = 1; TT_ASSERT_EQ(-ENOTSUP, rec_req_parse(t, 12, &from)); }
    { uint8_t t[12]; memcpy(t, r, 12); t[4] = 3; TT_ASSERT_EQ(-ENOTSUP, rec_req_parse(t, 12, &from)); }
    { uint8_t t[12]; memcpy(t, r, 12); t[4] = 0; TT_ASSERT_EQ(-ENOTSUP, rec_req_parse(t, 12, &from)); }
    TT_ASSERT_EQ(-EINVAL, rec_req_parse(NULL, 12, &from));
    TT_ASSERT_EQ(-EINVAL, rec_req_parse(r, 12, NULL));
}

static void test_builder_layout_and_limits(void)
{
    uint8_t buf[200]; rec_batch_t b; rec_batch_hdr_t h;
    const uint8_t raw[5] = {1, 2, 3, 4, 5};
    TT_ASSERT_EQ(0, rec_batch_begin(&b, buf, sizeof buf, 0x01020304u, REC_FLAG_GAP, 0xCAFEF00Du));
    TT_ASSERT_EQ(0, rec_batch_add(&b, 7, 0x1122334455667788ull, raw, 5));
    size_t n = rec_batch_finish(&b);
    TT_ASSERT_EQ(20 + 14 + 5, n);
    TT_ASSERT(memcmp(buf, "LDRB", 4) == 0);
    TT_ASSERT_EQ(2, buf[4]); TT_ASSERT_EQ(1, buf[5]); TT_ASSERT_EQ(0, rd16(buf + 6));
    TT_ASSERT_EQ(0x01020304u, rd32(buf + 8)); TT_ASSERT_EQ(1, rd16(buf + 12)); TT_ASSERT_EQ(0, rd16(buf + 14)); TT_ASSERT_EQ(0xCAFEF00Du, rd32(buf + 16));
    TT_ASSERT_EQ(7, rd32(buf + 20)); TT_ASSERT_EQ(0x55667788u, rd32(buf + 24)); TT_ASSERT_EQ(0x11223344u, rd32(buf + 28));
    TT_ASSERT_EQ(5, rd16(buf + 32)); TT_ASSERT(memcmp(buf + 34, raw, 5) == 0);
    TT_ASSERT_EQ(0, rec_batch_parse_header(buf, n, &h));
    TT_ASSERT_EQ(2, h.version); TT_ASSERT_EQ(1, h.flags); TT_ASSERT_EQ(0x01020304u, h.first_seq); TT_ASSERT_EQ(1, h.count);
    TT_ASSERT_EQ(0xCAFEF00Du, h.boot_id);

    /* keep-alive */
    TT_ASSERT_EQ(0, rec_batch_begin(&b, buf, sizeof buf, 42, 0, 7));
    TT_ASSERT_EQ(20, rec_batch_finish(&b));
    TT_ASSERT_EQ(0, rec_batch_parse_header(buf, 20, &h));
    TT_ASSERT_EQ(7, h.boot_id);
    TT_ASSERT_EQ(0, h.count); TT_ASSERT_EQ(42, h.first_seq); TT_ASSERT_EQ(0, h.flags);

    /* exact fit and one byte short; failed add leaves the batch unchanged */
    uint8_t g[64];
    for (size_t cap = 0; cap < 20; cap++) TT_ASSERT_EQ(-ENOSPC, rec_batch_begin(&b, g, cap, 0, 0, 1));
    TT_ASSERT_EQ(-EINVAL, rec_batch_begin(&b, g, sizeof g, 0, 0, 0)); /* boot_id 0 is invalid */
    memset(g, 0xEE, sizeof g);
    TT_ASSERT_EQ(0, rec_batch_begin(&b, g, 39, 0, 0, 1));
    TT_ASSERT_EQ(0, rec_batch_add(&b, 1, 2, raw, 5));
    TT_ASSERT_EQ(39, b.len);
    TT_ASSERT_EQ(-ENOSPC, rec_batch_add(&b, 2, 3, raw, 0)); /* needs 14 more */
    TT_ASSERT_EQ(39, b.len);
    TT_ASSERT_EQ(1, b.count);
    TT_ASSERT_EQ(39, rec_batch_finish(&b));
    for (size_t i = 39; i < sizeof g; i++) TT_ASSERT(g[i] == 0xEE);
    memset(g, 0xEE, sizeof g);
    TT_ASSERT_EQ(0, rec_batch_begin(&b, g, 38, 0, 0, 1));
    TT_ASSERT_EQ(-ENOSPC, rec_batch_add(&b, 1, 2, raw, 5));
    TT_ASSERT_EQ(0, b.count);
    TT_ASSERT_EQ(20, rec_batch_finish(&b));
    for (size_t i = 38; i < sizeof g; i++) TT_ASSERT(g[i] == 0xEE);
    TT_ASSERT_EQ(-EINVAL, rec_batch_add(&b, 1, 2, NULL, 5));
    TT_ASSERT_EQ(-EINVAL, rec_batch_add(&b, 1, 2, raw, 70000));
    /* header parse errors */
    TT_ASSERT_EQ(-EBADMSG, rec_batch_parse_header(buf, 19, &h));
    uint8_t t[20]; memcpy(t, buf, 20);
    t[0] = 'X'; TT_ASSERT_EQ(-EBADMSG, rec_batch_parse_header(t, 20, &h)); t[0] = 'L';
    t[6] = 1; TT_ASSERT_EQ(-EBADMSG, rec_batch_parse_header(t, 20, &h)); t[6] = 0;
    t[14] = 1; TT_ASSERT_EQ(-EBADMSG, rec_batch_parse_header(t, 20, &h)); t[14] = 0;
    memset(t + 16, 0, 4); TT_ASSERT_EQ(-EBADMSG, rec_batch_parse_header(t, 20, &h)); /* boot_id 0 */
    t[16] = 1;
    t[4] = 9; TT_ASSERT_EQ(-ENOTSUP, rec_batch_parse_header(t, 20, &h));
    t[4] = 1; TT_ASSERT_EQ(-ENOTSUP, rec_batch_parse_header(t, 20, &h)); /* old protocol version */
}

static void test_count_limit(void)
{
    static uint8_t big[20 + 65540 * 14];
    rec_batch_t b;
    TT_ASSERT_EQ(0, rec_batch_begin(&b, big, sizeof big, 0, 0, 1));
    static const uint8_t empty[1] = {0};
    int ok = 0;
    for (int i = 0; i < 65536; i++) {
        int r = rec_batch_add(&b, (uint32_t)i, 0, empty, 0);
        if (r == 0) ok++;
    }
    TT_ASSERT_EQ(65535, ok);
    TT_ASSERT_EQ(65535, b.count);
}

static void test_vector_batch(void)
{
    static uint8_t bin[512], json[4096];
    size_t n = vec_read("batch_gap_wrap.bin", bin, sizeof bin);
    vec_read("batch_gap_wrap.json", json, sizeof json);
    const char *j = (const char *)json;
    rec_batch_hdr_t h;
    TT_ASSERT_EQ(0, rec_batch_parse_header(bin, n, &h));
    TT_ASSERT_EQ(json_u64(j, "version"), h.version);
    TT_ASSERT_EQ(json_u64(j, "flags"), h.flags);
    TT_ASSERT_EQ(json_u64(j, "first_seq"), h.first_seq);
    TT_ASSERT_EQ(json_u64(j, "count"), h.count);
    TT_ASSERT_EQ(json_u64(j, "boot_id"), h.boot_id);
    /* rebuild from the JSON's records and compare to the .bin byte for byte */
    static uint8_t out[512]; rec_batch_t b;
    TT_ASSERT_EQ(0, rec_batch_begin(&b, out, sizeof out, h.first_seq, h.flags, h.boot_id));
    const char *pos = strstr(j, "\"records\"");
    for (unsigned i = 0; i < h.count; i++) {
        uint8_t raw[128];
        unsigned long long seq = json_u64_at(j, "seq", &pos);
        unsigned long long tm = json_u64_at(j, "esp_time_us", &pos);
        size_t rl = json_hex(j, "raw_hex", &pos, raw, sizeof raw);
        TT_ASSERT_EQ(0, rec_batch_add(&b, (uint32_t)seq, tm, raw, rl));
    }
    size_t on = rec_batch_finish(&b);
    TT_ASSERT_EQ(n, on);
    TT_ASSERT(on == n && memcmp(out, bin, n) == 0);
    /* and walk the .bin records */
    size_t off = 20; unsigned cnt = 0;
    while (off + 14 <= n) { off += 14 + rd16(bin + off + 12); cnt++; }
    TT_ASSERT_EQ(off, n);
    TT_ASSERT_EQ(h.count, cnt);
    TT_ASSERT_EQ(0xFFFFFFFFu, rd32(bin + 20));
    TT_ASSERT_EQ(0xA1B2C3D4u, h.boot_id);
}

/* A rebooted device: other boot_id, seq restarted at 0, keep-alive. */
static void test_vector_reboot(void)
{
    static uint8_t bin[64], gap[512], json[1024];
    size_t n = vec_read("batch_reboot.bin", bin, sizeof bin);
    vec_read("batch_reboot.json", json, sizeof json);
    size_t gn = vec_read("batch_gap_wrap.bin", gap, sizeof gap);
    rec_batch_hdr_t h, g;
    TT_ASSERT_EQ(20, n);
    TT_ASSERT_EQ(0, rec_batch_parse_header(bin, n, &h));
    TT_ASSERT_EQ(0, rec_batch_parse_header(gap, gn, &g));
    TT_ASSERT_EQ(0xFFFFFFFFu, h.boot_id);
    TT_ASSERT_EQ(json_u64((const char *)json, "boot_id"), h.boot_id);
    TT_ASSERT(h.boot_id != g.boot_id);
    TT_ASSERT_EQ(0, h.first_seq); TT_ASSERT_EQ(0, h.count); TT_ASSERT_EQ(0, h.flags);
    uint8_t out[20]; rec_batch_t b;
    TT_ASSERT_EQ(0, rec_batch_begin(&b, out, sizeof out, 0, 0, 0xFFFFFFFFu));
    TT_ASSERT_EQ(20, rec_batch_finish(&b));
    TT_ASSERT(memcmp(out, bin, 20) == 0);
}

static void test_batch_helpers(void)
{
    uint8_t buf[96]; rec_batch_t b; uint32_t next = 0;
    const uint8_t raw[3] = {1, 2, 3};
    /* next seq is first_seq + count: after a GAP this is NOT the requested seq */
    TT_ASSERT_EQ(0, rec_batch_begin(&b, buf, sizeof buf, 0xFFFFFFFEu, REC_FLAG_GAP, 5));
    TT_ASSERT_EQ(0, rec_batch_add(&b, 0xFFFFFFFEu, 1, raw, 3));
    TT_ASSERT_EQ(0, rec_batch_add(&b, 0xFFFFFFFFu, 2, raw, 3));
    TT_ASSERT_EQ(0, rec_batch_add(&b, 0, 3, raw, 3));
    size_t n = rec_batch_finish(&b);
    TT_ASSERT_EQ(0, rec_batch_next_seq(buf, n, &next));
    TT_ASSERT_EQ(1, next);
    TT_ASSERT_EQ(-EBADMSG, rec_batch_next_seq(buf, 10, &next));
    TT_ASSERT_EQ(-EINVAL, rec_batch_next_seq(buf, n, NULL));
    /* truncation heuristic: could one more maximum-size record have fitted? */
    TT_ASSERT(!rec_batch_maybe_truncated(20, 16384));
    TT_ASSERT(!rec_batch_maybe_truncated(16384 - (RB_REC_HDR + RB_MAX_RAW), 16384));
    TT_ASSERT(rec_batch_maybe_truncated(16384 - (RB_REC_HDR + RB_MAX_RAW) + 1, 16384));
    TT_ASSERT(rec_batch_maybe_truncated(16384, 16384));
    TT_ASSERT(!rec_batch_maybe_truncated(0, 100)); /* cap smaller than a record */
}

static uint8_t ringmem[2048];

static void test_from_ring(void)
{
    rb_t rb; uint8_t out[400]; size_t on; uint16_t cnt; rec_batch_hdr_t h;
    rb_init(&rb, ringmem, sizeof ringmem, 0xFFFFFFFDu);
    /* empty ring: keep-alive */
    TT_ASSERT_EQ(0, rec_batch_from_ring(&rb, 0xFFFFFFFDu, 0x1234u, out, sizeof out, &on, &cnt));
    TT_ASSERT_EQ(20, on); TT_ASSERT_EQ(0, cnt);
    TT_ASSERT_EQ(0, rec_batch_parse_header(out, on, &h));
    TT_ASSERT_EQ(0xFFFFFFFDu, h.first_seq); TT_ASSERT_EQ(0, h.flags); TT_ASSERT_EQ(0x1234u, h.boot_id);
    uint8_t raw[30];
    for (uint32_t i = 0; i < 6; i++) { memset(raw, (int)i, sizeof raw); rb_push(&rb, i, raw, 30, NULL); }
    /* seq runs FFFFFFFD..FFFFFFFF,0,1,2; each record is 16+30 = 46 + ... = 44 bytes */
    TT_ASSERT_EQ(0, rec_batch_from_ring(&rb, 0xFFFFFFFDu, 0x1234u, out, sizeof out, &on, &cnt));
    TT_ASSERT_EQ(6, cnt); TT_ASSERT_EQ(20 + 6 * 44, on);
    TT_ASSERT_EQ(0, rec_batch_parse_header(out, on, &h));
    TT_ASSERT_EQ(0xFFFFFFFDu, h.first_seq); TT_ASSERT_EQ(0, h.flags); TT_ASSERT_EQ(6, h.count);
    TT_ASSERT_EQ(0xFFFFFFFFu, rd32(out + 20 + 2 * 44));
    TT_ASSERT_EQ(0, rd32(out + 20 + 3 * 44));
    TT_ASSERT_EQ(1, rd32(out + 20 + 4 * 44));
    TT_ASSERT_EQ(3, out[20 + 3 * 44 + 14]);
    /* start mid-wrap */
    TT_ASSERT_EQ(0, rec_batch_from_ring(&rb, 0, 0x1234u, out, sizeof out, &on, &cnt));
    TT_ASSERT_EQ(3, cnt);
    TT_ASSERT_EQ(0, rd32(out + 8));
    /* batch that does not fit: only whole records, guard bytes untouched, resume works */
    uint8_t g[300]; uint32_t next = 0xFFFFFFFDu; int total = 0, rounds = 0;
    while (total < 6 && rounds++ < 10) {
        memset(g, 0xEE, sizeof g);
        size_t cap = 20 + 2 * 44 + 43; /* 2 records + 43 spare bytes: third does not fit */
        TT_ASSERT_EQ(0, rec_batch_from_ring(&rb, next, 0x1234u, g, cap, &on, &cnt));
        TT_ASSERT_EQ(2, cnt);
        TT_ASSERT_EQ(20 + 2 * 44, on);
        for (size_t i = cap; i < sizeof g; i++) TT_ASSERT(g[i] == 0xEE);
        TT_ASSERT_EQ(next, rd32(g + 8));
        next += cnt; total += cnt;
    }
    TT_ASSERT_EQ(6, total);
    TT_ASSERT_EQ(3, rounds);
    /* too small for even one record */
    TT_ASSERT_EQ(-EMSGSIZE, rec_batch_from_ring(&rb, 0xFFFFFFFDu, 0x1234u, g, 20 + 43, &on, &cnt));
    TT_ASSERT_EQ(-ENOSPC, rec_batch_from_ring(&rb, 0xFFFFFFFDu, 0x1234u, g, 19, &on, &cnt));
    /* caught up: keep-alive carrying the next expected seq */
    TT_ASSERT_EQ(0, rec_batch_from_ring(&rb, 3, 0x1234u, g, sizeof g, &on, &cnt));
    TT_ASSERT_EQ(0, cnt); TT_ASSERT_EQ(3, rd32(g + 8));
    /* evicted: GAP, first batch starts at oldest */
    uint8_t m2[200]; rb_t r2;
    rb_init(&r2, m2, sizeof m2, 0);
    for (uint32_t i = 0; i < 10; i++) rb_push(&r2, i, raw, 30, NULL); /* 4 records fit (176 B) */
    uint32_t oldest; rb_oldest_seq(&r2, &oldest);
    TT_ASSERT_EQ(6, oldest);
    TT_ASSERT_EQ(0, rec_batch_from_ring(&r2, 2, 0x1234u, out, sizeof out, &on, &cnt));
    TT_ASSERT_EQ(0, rec_batch_parse_header(out, on, &h));
    TT_ASSERT_EQ(REC_FLAG_GAP, h.flags); TT_ASSERT_EQ(6, h.first_seq); TT_ASSERT_EQ(4, h.count);
    TT_ASSERT_EQ(0, rec_batch_from_ring(&r2, 6, 0x1234u, out, sizeof out, &on, &cnt));
    TT_ASSERT_EQ(0, out[5]);
    TT_ASSERT_EQ(-EINVAL, rec_batch_from_ring(&r2, 0, 0, out, sizeof out, &on, &cnt)); /* boot_id 0 */
    TT_ASSERT_EQ(-EINVAL, rec_batch_from_ring(NULL, 0, 0x1234u, out, sizeof out, &on, &cnt));
}

int main(void)
{
    TT_RUN(test_request);
    TT_RUN(test_builder_layout_and_limits);
    TT_RUN(test_count_limit);
    TT_RUN(test_vector_batch);
    TT_RUN(test_vector_reboot);
    TT_RUN(test_batch_helpers);
    TT_RUN(test_from_ring);
    return TT_RESULT();
}
