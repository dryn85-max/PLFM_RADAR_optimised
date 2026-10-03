/* Host tests for origin_check: CSRF Origin rule of the AP-only POST endpoints. */
#include <string.h>
#include "tinytest.h"
#include "origin_check.h"

#define EXP "http://192.168.4.1"

static bool chk(const char *s) { return origin_allowed(s, strlen(s), EXP); }

static void test_absent_allowed(void) { TT_ASSERT(origin_allowed(NULL, 0, EXP)); }

static void test_exact_and_case(void)
{
    TT_ASSERT(chk("http://192.168.4.1"));
    TT_ASSERT(chk("HTTP://192.168.4.1"));
    TT_ASSERT(chk("Http://192.168.4.1"));
}

static void test_variants_rejected(void)
{
    TT_ASSERT(!chk("http://192.168.4.1/"));
    TT_ASSERT(!chk("http://192.168.4.1:80"));
    TT_ASSERT(!chk("http://192.168.4.1:8080"));
    TT_ASSERT(!chk("https://192.168.4.1"));
    TT_ASSERT(!chk("null"));
    TT_ASSERT(!chk(""));
    TT_ASSERT(!chk("http://192.168.4.10"));
    TT_ASSERT(!chk("http://192.168.4."));
    TT_ASSERT(!chk("http://evil.example"));
    TT_ASSERT(!chk("http://192.168.4.1.evil.example"));
}

static void test_len_and_nul(void)
{
    /* len shorter than the buffer: only len bytes count */
    TT_ASSERT(origin_allowed("http://192.168.4.1/", 18, EXP));
    TT_ASSERT(!origin_allowed("http://192.168.4.1", 17, EXP));
    TT_ASSERT(!origin_allowed("http://192.168.4.1", 19, EXP));
    /* embedded NUL with matching length */
    char b[] = "http://192.168.4\0" "1";
    TT_ASSERT(!origin_allowed(b, 18, EXP));
    TT_ASSERT(!origin_allowed("x", 1, NULL));
    TT_ASSERT(!origin_allowed("", 0, EXP));
}

int main(void)
{
    TT_RUN(test_absent_allowed);
    TT_RUN(test_exact_and_case);
    TT_RUN(test_variants_rejected);
    TT_RUN(test_len_and_nul);
    return TT_RESULT();
}
