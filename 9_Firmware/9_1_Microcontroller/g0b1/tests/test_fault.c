#include <string.h>
#include "tinytest.h"
#include "mock_log.h"
#include "fault.h"
#include "config.h"
#include "diag_log.h"

/* Spies for the sequencer entry points used by fault.c (replace the
 * real sequencer.c, which tests/Makefile leaves out of this test). */
static int g_estop_calls, g_rfoff_calls, g_latched_at_estop;
void sequencer_emergency_stop(void) { g_estop_calls++; g_latched_at_estop = fault_is_latched(); }
void sequencer_rf_off(void)         { g_rfoff_calls++; }

static void fresh(void)
{
    mock_reset();
    g_estop_calls = g_rfoff_calls = g_latched_at_estop = 0;
    fault_test_power_cycle();   /* wipes the latch like a power cycle */
    fault_init();
}

static void test_clean_boot(void)
{
    fresh();
    TT_ASSERT_EQ(0, fault_is_latched());
    TT_ASSERT_EQ(FAULT_NONE, fault_latched_code());
    TT_ASSERT_EQ(FAULT_NONE, fault_active());
}

static void test_latch_survives_reset(void)
{
    fresh();
    fault_raise(FAULT_OVERTEMP);
    TT_ASSERT_EQ(1, g_estop_calls);
    TT_ASSERT_EQ(0, g_rfoff_calls);
    TT_ASSERT_EQ(1, fault_is_latched());
    fault_test_simulate_reset();
    TT_ASSERT_EQ(1, fault_is_latched());
    TT_ASSERT_EQ(FAULT_OVERTEMP, fault_latched_code());
    TT_ASSERT_EQ(FAULT_OVERTEMP, fault_active());
    fault_test_simulate_reset();   /* and again: still there */
    TT_ASSERT_EQ(FAULT_OVERTEMP, fault_latched_code());
}

static void test_power_cycle_clears(void)
{
    fresh();
    fault_raise(FAULT_ESTOP_CMD);
    fault_test_power_cycle();
    fault_init();
    TT_ASSERT_EQ(0, fault_is_latched());
}

static void test_corrupted_ncode(void)
{
    fresh();
    fault_raise(FAULT_OVERTEMP);
    fault_test_corrupt(0, 0, 1u);   /* flip a bit in ~code */
    fault_test_simulate_reset();
    TT_ASSERT_EQ(0, fault_is_latched());
    TT_ASSERT_EQ(FAULT_NONE, fault_active());
}

static void test_corrupted_magic(void)
{
    fresh();
    fault_raise(FAULT_OVERTEMP);
    fault_test_corrupt(1u, 0, 0);
    fault_test_simulate_reset();
    TT_ASSERT_EQ(0, fault_is_latched());
}

static void test_corrupted_code(void)
{
    fresh();
    fault_raise(FAULT_OVERTEMP);
    fault_test_corrupt(0, 1u, 0);
    fault_test_simulate_reset();
    TT_ASSERT_EQ(0, fault_is_latched());
}

static void test_garbage_ram(void)
{
    /* random power-up RAM: right magic, consistent ~code but a non-latched code */
    fresh();
    fault_test_set_raw(FAULT_MAGIC, FAULT_PLL_LOCK, ~(uint32_t)FAULT_PLL_LOCK);
    fault_test_simulate_reset();
    TT_ASSERT_EQ(0, fault_is_latched());
    fault_test_set_raw(FAULT_MAGIC, 0xFFFFFFFFu, 0u);
    fault_test_simulate_reset();
    TT_ASSERT_EQ(0, fault_is_latched());
    fault_test_set_raw(0u, 0u, 0xFFFFFFFFu);
    fault_test_simulate_reset();
    TT_ASSERT_EQ(0, fault_is_latched());
}

static void test_invalid_latch_is_wiped(void)
{
    /* after a rejected latch the RAM must not "come back" via a half-valid state */
    fresh();
    fault_raise(FAULT_OVERTEMP);
    fault_test_corrupt(0, 0, 1u);
    fault_test_simulate_reset();
    fault_test_corrupt(0, 0, 1u);   /* flip the same bit again: would be valid if not wiped */
    fault_test_simulate_reset();
    TT_ASSERT_EQ(0, fault_is_latched());
}

static void test_nonlatched(void)
{
    fresh();
    fault_raise(FAULT_PLL_LOCK);
    TT_ASSERT_EQ(0, fault_is_latched());
    TT_ASSERT_EQ(1, g_rfoff_calls);
    TT_ASSERT_EQ(0, g_estop_calls);
    TT_ASSERT_EQ(FAULT_PLL_LOCK, fault_active());
    fault_test_simulate_reset();   /* not retained across reset */
    TT_ASSERT_EQ(FAULT_NONE, fault_active());
    fault_raise(FAULT_ADAR_COMM);
    fault_clear_nonlatched();
    TT_ASSERT_EQ(FAULT_NONE, fault_active());
}

static void test_latched_wins(void)
{
    fresh();
    fault_raise(FAULT_PLL_LOCK);
    fault_raise(FAULT_OVERTEMP);
    TT_ASSERT_EQ(FAULT_OVERTEMP, fault_active());
    fault_raise(FAULT_ADAR_COMM);
    TT_ASSERT_EQ(FAULT_OVERTEMP, fault_active());
    fault_clear_nonlatched();           /* must not clear the latch */
    TT_ASSERT_EQ(FAULT_OVERTEMP, fault_active());
    TT_ASSERT_EQ(1, fault_is_latched());
}

static void test_first_latch_kept(void)
{
    fresh();
    fault_raise(FAULT_ESTOP_CMD);
    fault_raise(FAULT_OVERTEMP);
    TT_ASSERT_EQ(FAULT_ESTOP_CMD, fault_latched_code());
    TT_ASSERT_EQ(2, g_estop_calls);   /* e-stop is re-applied, harmless */
}

/* M1: the latch is stored BEFORE the e-stop runs (a hang/reset inside the e-stop
 * must still boot into the latched state). */
static void test_latch_stored_before_estop(void)
{
    fresh();
    fault_raise(FAULT_OVERTEMP);
    TT_ASSERT_EQ(1, g_latched_at_estop);
    fresh();
    fault_panic();
    TT_ASSERT_EQ(1, g_latched_at_estop);
}

/* M1: fault_panic must not overwrite an earlier latched code (the first
 * cause is the useful one). */
static void test_panic_keeps_first_code(void)
{
    fresh();
    fault_raise(FAULT_OVERTEMP);
    fault_panic();
    TT_ASSERT_EQ(FAULT_OVERTEMP, fault_latched_code());
    fault_test_simulate_reset();
    TT_ASSERT_EQ(FAULT_OVERTEMP, fault_latched_code());
    TT_ASSERT_EQ(2, g_estop_calls);
}

/* C: watchdog reset at boot with no valid latch -> latched FAULT_WATCHDOG. */
static void test_on_boot_watchdog_latches(void)
{
    static const uint32_t FLAGS[] = { FAULT_RST_IWDG, FAULT_RST_WWDG, FAULT_RST_IWDG | FAULT_RST_WWDG,
                                      FAULT_RST_IWDG | FAULT_RST_PIN | FAULT_RST_SFT };
    unsigned i;
    TT_ASSERT(FAULT_WATCHDOG >= 10);
    for (i = 0; i < sizeof FLAGS / sizeof FLAGS[0]; i++) {
        fresh();
        TT_ASSERT_EQ(FAULT_WATCHDOG, fault_on_boot(FLAGS[i]));
        TT_ASSERT_EQ(1, fault_is_latched());
        TT_ASSERT_EQ(FAULT_WATCHDOG, fault_latched_code());
        fault_test_simulate_reset();                    /* survives the next reset */
        TT_ASSERT_EQ(FAULT_WATCHDOG, fault_latched_code());
    }
}

static void test_on_boot_other_resets_do_not_latch(void)
{
    static const uint32_t FLAGS[] = { 0u, FAULT_RST_PIN, FAULT_RST_PWR, FAULT_RST_SFT,
                                      FAULT_RST_PIN | FAULT_RST_PWR | FAULT_RST_SFT,
                                      0x1E000000u /* OBL, PIN, PWR, SFT */ };
    unsigned i;
    for (i = 0; i < sizeof FLAGS / sizeof FLAGS[0]; i++) {
        fresh();
        TT_ASSERT_EQ(FAULT_NONE, fault_on_boot(FLAGS[i]));
        TT_ASSERT_EQ(0, fault_is_latched());
    }
}

/* An existing valid latch is kept: the first cause wins. */
static void test_on_boot_keeps_existing_latch(void)
{
    fresh();
    fault_raise(FAULT_OVERTEMP);
    fault_test_simulate_reset();                        /* IWDG reset after the e-stop */
    TT_ASSERT_EQ(FAULT_NONE, fault_on_boot(FAULT_RST_IWDG));
    TT_ASSERT_EQ(FAULT_OVERTEMP, fault_latched_code());
    /* a corrupted latch counts as "no valid latch" */
    fresh();
    fault_raise(FAULT_OVERTEMP);
    fault_test_corrupt(0, 1u, 0);
    fault_test_simulate_reset();
    TT_ASSERT_EQ(FAULT_WATCHDOG, fault_on_boot(FAULT_RST_IWDG));
    TT_ASSERT_EQ(FAULT_WATCHDOG, fault_latched_code());
}

static void test_on_boot_is_gpio_free(void)
{
    fresh();
    mock_log_n = 0;
    (void)fault_on_boot(FAULT_RST_IWDG);
    TT_ASSERT_EQ(0, g_estop_calls);                     /* app_init runs the e-stop for a latched boot */
    TT_ASSERT_EQ(0, mock_log_n);
}

static void test_raise_none_ignored(void)
{
    fresh();
    fault_raise(FAULT_NONE);
    TT_ASSERT_EQ(0, g_estop_calls + g_rfoff_calls);
    TT_ASSERT_EQ(FAULT_NONE, fault_active());
}

static void test_panic(void)
{
    fresh();
    fault_panic();   /* host build returns instead of spinning */
    TT_ASSERT_EQ(1, g_estop_calls);
    TT_ASSERT_EQ(1, fault_is_latched());
    TT_ASSERT_EQ(FAULT_PANIC, fault_latched_code());
    fault_test_simulate_reset();
    TT_ASSERT_EQ(FAULT_PANIC, fault_latched_code());
}

static void test_panic_does_not_use_spi(void)
{
    /* GPIO-only: a hung bus must not matter. fault.c itself never touches SPI/I2C. */
    int i, spi = 0;
    fresh();
    fault_panic();
    for (i = 0; i < mock_log_n; i++)
        if (mock_log[i].kind == MOCK_EV_SPI || mock_log[i].kind == MOCK_EV_I2C_WRITE ||
            mock_log[i].kind == MOCK_EV_I2C_READ)
            spi++;
    TT_ASSERT_EQ(0, spi);
}

static void test_config(void)
{
    TT_ASSERT(ADAR_COUNT == 1 || ADAR_COUNT == 4);
    TT_ASSERT_EQ(750, OVERTEMP_DECI_C);
    TT_ASSERT_EQ(5000, THERMAL_PERIOD_MS);
    TT_ASSERT_EQ(250, AGC_PERIOD_MS);
    TT_ASSERT_EQ(256, IWDG_PRESCALER_DIV);
    TT_ASSERT_EQ(500, IWDG_RELOAD);
    TT_ASSERT_EQ(100, PLL_LOCK_TIMEOUT_MS);
    TT_ASSERT_EQ(20000000, SPI_ADAR_MAX_HZ);
    TT_ASSERT_EQ(10000000, SPI_PLL_MAX_HZ);
}

static void test_diag_compiles(void)
{
    /* every macro must be usable as a statement (also with -DDIAG_DISABLE) */
    uint32_t t0 = 0;
    (void)t0;
    DIAG("T", "x=%d", 1);
    DIAG_WARN("T", "w");
    DIAG_ERR("T", "e %d", 2);
    DIAG_REG("T", "r", 0x12);
    DIAG_REG32("T", "r32", 0x12345678u);
    DIAG_GPIO("T", "led", PIN_LED);
    DIAG_BOOL("T", "b", 1);
    DIAG_SECTION("sec");
    DIAG_ELAPSED("T", "op", t0);
    if (1) DIAG("T", "dangling-else safe");
    else DIAG("T", "never");
}

int main(void)
{
    TT_RUN(test_clean_boot);
    TT_RUN(test_latch_survives_reset);
    TT_RUN(test_power_cycle_clears);
    TT_RUN(test_corrupted_ncode);
    TT_RUN(test_corrupted_magic);
    TT_RUN(test_corrupted_code);
    TT_RUN(test_garbage_ram);
    TT_RUN(test_invalid_latch_is_wiped);
    TT_RUN(test_nonlatched);
    TT_RUN(test_latched_wins);
    TT_RUN(test_first_latch_kept);
    TT_RUN(test_latch_stored_before_estop);
    TT_RUN(test_panic_keeps_first_code);
    TT_RUN(test_on_boot_watchdog_latches);
    TT_RUN(test_on_boot_other_resets_do_not_latch);
    TT_RUN(test_on_boot_keeps_existing_latch);
    TT_RUN(test_on_boot_is_gpio_free);
    TT_RUN(test_raise_none_ignored);
    TT_RUN(test_panic);
    TT_RUN(test_panic_does_not_use_spi);
    TT_RUN(test_config);
    TT_RUN(test_diag_compiles);
    return TT_RESULT();
}
