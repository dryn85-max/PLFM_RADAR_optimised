/* Host tests for thermal.c: ADS7830 ch0 + TMP37 (20 mV/degC, no offset) with the
 * ADS7830 internal 2.5 V reference. */
#include <errno.h>
#include "tinytest.h"
#include "mock_log.h"
#include "thermal.h"
#include "fault.h"
#include "config.h"
#include "hal_time.h"

static int g_estop, g_rfoff;
void sequencer_emergency_stop(void) { g_estop++; }
void sequencer_rf_off(void)         { g_rfoff++; }

static int count_kind(int kind)
{
    int i, n = 0;
    for (i = 0; i < mock_log_n; i++) if (mock_log[i].kind == kind) n++;
    return n;
}

static void fresh(void)
{
    mock_reset();
    g_estop = g_rfoff = 0;
    fault_test_power_cycle();
    fault_init();
    thermal_init();
}

static void queue_raw(uint8_t raw) { mock_i2c_set_rx(&raw, 1); }

static void test_conversion_table(void)
{
    static const struct { uint8_t raw; int16_t deci; } T[] = {
        /* LSB = Vref/256 (ADS7830 DS: 2.5 V / 256 = 9.766 mV): mv = raw*2500/256,
         * deci = (mv*10+10)/20, all integer. */
        { 0, 0 }, { 1, 5 }, { 76, 371 }, { 100, 488 }, { 153, 747 }, { 152, 742 },
        { 154, 752 }, { 254, 1240 }, { 255, 1245 },
    };
    unsigned i;
    for (i = 0; i < sizeof T / sizeof T[0]; i++) {
        int16_t d = -1;
        mock_reset();
        queue_raw(T[i].raw);
        TT_ASSERT_EQ(0, thermal_read_deci_c(&d));
        TT_ASSERT_EQ(T[i].deci, d);
    }
}

static void test_conversion_uses_addr48_ch0_internal_ref(void)
{
    int16_t d;
    mock_reset();
    queue_raw(10);
    TT_ASSERT_EQ(0, thermal_read_deci_c(&d));
    TT_ASSERT_EQ(MOCK_EV_I2C_WRITE, mock_log[0].kind);
    TT_ASSERT_EQ(0x48, mock_log[0].a);
    TT_ASSERT_EQ(0x8C, mock_log[0].bytes[0]);   /* SD=1, CH0, PD=11 (ref on, ADC on) */
}

static void test_read_errors(void)
{
    int16_t d = 1234;
    mock_reset();
    mock_i2c_fail_next(-EIO);
    TT_ASSERT_EQ(-EIO, thermal_read_deci_c(&d));
    TT_ASSERT_EQ(1234, d);                      /* output untouched on error */
    TT_ASSERT_EQ(-EINVAL, thermal_read_deci_c(0));
}

static void test_not_before_5s_after_boot(void)
{
    fresh();
    queue_raw(76);
    thermal_tick();                              /* t = 0 */
    TT_ASSERT_EQ(0, count_kind(MOCK_EV_I2C_WRITE));
    mock_time_advance_us(4999u * 1000u);
    thermal_tick();                              /* t = 4999 ms */
    TT_ASSERT_EQ(0, count_kind(MOCK_EV_I2C_WRITE));
    mock_time_advance_us(1000u);
    thermal_tick();                              /* t = 5000 ms */
    TT_ASSERT_EQ(1, count_kind(MOCK_EV_I2C_WRITE));
    TT_ASSERT_EQ(371, thermal_last());
    TT_ASSERT_EQ(0, thermal_last_err());
    TT_ASSERT_EQ(0, g_estop);
}

static void test_period_repeats(void)
{
    fresh();
    mock_time_advance_us(5000u * 1000u);
    thermal_tick();
    thermal_tick();                              /* same instant: no second read */
    TT_ASSERT_EQ(1, count_kind(MOCK_EV_I2C_WRITE));
    /* the read itself consumed 1 ms of mock time (conversion delay); the period
     * is start-to-start, so the next read is due at t = 10000 ms */
    mock_time_advance_us(4998u * 1000u);          /* t = 9999 ms */
    thermal_tick();
    TT_ASSERT_EQ(1, count_kind(MOCK_EV_I2C_WRITE));
    mock_time_advance_us(1000u);                  /* t = 10000 ms */
    thermal_tick();
    TT_ASSERT_EQ(2, count_kind(MOCK_EV_I2C_WRITE));
}

static void test_first_tick_late_boot(void)
{
    /* init at t=3 s: first read is due at t=8 s, not 5 s */
    mock_reset();
    fault_test_power_cycle();
    fault_init();
    mock_time_advance_us(3000u * 1000u);
    thermal_init();
    mock_time_advance_us(4999u * 1000u);
    thermal_tick();
    TT_ASSERT_EQ(0, count_kind(MOCK_EV_I2C_WRITE));
    mock_time_advance_us(1000u);
    thermal_tick();
    TT_ASSERT_EQ(1, count_kind(MOCK_EV_I2C_WRITE));
}

static void test_overtemp_raises_latched_fault(void)
{
    fresh();
    /* No raw count maps to exactly 750 (raw 153.6): 153 -> 747 is the last value
     * below the threshold, 154 -> 752 the first at/above it. */
    queue_raw(153);                              /* 74.7 C: below threshold */
    mock_time_advance_us(5000u * 1000u);
    thermal_tick();
    TT_ASSERT_EQ(747, thermal_last());
    TT_ASSERT_EQ(0, fault_is_latched());
    queue_raw(154);                              /* 75.2 C: first count over threshold */
    mock_time_advance_us(5000u * 1000u);
    thermal_tick();
    TT_ASSERT_EQ(752, thermal_last());
    TT_ASSERT_EQ(1, fault_is_latched());
    TT_ASSERT_EQ(FAULT_OVERTEMP, fault_latched_code());
    TT_ASSERT_EQ(1, g_estop);
}

static void test_sensor_error_is_not_a_fault(void)
{
    fresh();
    queue_raw(76);
    mock_time_advance_us(5000u * 1000u);
    thermal_tick();
    TT_ASSERT_EQ(371, thermal_last());
    mock_time_advance_us(5000u * 1000u);
    mock_i2c_fail_next(-ETIMEDOUT);
    thermal_tick();
    TT_ASSERT_EQ(-ETIMEDOUT, thermal_last_err());
    TT_ASSERT_EQ(371, thermal_last());           /* last good value kept */
    TT_ASSERT_EQ(0, fault_is_latched());
    TT_ASSERT_EQ(FAULT_NONE, fault_active());
    TT_ASSERT_EQ(0, g_estop + g_rfoff);
    queue_raw(80);                               /* recovery clears the error */
    mock_time_advance_us(5000u * 1000u);
    thermal_tick();
    TT_ASSERT_EQ(0, thermal_last_err());
    TT_ASSERT_EQ(391, thermal_last());           /* 80*2500/256=781 mV -> 391 */
}

int main(void)
{
    TT_RUN(test_conversion_table);
    TT_RUN(test_conversion_uses_addr48_ch0_internal_ref);
    TT_RUN(test_read_errors);
    TT_RUN(test_not_before_5s_after_boot);
    TT_RUN(test_period_repeats);
    TT_RUN(test_first_tick_late_boot);
    TT_RUN(test_overtemp_raises_latched_fault);
    TT_RUN(test_sensor_error_is_not_a_fault);
    return TT_RESULT();
}
