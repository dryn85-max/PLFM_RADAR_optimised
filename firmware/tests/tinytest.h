/* Minimal host test framework: TT_ASSERT, TT_ASSERT_EQ, TT_RUN. */
#ifndef TINYTEST_H
#define TINYTEST_H
#include <stdio.h>
#include <stdlib.h>

static int tt_failures = 0;
static int tt_checks = 0;

#define TT_ASSERT(cond)                                                        \
    do {                                                                       \
        tt_checks++;                                                           \
        if (!(cond)) {                                                         \
            tt_failures++;                                                     \
            printf("  FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);           \
        }                                                                      \
    } while (0)

#define TT_ASSERT_EQ(expected, actual)                                         \
    do {                                                                       \
        long long tt_e_ = (long long)(expected);                               \
        long long tt_a_ = (long long)(actual);                                 \
        tt_checks++;                                                           \
        if (tt_e_ != tt_a_) {                                                  \
            tt_failures++;                                                     \
            printf("  FAIL %s:%d: %s == %s (expected %lld, got %lld)\n",       \
                   __FILE__, __LINE__, #expected, #actual, tt_e_, tt_a_);      \
        }                                                                      \
    } while (0)

/* Run one test function; it must start from a clean state (call mock_reset()). */
#define TT_RUN(fn)                                                             \
    do {                                                                       \
        int tt_before_ = tt_failures;                                          \
        fn();                                                                  \
        printf("%s %s\n", tt_failures == tt_before_ ? "ok  " : "FAIL", #fn);   \
    } while (0)

/* Call at the end of main(): returns the process exit code. */
#define TT_RESULT()                                                            \
    (printf("%d checks, %d failures\n", tt_checks, tt_failures),               \
     tt_failures ? 1 : 0)

#endif
