/* Host tests for log_ring: wrap, gap, offset wrap, ANSI strip, secret filter. */
#include <string.h>
#include "tinytest.h"
#include "log_ring.h"

static uint8_t mem[16];

static void test_basic(void)
{
    log_ring_t r; char o[32]; uint32_t nx = 99; bool gap = true;
    log_ring_init(&r, mem, sizeof mem);
    TT_ASSERT_EQ(0, log_ring_read(&r, 0, o, sizeof o, &nx, &gap));
    TT_ASSERT_EQ(0, nx); TT_ASSERT(!gap);
    log_ring_write(&r, "hello", 5);
    TT_ASSERT_EQ(5, log_ring_read(&r, 0, o, sizeof o, &nx, &gap));
    TT_ASSERT(memcmp(o, "hello", 5) == 0); TT_ASSERT_EQ(5, nx); TT_ASSERT(!gap);
    log_ring_write(&r, " you", 4);
    TT_ASSERT_EQ(4, log_ring_read(&r, nx, o, sizeof o, &nx, &gap));
    TT_ASSERT(memcmp(o, " you", 4) == 0); TT_ASSERT_EQ(9, nx); TT_ASSERT(!gap);
    TT_ASSERT_EQ(0, log_ring_read(&r, nx, o, sizeof o, &nx, &gap)); /* caught up */
    TT_ASSERT_EQ(9, nx); TT_ASSERT(!gap);
    log_ring_write(&r, "x", 0);
    TT_ASSERT_EQ(9, r.head);
}

static void test_exact_cap(void)
{
    log_ring_t r; char o[32]; uint32_t nx; bool gap;
    log_ring_init(&r, mem, sizeof mem);
    log_ring_write(&r, "0123456789ABCDEF", 16);
    TT_ASSERT_EQ(16, log_ring_read(&r, 0, o, sizeof o, &nx, &gap));
    TT_ASSERT(memcmp(o, "0123456789ABCDEF", 16) == 0); TT_ASSERT(!gap);
    TT_ASSERT_EQ(16, nx);
    log_ring_write(&r, "G", 1); /* evicts '0' */
    TT_ASSERT_EQ(16, log_ring_read(&r, 1, o, sizeof o, &nx, &gap));
    TT_ASSERT(memcmp(o, "123456789ABCDEFG", 16) == 0);
    TT_ASSERT(!gap);
    TT_ASSERT_EQ(16, log_ring_read(&r, 0, o, sizeof o, &nx, &gap)); /* gap: 0 gone */
    TT_ASSERT(gap);
    TT_ASSERT(memcmp(o, "123456789ABCDEFG", 16) == 0);
    TT_ASSERT_EQ(17, nx);
}

static void test_wrap_read_across_end(void)
{
    log_ring_t r; char o[32]; uint32_t nx; bool gap;
    log_ring_init(&r, mem, sizeof mem);
    log_ring_write(&r, "AAAAAAAAAAAA", 12);          /* wpos 12 */
    log_ring_write(&r, "bbbbbbbb", 8);               /* crosses end, evicts 4 */
    TT_ASSERT_EQ(16, log_ring_read(&r, 4, o, sizeof o, &nx, &gap));
    TT_ASSERT(!gap);
    TT_ASSERT(memcmp(o, "AAAAAAAAbbbbbbbb", 16) == 0);
    TT_ASSERT_EQ(20, nx);
    /* partial read starting inside the wrapped part */
    TT_ASSERT_EQ(5, log_ring_read(&r, 9, o, 5, &nx, &gap));
    TT_ASSERT(memcmp(o, "AAAbb", 5) == 0); TT_ASSERT_EQ(14, nx);
}

static void test_len_gt_cap(void)
{
    log_ring_t r; char o[32]; uint32_t nx; bool gap;
    log_ring_init(&r, mem, sizeof mem);
    log_ring_write(&r, "abc", 3);
    log_ring_write(&r, "0123456789ABCDEFGHIJ", 20);
    TT_ASSERT_EQ(23, r.head);
    TT_ASSERT_EQ(16, log_ring_read(&r, 0, o, sizeof o, &nx, &gap));
    TT_ASSERT(gap);
    TT_ASSERT(memcmp(o, "456789ABCDEFGHIJ", 16) == 0);
    TT_ASSERT_EQ(23, nx);
    /* write continues correctly afterwards */
    log_ring_write(&r, "!", 1);
    TT_ASSERT_EQ(1, log_ring_read(&r, 23, o, sizeof o, &nx, &gap));
    TT_ASSERT(o[0] == '!'); TT_ASSERT(!gap);
}

static void test_gap_and_future(void)
{
    log_ring_t r; char o[32]; uint32_t nx; bool gap;
    log_ring_init(&r, mem, sizeof mem);
    for (int i = 0; i < 10; i++) log_ring_write(&r, "0123456789", 10); /* head 100 */
    TT_ASSERT_EQ(16, log_ring_read(&r, 50, o, sizeof o, &nx, &gap));
    TT_ASSERT(gap); TT_ASSERT_EQ(100, nx);
    TT_ASSERT(memcmp(o, "4567890123456789", 16) == 0);
    TT_ASSERT_EQ(16, log_ring_read(&r, 101, o, sizeof o, &nx, &gap)); /* ahead */
    TT_ASSERT(gap); TT_ASSERT_EQ(100, nx);
    TT_ASSERT_EQ(16, log_ring_read(&r, 0x80000000u, o, sizeof o, &nx, &gap));
    TT_ASSERT(gap);
    TT_ASSERT_EQ(16, log_ring_read(&r, 0xFFFFFFF0u, o, sizeof o, &nx, &gap));
    TT_ASSERT(gap); TT_ASSERT_EQ(100, nx);
    TT_ASSERT_EQ(0, log_ring_read(&r, 100, o, sizeof o, &nx, &gap));
    TT_ASSERT(!gap);
    /* gap with small max: next advances from the oldest held */
    TT_ASSERT_EQ(4, log_ring_read(&r, 0, o, 4, &nx, &gap));
    TT_ASSERT(gap); TT_ASSERT_EQ(88, nx);
    TT_ASSERT(memcmp(o, "4567", 4) == 0);
}

static void test_offset_wrap(void)
{
    log_ring_t r; char o[32]; uint32_t nx; bool gap;
    log_ring_init_at(&r, mem, 10 /* not a power of two */, 0xFFFFFFF8u);
    log_ring_write(&r, "abcdef", 6);            /* head 0xFFFFFFFE */
    log_ring_write(&r, "ghijkl", 6);            /* head wraps to 4 */
    TT_ASSERT_EQ(4, r.head);
    TT_ASSERT_EQ(10, log_ring_read(&r, 0xFFFFFFFAu, o, sizeof o, &nx, &gap));
    TT_ASSERT(!gap);
    TT_ASSERT(memcmp(o, "cdefghijkl", 10) == 0);
    TT_ASSERT_EQ(4, nx);
    TT_ASSERT_EQ(10, log_ring_read(&r, 0xFFFFFFF8u, o, sizeof o, &nx, &gap));
    TT_ASSERT(gap); TT_ASSERT_EQ(4, nx);
    TT_ASSERT_EQ(3, log_ring_read(&r, 1, o, sizeof o, &nx, &gap));
    TT_ASSERT(!gap); TT_ASSERT(memcmp(o, "jkl", 3) == 0);
    log_ring_write(&r, "mn", 2);
    TT_ASSERT_EQ(2, log_ring_read(&r, 4, o, sizeof o, &nx, &gap));
    TT_ASSERT(!gap); TT_ASSERT(memcmp(o, "mn", 2) == 0); TT_ASSERT_EQ(6, nx);
}

static void test_max_small(void)
{
    log_ring_t r; char o[32]; uint32_t nx; bool gap;
    log_ring_init(&r, mem, sizeof mem);
    log_ring_write(&r, "abcdefghij", 10);
    TT_ASSERT_EQ(3, log_ring_read(&r, 0, o, 3, &nx, &gap));
    TT_ASSERT(memcmp(o, "abc", 3) == 0); TT_ASSERT_EQ(3, nx);
    TT_ASSERT_EQ(3, log_ring_read(&r, nx, o, 3, &nx, &gap));
    TT_ASSERT(memcmp(o, "def", 3) == 0); TT_ASSERT_EQ(6, nx);
    TT_ASSERT_EQ(0, log_ring_read(&r, nx, o, 0, &nx, &gap));
    TT_ASSERT_EQ(6, nx); TT_ASSERT(!gap);
    TT_ASSERT_EQ(4, log_ring_read(&r, nx, o, 100, NULL, NULL));
}

static void test_cap1_and_model(void)
{
    log_ring_t r; uint8_t m1[1]; char o[8]; uint32_t nx; bool gap;
    log_ring_init(&r, m1, 1);
    log_ring_write(&r, "xyz", 3);
    TT_ASSERT_EQ(1, log_ring_read(&r, 0, o, sizeof o, &nx, &gap));
    TT_ASSERT(gap && o[0] == 'z' && nx == 3);
    /* model check: odd cap, random chunks, a reader following along */
    uint8_t m7[7]; char model[4096]; size_t mlen = 0; uint32_t rd = 0, seed = 1;
    log_ring_init(&r, m7, 7);
    for (int it = 0; it < 500; it++) {
        seed = seed * 1103515245u + 12345u;
        size_t n = (seed >> 16) % 12;
        char chunk[12];
        for (size_t i = 0; i < n; i++) chunk[i] = (char)('a' + (mlen + i) % 26);
        memcpy(model + mlen, chunk, n); mlen += n;
        log_ring_write(&r, chunk, n);
        char got[16];
        size_t g = log_ring_read(&r, rd, got, sizeof got, &nx, &gap);
        uint32_t oldest = mlen > 7 ? (uint32_t)mlen - 7 : 0;
        uint32_t exp_from = (rd < oldest) ? oldest : rd;
        TT_ASSERT_EQ(rd < oldest, gap);
        size_t avail = mlen - exp_from;
        TT_ASSERT_EQ(avail < sizeof got ? avail : sizeof got, g);
        TT_ASSERT(memcmp(got, model + exp_from, g) == 0);
        TT_ASSERT_EQ(exp_from + g, nx);
        rd = nx;
    }
}

static void test_clean(void)
{
    char o[64]; size_t n;
    const char *l = "\x1b[0;32mI (123) tag: msg\x1b[0m\n";
    n = log_line_clean(l, strlen(l), o, sizeof o);
    TT_ASSERT_EQ(strlen("I (123) tag: msg\n"), n);
    TT_ASSERT(memcmp(o, "I (123) tag: msg\n", n) == 0);
    n = log_line_clean("abc\x1b[3", 6, o, sizeof o); /* truncated escape */
    TT_ASSERT_EQ(3, n); TT_ASSERT(memcmp(o, "abc", 3) == 0);
    n = log_line_clean("abc\x1b", 4, o, sizeof o);
    TT_ASSERT_EQ(3, n);
    n = log_line_clean("abc\x1b[", 5, o, sizeof o);
    TT_ASSERT_EQ(3, n);
    n = log_line_clean("a\x1bZb", 4, o, sizeof o); /* ESC not followed by '[' */
    TT_ASSERT_EQ(3, n); TT_ASSERT(memcmp(o, "aZb", 3) == 0);
    n = log_line_clean("a\x1b[1;31\nb", 9, o, sizeof o); /* malformed: keep '\n' */
    TT_ASSERT_EQ(3, n); TT_ASSERT(memcmp(o, "a\nb", 3) == 0);
    n = log_line_clean("\x1b[K\x1b[2Jx", 8, o, sizeof o); /* back-to-back */
    TT_ASSERT_EQ(1, n); TT_ASSERT(o[0] == 'x');
    n = log_line_clean("abcdef", 6, o, 4); /* output truncated */
    TT_ASSERT_EQ(4, n); TT_ASSERT(memcmp(o, "abcd", 4) == 0);
    TT_ASSERT_EQ(0, log_line_clean("", 0, o, sizeof o));
    TT_ASSERT_EQ(0, log_line_clean("abc", 3, o, 0));
    char io[] = "\x1b[0mhi\x1b[0m"; /* in place */
    n = log_line_clean(io, strlen(io), io, sizeof io);
    TT_ASSERT_EQ(2, n); TT_ASSERT(memcmp(io, "hi", 2) == 0);
}

static void test_secret(void)
{
    TT_ASSERT(log_line_is_secret("I (5) wifi: AP password: hunter2\n", 33));
    TT_ASSERT(log_line_is_secret("AP password", 11));
    TT_ASSERT(!log_line_is_secret("AP passwor", 10));
    TT_ASSERT(!log_line_is_secret("ap password", 11)); /* case-sensitive */
    TT_ASSERT(!log_line_is_secret("AP  password", 12));
    TT_ASSERT(!log_line_is_secret("", 0));
    TT_ASSERT(!log_line_is_secret("AP password", 5)); /* len limits the scan */
    TT_ASSERT(log_line_is_secret("xxAP passwordyy", 15));
}

int main(void)
{
    TT_RUN(test_basic);
    TT_RUN(test_exact_cap);
    TT_RUN(test_wrap_read_across_end);
    TT_RUN(test_len_gt_cap);
    TT_RUN(test_gap_and_future);
    TT_RUN(test_offset_wrap);
    TT_RUN(test_max_small);
    TT_RUN(test_cap1_and_model);
    TT_RUN(test_clean);
    TT_RUN(test_secret);
    return TT_RESULT();
}
