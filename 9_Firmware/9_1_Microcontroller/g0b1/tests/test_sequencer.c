/* Host tests for sequencer.c (safety: power-up/down and emergency-stop order).
 * Links the REAL fault.c and sequencer.c. */
#include <errno.h>
#include <stddef.h>
#include "tinytest.h"
#include "mock_log.h"
#include "sequencer.h"
#include "fault.h"
#include "hal_gpio.h"

static void fresh(void)
{
    mock_reset();
    fault_test_power_cycle();
    fault_init();
}

typedef struct { int kind, a, b; } ev_t;   /* kind: 0 write, 1 delay_ms */

/* Flatten the log into {GPIO write | DELAY_MS} events; any other kind is returned as -1 count. */
static int flatten(ev_t *out, int cap)
{
    int i, n = 0;
    for (i = 0; i < mock_log_n; i++) {
        if (n >= cap) return -2;
        if (mock_log[i].kind == MOCK_EV_GPIO_WRITE) {
            out[n].kind = 0; out[n].a = mock_log[i].a; out[n].b = mock_log[i].b; n++;
        } else if (mock_log[i].kind == MOCK_EV_DELAY_MS) {
            out[n].kind = 1; out[n].a = mock_log[i].a; out[n].b = 0; n++;
        } else {
            return -1;
        }
    }
    return n;
}

static int count_kind(int kind)
{
    int i, n = 0;
    for (i = 0; i < mock_log_n; i++) if (mock_log[i].kind == kind) n++;
    return n;
}

static int count_bus(void)
{
    return count_kind(MOCK_EV_SPI) + count_kind(MOCK_EV_I2C_WRITE) + count_kind(MOCK_EV_I2C_READ);
}

/* expected: pin, level, delay_ms (delay event only when > 0) */
typedef struct { gpio_t pin; int level; int delay; } exp_t;

static void check_sequence(const exp_t *e, int n)
{
    ev_t ev[64];
    int cnt = flatten(ev, 64), i, k = 0;
    int expected = 0;
    for (i = 0; i < n; i++) expected += 1 + (e[i].delay > 0);
    TT_ASSERT_EQ(expected, cnt);
    if (cnt != expected) return;
    for (i = 0; i < n; i++) {
        TT_ASSERT_EQ(0, ev[k].kind);
        TT_ASSERT_EQ(e[i].pin, ev[k].a);
        TT_ASSERT_EQ(e[i].level, ev[k].b);
        k++;
        if (e[i].delay > 0) {
            TT_ASSERT_EQ(1, ev[k].kind);
            TT_ASSERT_EQ(e[i].delay, ev[k].a);
            k++;
        }
    }
}

static void test_base_up_table(void)
{
    static const exp_t E[] = {
        { PIN_EN_FPGA, 1, 100 }, { PIN_EN_LO, 1, 10 }, { PIN_PLL_CS, 1, 0 },
        { PIN_EN_ADAR, 1, 500 },
        { PIN_ADAR_CS0, 1, 0 }, { PIN_ADAR_CS1, 1, 0 }, { PIN_ADAR_CS2, 1, 0 }, { PIN_ADAR_CS3, 1, 0 },
        { PIN_EN_ADTR_VDD_SW, 1, 1 }, { PIN_EN_ADTR_VSS_SW, 1, 1 },
    };
    fresh();
    TT_ASSERT_EQ(0, sequencer_power_up());
    check_sequence(E, 10);
    TT_ASSERT_EQ(10, SEQ_BASE_UP_N);
    TT_ASSERT_EQ(0, count_bus());
}

static void test_rf_up_table(void)
{
    static const exp_t E[] = { { PIN_EN_LNA, 1, 2 }, { PIN_EN_PA, 1, 50 } };
    fresh();
    TT_ASSERT_EQ(0, sequencer_rf_up());
    check_sequence(E, 2);
}

static void test_down_table(void)
{
    static const exp_t E[] = {
        { PIN_FPGA_DIG3, 0, 0 }, { PIN_EN_PA, 0, 10 }, { PIN_EN_LNA, 0, 10 },
        { PIN_EN_ADTR_VSS_SW, 0, 1 }, { PIN_EN_ADTR_VDD_SW, 0, 1 }, { PIN_EN_ADAR, 0, 10 },
        /* chip selects low only after the chip's supply is off (no back-powering) */
        { PIN_ADAR_CS0, 0, 0 }, { PIN_ADAR_CS1, 0, 0 }, { PIN_ADAR_CS2, 0, 0 }, { PIN_ADAR_CS3, 0, 0 },
        { PIN_EN_LO, 0, 10 }, { PIN_PLL_CE, 0, 0 }, { PIN_PLL_CS, 0, 0 },
        /* FPGA inputs low before EN_FPGA, as in the e-stop */
        { PIN_FPGA_DIG0, 0, 0 }, { PIN_FPGA_DIG1, 0, 0 }, { PIN_FPGA_DIG2, 0, 0 },
        { PIN_FPGA_DIG4, 0, 0 }, { PIN_EN_FPGA, 0, 0 },
    };
    fresh();
    sequencer_power_down();
    check_sequence(E, 18);
    TT_ASSERT_EQ(18, SEQ_DOWN_N);
}

static void test_estop_order_no_delay_no_bus(void)
{
    static const exp_t E[] = {
        { PIN_FPGA_DIG3, 0, 0 }, { PIN_EN_PA, 0, 0 }, { PIN_EN_LNA, 0, 0 },
        { PIN_EN_ADTR_VSS_SW, 0, 0 }, { PIN_EN_ADTR_VDD_SW, 0, 0 }, { PIN_EN_ADAR, 0, 0 },
        /* chip selects / CE low only AFTER the chip's supply is off: a high MCU
         * output into an unpowered ADAR1000 / PLL would back-power it through its
         * input protection (inputs must stay <= supply + 0.3 V), while a low level
         * is always within the abs. max. ratings and selecting an unpowered
         * device is harmless. */
        { PIN_ADAR_CS0, 0, 0 }, { PIN_ADAR_CS1, 0, 0 }, { PIN_ADAR_CS2, 0, 0 }, { PIN_ADAR_CS3, 0, 0 },
        { PIN_EN_LO, 0, 0 }, { PIN_PLL_CE, 0, 0 }, { PIN_PLL_CS, 0, 0 },
        /* FPGA inputs low BEFORE the FPGA loses power: no back-powering through MCU outputs */
        { PIN_FPGA_DIG0, 0, 0 }, { PIN_FPGA_DIG1, 0, 0 }, { PIN_FPGA_DIG2, 0, 0 },
        { PIN_FPGA_DIG4, 0, 0 }, { PIN_EN_FPGA, 0, 0 },
    };
    size_t i;
    fresh();
    sequencer_emergency_stop();
    check_sequence(E, 18);
    TT_ASSERT_EQ(0, count_bus());
    TT_ASSERT_EQ(0, count_kind(MOCK_EV_DELAY_US) + count_kind(MOCK_EV_DELAY_MS));
    TT_ASSERT_EQ(0, count_kind(MOCK_EV_UART_WRITE));
    for (i = 0; i < SEQ_ESTOP_N; i++) {
        TT_ASSERT_EQ(0, SEQ_ESTOP[i].delay_ms);
        TT_ASSERT_EQ(0, SEQ_ESTOP[i].level);
    }
    TT_ASSERT_EQ(18, SEQ_ESTOP_N);
}

static void test_estop_drops_pa_before_everything_but_mixers(void)
{
    /* every rail ends low, even when everything was high before */
    int p;
    fresh();
    for (p = PIN_EN_FPGA; p <= PIN_FPGA_DIG4; p++) gpio_write((gpio_t)p, 1);
    mock_log_n = 0;
    sequencer_emergency_stop();
    for (p = PIN_EN_FPGA; p <= PIN_EN_PA; p++) TT_ASSERT_EQ(0, gpio_read((gpio_t)p));
    for (p = PIN_FPGA_DIG0; p <= PIN_FPGA_DIG4; p++) TT_ASSERT_EQ(0, gpio_read((gpio_t)p));
}

static void test_estop_dig_low_before_fpga_enable(void)
{
    int i, fpga_idx = -1, dig_low_seen = 0;
    fresh();
    sequencer_emergency_stop();
    TT_ASSERT_EQ(PIN_FPGA_DIG3, mock_log[0].a);   /* DIG3 stays first */
    for (i = 0; i < mock_log_n; i++) {
        if (mock_log[i].a == PIN_EN_FPGA) fpga_idx = i;
    }
    TT_ASSERT(fpga_idx > 0);
    for (i = 0; i < fpga_idx; i++) {
        int pin = mock_log[i].a;
        if (pin >= PIN_FPGA_DIG0 && pin <= PIN_FPGA_DIG4) {
            TT_ASSERT_EQ(0, mock_log[i].b);
            dig_low_seen |= 1 << (pin - PIN_FPGA_DIG0);
        }
    }
    TT_ASSERT_EQ(0x1F, dig_low_seen);
    TT_ASSERT_EQ(fpga_idx, mock_log_n - 1);       /* FPGA enable is the last step */
}

/* No chip select / CE is ever driven high by the e-stop or the orderly shutdown,
 * and each goes low only after its chip's supply enable has gone low. */
static void test_cs_low_only_after_supply_off(void)
{
    int pass, i, adar_off, lo_off;
    for (pass = 0; pass < 2; pass++) {
        fresh();
        if (pass == 0) sequencer_emergency_stop(); else sequencer_power_down();
        adar_off = lo_off = -1;
        for (i = 0; i < mock_log_n; i++) {
            const mock_event_t *e = &mock_log[i];
            if (e->kind != MOCK_EV_GPIO_WRITE) continue;
            if (e->a == PIN_EN_ADAR && e->b == 0) adar_off = i;
            if (e->a == PIN_EN_LO && e->b == 0) lo_off = i;
            if (e->a >= PIN_ADAR_CS0 && e->a <= PIN_ADAR_CS3) {
                TT_ASSERT_EQ(0, e->b);
                TT_ASSERT(adar_off >= 0);
            }
            if (e->a == PIN_PLL_CS || e->a == PIN_PLL_CE) {
                TT_ASSERT_EQ(0, e->b);
                TT_ASSERT(lo_off >= 0);
            }
        }
    }
}

/* M2: orderly shutdown also drives DIG0-2 low before the FPGA loses power. */
static void test_down_dig_low_before_fpga_enable(void)
{
    int i, fpga_idx = -1, seen = 0;
    fresh();
    sequencer_power_down();
    for (i = 0; i < mock_log_n; i++) if (mock_log[i].kind == MOCK_EV_GPIO_WRITE && mock_log[i].a == PIN_EN_FPGA) fpga_idx = i;
    TT_ASSERT(fpga_idx > 0);
    for (i = 0; i < fpga_idx; i++) {
        const mock_event_t *e = &mock_log[i];
        if (e->kind == MOCK_EV_GPIO_WRITE && e->a >= PIN_FPGA_DIG0 && e->a <= PIN_FPGA_DIG4) {
            TT_ASSERT_EQ(0, e->b);
            seen |= 1 << (e->a - PIN_FPGA_DIG0);
        }
    }
    TT_ASSERT_EQ(0x1F, seen);
}

static void test_estop_idempotent(void)
{
    fresh();
    sequencer_emergency_stop();
    sequencer_emergency_stop();
    TT_ASSERT_EQ(36, mock_log_n);
    TT_ASSERT_EQ(0, count_bus());
}

static void test_latched_refuses_power_up(void)
{
    fresh();
    fault_raise(FAULT_OVERTEMP);
    mock_log_n = 0;
    TT_ASSERT_EQ(-EPERM, sequencer_power_up());
    TT_ASSERT_EQ(-EPERM, sequencer_rf_up());
    TT_ASSERT_EQ(0, mock_log_n);   /* nothing written, no delay */
    /* still refused after a simulated watchdog reset */
    fault_test_simulate_reset();
    TT_ASSERT_EQ(-EPERM, sequencer_power_up());
    TT_ASSERT_EQ(0, mock_log_n);
    /* a power cycle re-allows it */
    fault_test_power_cycle();
    fault_init();
    TT_ASSERT_EQ(0, sequencer_power_up());
}

static void test_nonlatched_fault_does_not_block_up(void)
{
    fresh();
    fault_raise(FAULT_PLL_LOCK);
    mock_log_n = 0;
    TT_ASSERT_EQ(0, sequencer_power_up());
    TT_ASSERT(mock_log_n > 0);
}

static void test_rf_off(void)
{
    ev_t ev[16];
    int n, i;
    fresh();
    gpio_write(PIN_FPGA_DIG3, 1); gpio_write(PIN_EN_PA, 1); gpio_write(PIN_EN_LNA, 1);
    gpio_write(PIN_EN_LO, 1);
    mock_log_n = 0;
    sequencer_rf_off();
    n = flatten(ev, 16);
    TT_ASSERT_EQ(3, n);   /* no delay events */
    TT_ASSERT_EQ(PIN_FPGA_DIG3, ev[0].a);
    TT_ASSERT_EQ(PIN_EN_PA, ev[1].a);
    TT_ASSERT_EQ(PIN_EN_LNA, ev[2].a);
    for (i = 0; i < 3; i++) { TT_ASSERT_EQ(0, ev[i].kind); TT_ASSERT_EQ(0, ev[i].b); }
    TT_ASSERT_EQ(1, gpio_read(PIN_EN_LO));   /* base rails stay */
    TT_ASSERT_EQ(0, count_bus());
}

static void test_fault_integration(void)
{
    /* real fault.c -> real sequencer: over-temp runs the actual e-stop */
    fresh();
    gpio_write(PIN_EN_PA, 1); gpio_write(PIN_EN_FPGA, 1);
    mock_log_n = 0;
    fault_raise(FAULT_OVERTEMP);
    TT_ASSERT_EQ(0, gpio_read(PIN_EN_PA));
    TT_ASSERT_EQ(0, gpio_read(PIN_EN_FPGA));
    TT_ASSERT_EQ(18, mock_log_n);
    /* PLL lock loss: only RF off */
    fresh();
    gpio_write(PIN_EN_FPGA, 1); gpio_write(PIN_EN_PA, 1);
    fault_raise(FAULT_PLL_LOCK);
    TT_ASSERT_EQ(0, gpio_read(PIN_EN_PA));
    TT_ASSERT_EQ(1, gpio_read(PIN_EN_FPGA));
    /* panic path */
    fresh();
    gpio_write(PIN_EN_PA, 1);
    fault_panic();
    TT_ASSERT_EQ(0, gpio_read(PIN_EN_PA));
}

static void test_run_custom_and_null(void)
{
    static const seq_step_t S[] = { { PIN_EN_LO, 1, 3 }, { PIN_EN_LO, 0, 0 } };
    fresh();
    sequencer_run(S, 2);
    TT_ASSERT_EQ(3, mock_log_n);
    sequencer_run(NULL, 5);   /* must not crash */
    sequencer_run(S, 0);
    TT_ASSERT_EQ(3, mock_log_n);
}

int main(void)
{
    TT_RUN(test_base_up_table);
    TT_RUN(test_rf_up_table);
    TT_RUN(test_down_table);
    TT_RUN(test_estop_order_no_delay_no_bus);
    TT_RUN(test_estop_drops_pa_before_everything_but_mixers);
    TT_RUN(test_estop_dig_low_before_fpga_enable);
    TT_RUN(test_cs_low_only_after_supply_off);
    TT_RUN(test_down_dig_low_before_fpga_enable);
    TT_RUN(test_estop_idempotent);
    TT_RUN(test_latched_refuses_power_up);
    TT_RUN(test_nonlatched_fault_does_not_block_up);
    TT_RUN(test_rf_off);
    TT_RUN(test_fault_integration);
    TT_RUN(test_run_custom_and_null);
    return TT_RESULT();
}
