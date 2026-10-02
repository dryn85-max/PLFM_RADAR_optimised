/* Host tests for rec_proto: request, batch builder, ring batches, shared batch vector. */
#include <errno.h>
#include <string.h>
#include "tinytest.h"
#include "vec_util.h"
#include "rec_proto.h"
#include "rec_payload.h"

static uint32_t rd32(const uint8_t *p) { return p[0] | (p[1] << 8) | (p[2] << 16) | ((uint32_t)p[3] << 24); }
static uint16_t rd16(const uint8_t *p) { return (uint16_t)(p[0] | (p[1] << 8)); }

static void test_request(void)
{
    uint8_t r[REC_REQ_LEN]; uint32_t from = 0;
    TT_ASSERT_EQ(0, rec_req_encode(r, 0xDEADBEEFu));
    static const uint8_t exp[12] = {'L', 'D', 'R', 'Q', 3, 0, 0, 0, 0xEF, 0xBE, 0xAD, 0xDE};
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
    { uint8_t t[12]; memcpy(t, r, 12); t[4] = 2; TT_ASSERT_EQ(-ENOTSUP, rec_req_parse(t, 12, &from)); } /* v2 clients are refused */
    { uint8_t t[12]; memcpy(t, r, 12); t[4] = 4; TT_ASSERT_EQ(-ENOTSUP, rec_req_parse(t, 12, &from)); }
    { uint8_t t[12]; memcpy(t, r, 12); t[4] = 0; TT_ASSERT_EQ(-ENOTSUP, rec_req_parse(t, 12, &from)); }
    TT_ASSERT_EQ(-EINVAL, rec_req_parse(NULL, 12, &from));
    TT_ASSERT_EQ(-EINVAL, rec_req_parse(r, 12, NULL));
}

static void test_builder_layout_and_limits(void)
{
    uint8_t buf[200]; rec_batch_t b; rec_batch_hdr_t h;
    const uint8_t raw[5] = {1, 2, 3, 4, 5};
    TT_ASSERT_EQ(0, rec_batch_begin(&b, buf, sizeof buf, 0x01020304u, REC_FLAG_GAP, 0xCAFEF00Du));
    TT_ASSERT_EQ(0, rec_batch_add(&b, 7, 0x1122334455667788ull, 0x5A, raw, 5));
    size_t n = rec_batch_finish(&b);
    TT_ASSERT_EQ(20 + 15 + 5, n);
    TT_ASSERT(memcmp(buf, "LDRB", 4) == 0);
    TT_ASSERT_EQ(3, buf[4]); TT_ASSERT_EQ(1, buf[5]); TT_ASSERT_EQ(0, rd16(buf + 6));
    TT_ASSERT_EQ(0x01020304u, rd32(buf + 8)); TT_ASSERT_EQ(1, rd16(buf + 12)); TT_ASSERT_EQ(0, rd16(buf + 14)); TT_ASSERT_EQ(0xCAFEF00Du, rd32(buf + 16));
    TT_ASSERT_EQ(7, rd32(buf + 20)); TT_ASSERT_EQ(0x55667788u, rd32(buf + 24)); TT_ASSERT_EQ(0x11223344u, rd32(buf + 28));
    TT_ASSERT_EQ(0x5A, buf[32]); TT_ASSERT_EQ(5, rd16(buf + 33)); TT_ASSERT(memcmp(buf + 35, raw, 5) == 0);
    TT_ASSERT_EQ(0, rec_batch_parse_header(buf, n, &h));
    TT_ASSERT_EQ(3, h.version); TT_ASSERT_EQ(1, h.flags); TT_ASSERT_EQ(0x01020304u, h.first_seq); TT_ASSERT_EQ(1, h.count);
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
    TT_ASSERT_EQ(0, rec_batch_begin(&b, g, 40, 0, 0, 1));
    TT_ASSERT_EQ(0, rec_batch_add(&b, 1, 2, 0, raw, 5));
    TT_ASSERT_EQ(40, b.len);
    TT_ASSERT_EQ(-ENOSPC, rec_batch_add(&b, 2, 3, 0, raw, 0)); /* needs 15 more */
    TT_ASSERT_EQ(40, b.len);
    TT_ASSERT_EQ(1, b.count);
    TT_ASSERT_EQ(40, rec_batch_finish(&b));
    for (size_t i = 40; i < sizeof g; i++) TT_ASSERT(g[i] == 0xEE);
    memset(g, 0xEE, sizeof g);
    TT_ASSERT_EQ(0, rec_batch_begin(&b, g, 39, 0, 0, 1));
    TT_ASSERT_EQ(-ENOSPC, rec_batch_add(&b, 1, 2, 0, raw, 5));
    TT_ASSERT_EQ(0, b.count);
    TT_ASSERT_EQ(20, rec_batch_finish(&b));
    for (size_t i = 39; i < sizeof g; i++) TT_ASSERT(g[i] == 0xEE);
    TT_ASSERT_EQ(-EINVAL, rec_batch_add(&b, 1, 2, 0, NULL, 5));
    TT_ASSERT_EQ(-EINVAL, rec_batch_add(&b, 1, 2, 0, raw, 70000));
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
    t[4] = 2; TT_ASSERT_EQ(-ENOTSUP, rec_batch_parse_header(t, 20, &h)); /* v2 batches are not accepted */
}

static void test_count_limit(void)
{
    static uint8_t big[20 + 65540 * 15];
    rec_batch_t b;
    TT_ASSERT_EQ(0, rec_batch_begin(&b, big, sizeof big, 0, 0, 1));
    static const uint8_t empty[1] = {0};
    int ok = 0;
    for (int i = 0; i < 65536; i++) {
        int r = rec_batch_add(&b, (uint32_t)i, 0, 0, empty, 0);
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
        unsigned long long ty = json_u64_at(j, "type", &pos);
        size_t rl = json_hex(j, "raw_hex", &pos, raw, sizeof raw);
        TT_ASSERT_EQ(0, rec_batch_add(&b, (uint32_t)seq, tm, (uint8_t)ty, raw, rl));
    }
    size_t on = rec_batch_finish(&b);
    TT_ASSERT_EQ(n, on);
    TT_ASSERT(on == n && memcmp(out, bin, n) == 0);
    /* and walk the .bin records */
    size_t off = 20; unsigned cnt = 0;
    while (off + 15 <= n) { off += 15 + rd16(bin + off + 13); cnt++; }
    TT_ASSERT_EQ(off, n);
    TT_ASSERT_EQ(h.count, cnt);
    TT_ASSERT_EQ(0xFFFFFFFFu, rd32(bin + 20));
    TT_ASSERT_EQ(0xA1B2C3D4u, h.boot_id);
}

/* Mixed v3 batch (frame, gps_fix, imu, time_sync, frame; seq wraps): rebuilt
 * from the JSON, rebuilt from a ring, walked record by record, every payload
 * checked against its own vector. */
static void test_vector_mixed(void)
{
    static uint8_t bin[1024], json[8192], out[1024], r0[64], r1[64];
    size_t n = vec_read("batch_v3_mixed.bin", bin, sizeof bin);
    vec_read("batch_v3_mixed.json", json, sizeof json);
    const char *j = (const char *)json;
    rec_batch_hdr_t h;
    TT_ASSERT_EQ(0, rec_batch_parse_header(bin, n, &h));
    TT_ASSERT_EQ(3, h.version); TT_ASSERT_EQ(0, h.flags); TT_ASSERT_EQ(5, h.count);
    TT_ASSERT_EQ(0xFFFFFFFDu, h.first_seq); TT_ASSERT_EQ(0x0BADCAFEu, h.boot_id);
    TT_ASSERT_EQ(json_u64(j, "boot_id"), h.boot_id);
    size_t n0 = vec_read("frame_engineering_real_0.bin", r0, sizeof r0);
    size_t n1 = vec_read("frame_engineering_real_1.bin", r1, sizeof r1);
    static const uint8_t want_type[5] = {0, 1, 2, 3, 0};
    static const uint32_t want_seq[5] = {0xFFFFFFFDu, 0xFFFFFFFEu, 0xFFFFFFFFu, 0, 1};
    rb_t rb; static uint8_t mem[2048];
    rb_init(&rb, mem, sizeof mem, 0xFFFFFFFDu);
    rec_batch_t b;
    TT_ASSERT_EQ(0, rec_batch_begin(&b, out, sizeof out, h.first_seq, h.flags, h.boot_id));
    size_t off = 20;
    const char *pos = strstr(j, "\"records\"");
    for (unsigned i = 0; i < 5; i++) {
        uint8_t raw[128];
        unsigned long long seq = json_u64_at(j, "seq", &pos);
        unsigned long long tm = json_u64_at(j, "esp_time_us", &pos);
        unsigned long long ty = json_u64_at(j, "type", &pos);
        size_t rl = json_hex(j, "raw_hex", &pos, raw, sizeof raw);
        TT_ASSERT_EQ(want_seq[i], seq); TT_ASSERT_EQ(want_type[i], ty);
        TT_ASSERT_EQ(want_seq[i], rd32(bin + off));
        TT_ASSERT_EQ(want_type[i], bin[off + 12]);
        TT_ASSERT_EQ(rl, rd16(bin + off + 13));
        TT_ASSERT_EQ(0, rec_record_check((uint8_t)ty, rl));
        TT_ASSERT(memcmp(bin + off + 15, raw, rl) == 0);
        TT_ASSERT_EQ(0, rec_batch_add(&b, (uint32_t)seq, tm, (uint8_t)ty, raw, rl));
        TT_ASSERT_EQ(0, rb_push(&rb, tm, (uint8_t)ty, raw, rl, NULL));
        off += 15 + rl;
    }
    TT_ASSERT_EQ(n, off);
    TT_ASSERT_EQ(n, rec_batch_finish(&b));
    TT_ASSERT(memcmp(out, bin, n) == 0);
    /* the real frames travel as type 0 records, unmodified */
    TT_ASSERT(memcmp(bin + 20 + 15, r0, n0) == 0);
    TT_ASSERT(memcmp(bin + n - n1, r1, n1) == 0);
    /* same bytes from the ring, whole and in 2-record slices (stream cursor) */
    static uint8_t out2[1024]; size_t on; uint16_t cnt;
    TT_ASSERT_EQ(0, rec_batch_from_ring(&rb, 0xFFFFFFFDu, h.boot_id, out2, sizeof out2, &on, &cnt));
    TT_ASSERT_EQ(5, cnt); TT_ASSERT_EQ(n, on); TT_ASSERT(memcmp(out2, bin, n) == 0);
    rec_stream_t st; rec_stream_init(&st);
    uint32_t next = 0xFFFFFFFDu; unsigned total = 0, k = 0;
    while (total < 5 && k++ < 5) {
        size_t cap = 20 + 15 + 32 + 15 + 18 + 4; /* gps + imu fit, the third record does not */
        TT_ASSERT_EQ(0, rec_stream_batch(&rb, &st, next, h.boot_id, out2, cap, &on, &cnt));
        TT_ASSERT(cnt >= 1);
        TT_ASSERT_EQ(want_seq[total], rd32(out2 + 8));
        TT_ASSERT_EQ(0, rec_batch_next_seq(out2, on, &next));
        total += cnt;
    }
    TT_ASSERT_EQ(5, total);
    /* payload vectors are the records' payloads */
    static uint8_t p[64];
    size_t pn = vec_read("gps_fix_nominal.bin", p, sizeof p);
    TT_ASSERT_EQ(pn, rd16(bin + 20 + 15 + n0 + 13));
    TT_ASSERT(memcmp(bin + 20 + 15 + n0 + 15, p, pn) == 0);
    pn = vec_read("imu_nominal.bin", p, sizeof p);
    size_t o2 = 20 + 15 + n0 + 15 + 32;
    TT_ASSERT_EQ(pn, rd16(bin + o2 + 13));
    TT_ASSERT(memcmp(bin + o2 + 15, p, pn) == 0);
    pn = vec_read("time_sync_gps.bin", p, sizeof p);
    size_t o3 = o2 + 15 + 18;
    TT_ASSERT_EQ(pn, rd16(bin + o3 + 13));
    TT_ASSERT(memcmp(bin + o3 + 15, p, pn) == 0);
}

/* Unknown type bytes are carried through the ring and the batch unchanged
 * (the transport is type-agnostic), also with empty payloads. */
static void test_unknown_types_pass_through(void)
{
    rb_t rb; static uint8_t m[512]; uint8_t out[256]; size_t on; uint16_t cnt;
    rb_init(&rb, m, sizeof m, 0);
    static const uint8_t types[] = {0x04, 0x7F, 0x80, 0xFF, 0x00};
    for (unsigned i = 0; i < sizeof types; i++) {
        uint8_t raw[3] = {(uint8_t)i, 2, 3};
        TT_ASSERT_EQ(0, rb_push(&rb, i, types[i], raw, i % 2 ? 0 : 3, NULL));
    }
    TT_ASSERT_EQ(0, rec_batch_from_ring(&rb, 0, 9, out, sizeof out, &on, &cnt));
    TT_ASSERT_EQ(5, cnt);
    size_t off = 20;
    for (unsigned i = 0; i < 5; i++) {
        TT_ASSERT_EQ(types[i], out[off + 12]);
        off += 15 + rd16(out + off + 13);
    }
    TT_ASSERT_EQ(on, off);
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
    TT_ASSERT_EQ(0, rec_batch_add(&b, 0xFFFFFFFEu, 1, 0, raw, 3));
    TT_ASSERT_EQ(0, rec_batch_add(&b, 0xFFFFFFFFu, 2, 0, raw, 3));
    TT_ASSERT_EQ(0, rec_batch_add(&b, 0, 3, 0, raw, 3));
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
    for (uint32_t i = 0; i < 6; i++) { memset(raw, (int)i, sizeof raw); rb_push(&rb, i, (uint8_t)(i), raw, 30, NULL); }
    /* seq runs FFFFFFFD..FFFFFFFF,0,1,2; each record is 15+30 = 45 bytes */
    TT_ASSERT_EQ(0, rec_batch_from_ring(&rb, 0xFFFFFFFDu, 0x1234u, out, sizeof out, &on, &cnt));
    TT_ASSERT_EQ(6, cnt); TT_ASSERT_EQ(20 + 6 * 45, on);
    TT_ASSERT_EQ(0, rec_batch_parse_header(out, on, &h));
    TT_ASSERT_EQ(0xFFFFFFFDu, h.first_seq); TT_ASSERT_EQ(0, h.flags); TT_ASSERT_EQ(6, h.count);
    TT_ASSERT_EQ(0xFFFFFFFFu, rd32(out + 20 + 2 * 45));
    TT_ASSERT_EQ(0, rd32(out + 20 + 3 * 45));
    TT_ASSERT_EQ(1, rd32(out + 20 + 4 * 45));
    TT_ASSERT_EQ(3, out[20 + 3 * 45 + 15]);
    TT_ASSERT_EQ(3, out[20 + 3 * 45 + 12]); /* the type byte travels with the record */
    TT_ASSERT_EQ(30, rd16(out + 20 + 3 * 45 + 13));
    /* start mid-wrap */
    TT_ASSERT_EQ(0, rec_batch_from_ring(&rb, 0, 0x1234u, out, sizeof out, &on, &cnt));
    TT_ASSERT_EQ(3, cnt);
    TT_ASSERT_EQ(0, rd32(out + 8));
    /* batch that does not fit: only whole records, guard bytes untouched, resume works */
    uint8_t g[300]; uint32_t next = 0xFFFFFFFDu; int total = 0, rounds = 0;
    while (total < 6 && rounds++ < 10) {
        memset(g, 0xEE, sizeof g);
        size_t cap = 20 + 2 * 45 + 44; /* 2 records + 44 spare bytes: third does not fit */
        TT_ASSERT_EQ(0, rec_batch_from_ring(&rb, next, 0x1234u, g, cap, &on, &cnt));
        TT_ASSERT_EQ(2, cnt);
        TT_ASSERT_EQ(20 + 2 * 45, on);
        for (size_t i = cap; i < sizeof g; i++) TT_ASSERT(g[i] == 0xEE);
        TT_ASSERT_EQ(next, rd32(g + 8));
        next += cnt; total += cnt;
    }
    TT_ASSERT_EQ(6, total);
    TT_ASSERT_EQ(3, rounds);
    /* too small for even one record */
    TT_ASSERT_EQ(-EMSGSIZE, rec_batch_from_ring(&rb, 0xFFFFFFFDu, 0x1234u, g, 20 + 44, &on, &cnt));
    TT_ASSERT_EQ(-ENOSPC, rec_batch_from_ring(&rb, 0xFFFFFFFDu, 0x1234u, g, 19, &on, &cnt));
    /* caught up: keep-alive carrying the next expected seq */
    TT_ASSERT_EQ(0, rec_batch_from_ring(&rb, 3, 0x1234u, g, sizeof g, &on, &cnt));
    TT_ASSERT_EQ(0, cnt); TT_ASSERT_EQ(3, rd32(g + 8));
    /* evicted: GAP, first batch starts at oldest */
    uint8_t m2[200]; rb_t r2;
    rb_init(&r2, m2, sizeof m2, 0);
    for (uint32_t i = 0; i < 10; i++) rb_push(&r2, i, (uint8_t)(i), raw, 30, NULL); /* 4 records fit (176 B) */
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

static uint8_t streammem[400];

/* Cursor reuse across batches must equal reopening from the next seq. */
static void test_stream_equals_reopen(void)
{
    rb_t rb; rec_stream_t st; uint8_t a[300], b[300]; size_t an, bn; uint16_t ac, bc;
    uint8_t raw[30]; uint32_t next = 0xFFFFFFFDu;
    rb_init(&rb, streammem, sizeof streammem, 0xFFFFFFFDu); /* wraps at 2^32 */
    rec_stream_init(&st);
    for (uint32_t round = 0; round < 12; round++) {
        /* 1-3 new records per round; the 400 B ring holds 9, never evicts a pending one */
        for (uint32_t i = 0; i < 1 + round % 3; i++) {
            memset(raw, (int)(round * 7 + i), sizeof raw);
            rb_push(&rb, round * 100 + i, (uint8_t)(round * 100 + i), raw, 30, NULL);
        }
        /* cap 20 + 2*45 + 10: two records per batch, so batches continue the cursor */
        for (int k = 0; k < 3; k++) {
            size_t cap = 20 + 2 * 45 + 10;
            TT_ASSERT_EQ(0, rec_stream_batch(&rb, &st, next, 0x77u, a, cap, &an, &ac));
            TT_ASSERT_EQ(0, rec_batch_from_ring(&rb, next, 0x77u, b, cap, &bn, &bc));
            TT_ASSERT_EQ(bn, an); TT_ASSERT_EQ(bc, ac);
            TT_ASSERT(memcmp(a, b, an) == 0);
            TT_ASSERT_EQ(0, rec_batch_next_seq(a, an, &next));
        }
    }
    TT_ASSERT_EQ(next, rb_next_seq(&rb));
}

static void test_stream_stale(void)
{
    rb_t rb; rec_stream_t st; uint8_t out[400]; size_t on; uint16_t cnt; rec_batch_hdr_t h;
    uint8_t raw[30]; uint8_t m[200];
    rb_init(&rb, m, sizeof m, 0);
    rec_stream_init(&st);
    memset(raw, 1, sizeof raw);
    for (uint32_t i = 0; i < 3; i++) rb_push(&rb, i, (uint8_t)(i), raw, 30, NULL);
    TT_ASSERT_EQ(0, rec_stream_batch(&rb, &st, 0, 0x77u, out, sizeof out, &on, &cnt));
    TT_ASSERT_EQ(3, cnt); TT_ASSERT_EQ(0, out[5]);
    /* caught up: keep-alive, no gap */
    TT_ASSERT_EQ(0, rec_stream_batch(&rb, &st, 0, 0x77u, out, sizeof out, &on, &cnt));
    TT_ASSERT_EQ(0, cnt); TT_ASSERT_EQ(0, out[5]); TT_ASSERT_EQ(3, rd32(out + 8));
    /* push 10 more: cursor (seq 3) is evicted (ring holds 4) */
    for (uint32_t i = 3; i < 13; i++) rb_push(&rb, i, (uint8_t)(i), raw, 30, NULL);
    TT_ASSERT_EQ(0, rec_stream_batch(&rb, &st, 0, 0x77u, out, sizeof out, &on, &cnt));
    TT_ASSERT_EQ(0, rec_batch_parse_header(out, on, &h));
    TT_ASSERT_EQ(REC_FLAG_GAP, h.flags); TT_ASSERT_EQ(9, h.first_seq); TT_ASSERT_EQ(4, h.count);
    /* afterwards no gap again */
    rb_push(&rb, 13, (uint8_t)(13), raw, 30, NULL);
    TT_ASSERT_EQ(0, rec_stream_batch(&rb, &st, 0, 0x77u, out, sizeof out, &on, &cnt));
    TT_ASSERT_EQ(0, out[5]); TT_ASSERT_EQ(13, rd32(out + 8)); TT_ASSERT_EQ(1, cnt);
    TT_ASSERT_EQ(-EINVAL, rec_stream_batch(&rb, NULL, 0, 0x77u, out, sizeof out, &on, &cnt));
}

static void test_stream_wrap_stale(void)
{
    /* eviction across the 2^32 seq wrap */
    rb_t rb; rec_stream_t st; uint8_t out[400]; size_t on; uint16_t cnt; rec_batch_hdr_t h;
    uint8_t raw[30] = {0}; uint8_t m[200];
    rb_init(&rb, m, sizeof m, 0xFFFFFFFEu);
    rec_stream_init(&st);
    rb_push(&rb, 0, (uint8_t)(0), raw, 30, NULL);
    TT_ASSERT_EQ(0, rec_stream_batch(&rb, &st, 0xFFFFFFFEu, 0x77u, out, sizeof out, &on, &cnt));
    TT_ASSERT_EQ(1, cnt);
    for (uint32_t i = 0; i < 8; i++) rb_push(&rb, i, (uint8_t)(i), raw, 30, NULL); /* oldest = 0xFFFFFFFF+5-3.. */
    TT_ASSERT_EQ(0, rec_stream_batch(&rb, &st, 0, 0x77u, out, sizeof out, &on, &cnt));
    TT_ASSERT_EQ(0, rec_batch_parse_header(out, on, &h));
    TT_ASSERT_EQ(REC_FLAG_GAP, h.flags);
    uint32_t oldest; rb_oldest_seq(&rb, &oldest);
    TT_ASSERT_EQ(oldest, h.first_seq); TT_ASSERT_EQ(4, h.count);
}

int main(void)
{
    TT_RUN(test_request);
    TT_RUN(test_builder_layout_and_limits);
    TT_RUN(test_count_limit);
    TT_RUN(test_vector_batch);
    TT_RUN(test_vector_mixed);
    TT_RUN(test_unknown_types_pass_through);
    TT_RUN(test_vector_reboot);
    TT_RUN(test_batch_helpers);
    TT_RUN(test_from_ring);
    TT_RUN(test_stream_equals_reopen);
    TT_RUN(test_stream_stale);
    TT_RUN(test_stream_wrap_stale);
    return TT_RESULT();
}
