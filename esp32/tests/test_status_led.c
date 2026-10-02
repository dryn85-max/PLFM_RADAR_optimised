/* Host tests for status_led: colour priority, flash and blink timing, uint32 wrap. */
#include <stdint.h>
#include "tinytest.h"
#include "status_led.h"

static int eq(sl_rgb_t a, sl_rgb_t b) { return a.r == b.r && a.g == b.g && a.b == b.b; }
#define T_COL(exp, act) TT_ASSERT(eq((exp), (act)))

static sl_in_t idle(void) { sl_in_t in = {0}; in.zone = BB_ZONE_NONE; in.ap = SL_AP_OFF; return in; }

static void test_off_by_default(void)
{
    sl_in_t in = idle();
    T_COL(SL_OFF, sl_color(&in, 0));
    T_COL(SL_OFF, sl_color(&in, 123456));
}

static void test_held_zones(void)
{
    sl_in_t in = idle();
    in.held = true;
    in.zone = BB_ZONE_NONE;   T_COL(SL_OFF, sl_color(&in, 5));
    in.zone = BB_ZONE_AP;     T_COL(SL_BLUE, sl_color(&in, 5));
    in.zone = BB_ZONE_RESET;  T_COL(SL_RED, sl_color(&in, 5));
    in.zone = BB_ZONE_CANCEL; T_COL(SL_OFF, sl_color(&in, 5));
    /* zone set but not held: ignored */
    in.held = false; in.zone = BB_ZONE_RESET;
    T_COL(SL_OFF, sl_color(&in, 5));
}

static void test_priority(void)
{
    sl_in_t in = idle();
    in.ap = SL_AP_ONLY;
    T_COL(SL_BLUE, sl_color(&in, 0));
    in.flash_active = true; in.flash_color = SL_RED; in.flash_start_ms = 1000;
    T_COL(SL_BLUE, sl_color(&in, 900));        /* before the flash starts: AP only */
    T_COL(SL_RED, sl_color(&in, 1000));        /* flash beats AP only */
    T_COL(SL_OFF, sl_color(&in, 1100));        /* flash off-phase is dark, not the AP colour */
    T_COL(SL_BLUE, sl_color(&in, 1600));       /* flash over (600 ms): AP only again */
    in.ap = SL_AP_ON_DEMAND; in.ap_ref_ms = 0;
    T_COL(SL_OFF, sl_color(&in, 1100));        /* flash off-phase beats the blink on-phase */
    T_COL(SL_RED, sl_color(&in, 1200));        /* flash on-phase beats the blink off-phase */
    T_COL(SL_OFF, sl_color(&in, 1700));        /* flash over: blink off-phase (700 ms) */
    T_COL(SL_BLUE, sl_color(&in, 2100));       /* blink on-phase (100 ms) */
    /* held beats flash and AP */
    in.held = true; in.zone = BB_ZONE_AP; in.flash_color = SL_RED;
    T_COL(SL_BLUE, sl_color(&in, 1000));
    in.zone = BB_ZONE_RESET; in.ap = SL_AP_ONLY;
    T_COL(SL_RED, sl_color(&in, 1100));
    in.zone = BB_ZONE_NONE;                    /* off while held, over flash and AP only */
    T_COL(SL_OFF, sl_color(&in, 1000));
    in.zone = BB_ZONE_CANCEL;
    T_COL(SL_OFF, sl_color(&in, 1000));
}

static void test_flash_timing(void)
{
    sl_in_t in = idle();
    in.flash_active = true; in.flash_color = SL_BLUE; in.flash_start_ms = 5000;
    static const struct { uint32_t e; int on; } c[] = {
        {0, 1}, {99, 1}, {100, 0}, {199, 0}, {200, 1}, {299, 1}, {300, 0}, {399, 0},
        {400, 1}, {499, 1}, {500, 0}, {599, 0}, {600, 0}, {601, 0}, {100000, 0}};
    for (unsigned i = 0; i < sizeof c / sizeof c[0]; i++) {
        sl_rgb_t got = sl_color(&in, 5000 + c[i].e);
        T_COL(c[i].on ? SL_BLUE : SL_OFF, got);
    }
    in.flash_color = SL_RED;
    T_COL(SL_RED, sl_color(&in, 5000));
    in.flash_active = false;
    T_COL(SL_OFF, sl_color(&in, 5000));
}

static void test_blink_timing(void)
{
    sl_in_t in = idle();
    in.ap = SL_AP_ON_DEMAND; in.ap_ref_ms = 7000;
    static const struct { uint32_t e; int on; } c[] = {
        {0, 1}, {499, 1}, {500, 0}, {999, 0}, {1000, 1}, {1499, 1}, {1500, 0}, {123000, 1}, {123500, 0}};
    for (unsigned i = 0; i < sizeof c / sizeof c[0]; i++) {
        T_COL(c[i].on ? SL_BLUE : SL_OFF, sl_color(&in, 7000 + c[i].e));
    }
}

static void test_wrap_flash(void)
{
    sl_in_t in = idle();
    in.flash_active = true; in.flash_color = SL_RED;
    in.flash_start_ms = UINT32_MAX - 49;       /* wraps 50 ms into the flash */
    T_COL(SL_RED, sl_color(&in, UINT32_MAX - 49));
    T_COL(SL_RED, sl_color(&in, UINT32_MAX));  /* e = 49 */
    T_COL(SL_RED, sl_color(&in, 0));           /* e = 50 */
    T_COL(SL_RED, sl_color(&in, 49));          /* e = 99 */
    T_COL(SL_OFF, sl_color(&in, 50));          /* e = 100 */
    T_COL(SL_RED, sl_color(&in, 150));         /* e = 200 */
    T_COL(SL_OFF, sl_color(&in, 549));         /* e = 599 */
    T_COL(SL_OFF, sl_color(&in, 550));         /* e = 600: over */
    T_COL(SL_OFF, sl_color(&in, 5000));
}

static void test_wrap_blink(void)
{
    sl_in_t in = idle();
    in.ap = SL_AP_ON_DEMAND;
    in.ap_ref_ms = UINT32_MAX - 99;            /* wraps 100 ms into the first on-phase */
    T_COL(SL_BLUE, sl_color(&in, UINT32_MAX));     /* e = 99 */
    T_COL(SL_BLUE, sl_color(&in, 0));              /* e = 100 */
    T_COL(SL_BLUE, sl_color(&in, 399));            /* e = 499 */
    T_COL(SL_OFF, sl_color(&in, 400));             /* e = 500 */
    T_COL(SL_OFF, sl_color(&in, 899));             /* e = 999 */
    T_COL(SL_BLUE, sl_color(&in, 900));            /* e = 1000 */
}

int main(void)
{
    TT_RUN(test_off_by_default);
    TT_RUN(test_held_zones);
    TT_RUN(test_priority);
    TT_RUN(test_flash_timing);
    TT_RUN(test_blink_timing);
    TT_RUN(test_wrap_flash);
    TT_RUN(test_wrap_blink);
    return TT_RESULT();
}
