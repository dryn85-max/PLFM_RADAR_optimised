/* Host tests for boot_btn: hold zones and the action on release. */
#include <stdint.h>
#include "tinytest.h"
#include "boot_btn.h"

/* Press at t=0 and hold for exactly ms in one step, then release; returns the action. */
static bb_act_t press_release(bb_t *b, uint32_t ms)
{
    bootbtn_step(b, true, 50);         /* press edge: starts at 0 */
    if (ms) bootbtn_step(b, true, ms); /* held ms */
    return bootbtn_step(b, false, 50);
}

static void test_zone_boundaries(void)
{
    TT_ASSERT_EQ(BB_ZONE_NONE, bootbtn_zone(0));
    TT_ASSERT_EQ(BB_ZONE_NONE, bootbtn_zone(1999));
    TT_ASSERT_EQ(BB_ZONE_AP, bootbtn_zone(2000));
    TT_ASSERT_EQ(BB_ZONE_AP, bootbtn_zone(4999));
    TT_ASSERT_EQ(BB_ZONE_RESET, bootbtn_zone(5000));
    TT_ASSERT_EQ(BB_ZONE_RESET, bootbtn_zone(9999));
    TT_ASSERT_EQ(BB_ZONE_CANCEL, bootbtn_zone(10000));
    TT_ASSERT_EQ(BB_ZONE_CANCEL, bootbtn_zone(UINT32_MAX));
}

static void test_release_boundaries(void)
{
    bb_t b; bootbtn_init(&b, false);
    TT_ASSERT_EQ(BB_ACT_NONE, press_release(&b, 0));
    TT_ASSERT_EQ(BB_ACT_NONE, press_release(&b, 1999));
    TT_ASSERT_EQ(BB_ACT_AP, press_release(&b, 2000));
    TT_ASSERT_EQ(BB_ACT_AP, press_release(&b, 4999));
    TT_ASSERT_EQ(BB_ACT_RESET, press_release(&b, 5000));
    TT_ASSERT_EQ(BB_ACT_RESET, press_release(&b, 9999));
    TT_ASSERT_EQ(BB_ACT_NONE, press_release(&b, 10000));
    TT_ASSERT_EQ(BB_ACT_NONE, press_release(&b, 60000));
}

static void test_held_zone_progress(void)
{
    bb_t b; bootbtn_init(&b, false);
    TT_ASSERT(!bootbtn_is_held(&b));
    TT_ASSERT_EQ(BB_ZONE_NONE, bootbtn_held_zone(&b));
    bootbtn_step(&b, true, 50);
    TT_ASSERT(bootbtn_is_held(&b));
    TT_ASSERT_EQ(BB_ZONE_NONE, bootbtn_held_zone(&b));
    bootbtn_step(&b, true, 1999);
    TT_ASSERT_EQ(BB_ZONE_NONE, bootbtn_held_zone(&b));
    bootbtn_step(&b, true, 1);
    TT_ASSERT_EQ(BB_ZONE_AP, bootbtn_held_zone(&b));
    bootbtn_step(&b, true, 2999);
    TT_ASSERT_EQ(BB_ZONE_AP, bootbtn_held_zone(&b));
    bootbtn_step(&b, true, 1);
    TT_ASSERT_EQ(BB_ZONE_RESET, bootbtn_held_zone(&b));
    bootbtn_step(&b, true, 4999);
    TT_ASSERT_EQ(BB_ZONE_RESET, bootbtn_held_zone(&b));
    bootbtn_step(&b, true, 1);
    TT_ASSERT_EQ(BB_ZONE_CANCEL, bootbtn_held_zone(&b));
    TT_ASSERT_EQ(BB_ACT_NONE, bootbtn_step(&b, false, 50));
    TT_ASSERT(!bootbtn_is_held(&b));
    TT_ASSERT_EQ(BB_ZONE_NONE, bootbtn_held_zone(&b));
}

static void test_many_small_steps(void)
{
    bb_t b; bootbtn_init(&b, false);
    bootbtn_step(&b, true, 50);
    for (int i = 0; i < 39; i++) bootbtn_step(&b, true, 50); /* 1950 */
    TT_ASSERT_EQ(BB_ZONE_NONE, bootbtn_held_zone(&b));
    bootbtn_step(&b, true, 50);                              /* 2000 */
    TT_ASSERT_EQ(BB_ZONE_AP, bootbtn_held_zone(&b));
    TT_ASSERT_EQ(BB_ACT_AP, bootbtn_step(&b, false, 50));
    /* 1 ms steps up to 4999 / 5000 */
    bootbtn_step(&b, true, 1);
    for (int i = 0; i < 4999; i++) bootbtn_step(&b, true, 1);
    TT_ASSERT_EQ(BB_ZONE_AP, bootbtn_held_zone(&b));
    bootbtn_step(&b, true, 1);
    TT_ASSERT_EQ(BB_ZONE_RESET, bootbtn_held_zone(&b));
    TT_ASSERT_EQ(BB_ACT_RESET, bootbtn_step(&b, false, 50));
}

static void test_repeated_presses(void)
{
    bb_t b; bootbtn_init(&b, false);
    for (int i = 0; i < 3; i++) {
        TT_ASSERT_EQ(BB_ACT_AP, press_release(&b, 3000));
        TT_ASSERT_EQ(BB_ACT_NONE, bootbtn_step(&b, false, 50)); /* idle: nothing */
        TT_ASSERT_EQ(BB_ACT_NONE, press_release(&b, 100));  /* short press: timer restarted */
        TT_ASSERT_EQ(BB_ACT_RESET, press_release(&b, 6000));
    }
    /* a release with a big dt must not add to anything */
    bootbtn_step(&b, true, 50);
    bootbtn_step(&b, true, 1000);
    TT_ASSERT_EQ(BB_ACT_NONE, bootbtn_step(&b, false, 100000));
}

static void test_held_at_boot(void)
{
    bb_t b; bootbtn_init(&b, true);
    TT_ASSERT(!bootbtn_is_held(&b));
    TT_ASSERT_EQ(BB_ACT_NONE, bootbtn_step(&b, true, 50));
    TT_ASSERT_EQ(BB_ACT_NONE, bootbtn_step(&b, true, 6000)); /* long hold in RESET range */
    TT_ASSERT(!bootbtn_is_held(&b));
    TT_ASSERT_EQ(BB_ZONE_NONE, bootbtn_held_zone(&b));
    TT_ASSERT_EQ(BB_ACT_NONE, bootbtn_step(&b, false, 50));  /* first release: no action */
    TT_ASSERT_EQ(BB_ACT_AP, press_release(&b, 3000));   /* then a real press works */
    /* released already at the first sample: ignore flag clears, next press is real */
    bootbtn_init(&b, true);
    TT_ASSERT_EQ(BB_ACT_NONE, bootbtn_step(&b, false, 50));
    TT_ASSERT_EQ(BB_ACT_RESET, press_release(&b, 5000));
    /* held at boot, released, pressed again long: release after boot-press does not leak */
    bootbtn_init(&b, true);
    bootbtn_step(&b, true, 3000);
    bootbtn_step(&b, false, 50);
    bootbtn_step(&b, true, 50);
    TT_ASSERT(bootbtn_is_held(&b));
    TT_ASSERT_EQ(BB_ZONE_NONE, bootbtn_held_zone(&b)); /* timing restarted at 0 */
}

static void test_saturation(void)
{
    bb_t b; bootbtn_init(&b, false);
    bootbtn_step(&b, true, 50);
    bootbtn_step(&b, true, UINT32_MAX - 10);
    bootbtn_step(&b, true, 100);
    TT_ASSERT_EQ(UINT32_MAX, b.held_ms);
    TT_ASSERT_EQ(BB_ZONE_CANCEL, bootbtn_held_zone(&b));
    bootbtn_step(&b, true, UINT32_MAX);
    TT_ASSERT_EQ(BB_ZONE_CANCEL, bootbtn_held_zone(&b));
    TT_ASSERT_EQ(BB_ACT_NONE, bootbtn_step(&b, false, 50));
    /* many large steps do not wrap back into the AP zone */
    bootbtn_step(&b, true, 50);
    for (int i = 0; i < 10; i++) bootbtn_step(&b, true, 0x80000000u);
    TT_ASSERT_EQ(BB_ZONE_CANCEL, bootbtn_held_zone(&b));
    TT_ASSERT_EQ(BB_ACT_NONE, bootbtn_step(&b, false, 50));
}

int main(void)
{
    TT_RUN(test_zone_boundaries);
    TT_RUN(test_release_boundaries);
    TT_RUN(test_held_zone_progress);
    TT_RUN(test_many_small_steps);
    TT_RUN(test_repeated_presses);
    TT_RUN(test_held_at_boot);
    TT_RUN(test_saturation);
    return TT_RESULT();
}
