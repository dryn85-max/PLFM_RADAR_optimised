/* Ports of the upstream "Gap 3 safety" test intents (9_1_Microcontroller/tests/)
 * to the G0B1 modules. The IWDG-refresh and cold-start timer intents that need
 * app_loop()/app_init() are in test_app.c.
 * Upstream IDQ periodic re-read and max-8-sensors do not apply (no Idq channels,
 * one sensor on this prototype). */
#include <errno.h>
#include <string.h>
#include "tinytest.h"
#include "mock_log.h"
#include "cmd.h"
#include "agc.h"
#include "config.h"
#include "fault.h"
#include "sequencer.h"
#include "thermal.h"
#include "hal_gpio.h"

static agc_t g_agc;

static void fresh(void)
{
    mock_reset();
    fault_test_power_cycle();
    fault_init();
    agc_init(&g_agc);
    cmd_init(&g_agc);
    thermal_init();
}

/* 1. Emergency stop cuts rails, in the documented order. */
static void test_gap3_estop_cuts_rails(void)
{
    static const gpio_t ORDER[] = { PIN_FPGA_DIG3, PIN_EN_PA, PIN_EN_LNA, PIN_EN_ADTR_VSS_SW,
                                    PIN_EN_ADTR_VDD_SW, PIN_EN_ADAR, PIN_EN_LO };
    int p, i;
    fresh();
    for (p = PIN_EN_FPGA; p <= PIN_FPGA_DIG4; p++) gpio_write((gpio_t)p, 1);
    mock_log_n = 0;
    sequencer_emergency_stop();
    for (p = PIN_EN_FPGA; p <= PIN_EN_PA; p++) TT_ASSERT_EQ(0, gpio_read((gpio_t)p));
    {
        int prev = -1, k;
        for (i = 0; i < (int)(sizeof ORDER / sizeof ORDER[0]); i++) {
            int idx = -1;
            for (k = 0; k < mock_log_n; k++)
                if (mock_log[k].a == (int)ORDER[i] && mock_log[k].b == 0) { idx = k; break; }
            TT_ASSERT(idx > prev);          /* rails drop in the documented order */
            prev = idx;
        }
    }
    TT_ASSERT_EQ(PIN_EN_FPGA, mock_log[mock_log_n - 1].a);   /* FPGA enable last */
}

/* 2. The emergency state is set (latched) with the stop; a reset does not re-energise. */
static void test_gap3_latch_set_and_survives_reset(void)
{
    char out[64];
    fresh();
    (void)cmd_exec("stop", out, sizeof out);
    TT_ASSERT_EQ(1, fault_is_latched());
    TT_ASSERT_EQ(-EPERM, sequencer_power_up());
    fault_test_simulate_reset();                     /* IWDG / NRST */
    TT_ASSERT_EQ(1, fault_is_latched());
    mock_log_n = 0;
    TT_ASSERT_EQ(-EPERM, sequencer_power_up());
    TT_ASSERT_EQ(-EPERM, sequencer_rf_up());
    TT_ASSERT_EQ(0, mock_log_n);
}

/* 3 (config half). IWDG window: prescaler * reload / 32 kHz LSI = 4.0 s. */
static void test_gap3_iwdg_window(void)
{
    TT_ASSERT_EQ(4000, IWDG_PRESCALER_DIV * IWDG_RELOAD * 1000 / 32000);
}

/* 4. Over-temperature (75.0 C) -> emergency stop, latched. */
static void test_gap3_overtemp_estops(void)
{
    uint8_t raw = 154;                               /* 752 deci-C */
    fresh();
    gpio_write(PIN_EN_PA, 1);
    gpio_write(PIN_EN_FPGA, 1);
    mock_time_advance_us(5000u * 1000u);
    mock_i2c_set_rx(&raw, 1);
    thermal_tick();
    TT_ASSERT_EQ(FAULT_OVERTEMP, fault_latched_code());
    TT_ASSERT_EQ(0, gpio_read(PIN_EN_PA));
    TT_ASSERT_EQ(0, gpio_read(PIN_EN_FPGA));
}

int main(void)
{
    TT_RUN(test_gap3_estop_cuts_rails);
    TT_RUN(test_gap3_latch_set_and_survives_reset);
    TT_RUN(test_gap3_iwdg_window);
    TT_RUN(test_gap3_overtemp_estops);
    return TT_RESULT();
}
