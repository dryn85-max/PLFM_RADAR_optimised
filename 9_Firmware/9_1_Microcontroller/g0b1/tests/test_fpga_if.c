/* Host tests for fpga_if.c: DIG0..7 contract (roles checked against
 * radar_system_top.v: DIG0 new_chirp, DIG1 new_elevation, DIG2 new_azimuth,
 * DIG3 mixers_enable, DIG4 reset_n, DIG5 agc saturation, DIG6 agc enable). */
#include "tinytest.h"
#include "mock_log.h"
#include "fpga_if.h"
#include "hal_gpio.h"

static void fresh(void) { mock_reset(); fpga_if_init(); mock_log_n = 0; }

static void test_init_all_low(void)
{
    int p;
    mock_reset();
    gpio_write(PIN_FPGA_DIG0, 1); gpio_write(PIN_FPGA_DIG3, 1); gpio_write(PIN_FPGA_DIG4, 1);
    fpga_if_init();
    for (p = PIN_FPGA_DIG0; p <= PIN_FPGA_DIG4; p++) TT_ASSERT_EQ(0, gpio_read((gpio_t)p));
}

static void test_reset_pulse(void)
{
    fresh();
    gpio_write(PIN_FPGA_DIG4, 1);
    mock_log_n = 0;
    fpga_if_reset_pulse();
    TT_ASSERT_EQ(3, mock_log_n);
    TT_ASSERT_EQ(MOCK_EV_GPIO_WRITE, mock_log[0].kind);
    TT_ASSERT_EQ(PIN_FPGA_DIG4, mock_log[0].a);
    TT_ASSERT_EQ(0, mock_log[0].b);
    TT_ASSERT_EQ(MOCK_EV_DELAY_MS, mock_log[1].kind);
    TT_ASSERT_EQ(10, mock_log[1].a);
    TT_ASSERT_EQ(MOCK_EV_GPIO_WRITE, mock_log[2].kind);
    TT_ASSERT_EQ(PIN_FPGA_DIG4, mock_log[2].a);
    TT_ASSERT_EQ(1, mock_log[2].b);
}

static void test_mixers(void)
{
    fresh();
    fpga_if_set_mixers(1);
    TT_ASSERT_EQ(1, gpio_read(PIN_FPGA_DIG3));
    fpga_if_set_mixers(5);   /* any nonzero = on */
    TT_ASSERT_EQ(1, gpio_read(PIN_FPGA_DIG3));
    fpga_if_set_mixers(0);
    TT_ASSERT_EQ(0, gpio_read(PIN_FPGA_DIG3));
    TT_ASSERT_EQ(3, mock_log_n);
}

static void test_toggles_touch_only_own_bit(void)
{
    static const struct { void (*fn)(void); int pin; } T[] = {
        { fpga_if_toggle_chirp, PIN_FPGA_DIG0 },
        { fpga_if_toggle_elevation, PIN_FPGA_DIG1 },
        { fpga_if_toggle_azimuth, PIN_FPGA_DIG2 },
    };
    unsigned i;
    int p;
    for (i = 0; i < 3; i++) {
        fresh();
        T[i].fn();
        TT_ASSERT_EQ(1, mock_log_n);
        TT_ASSERT_EQ(MOCK_EV_GPIO_TOGGLE, mock_log[0].kind);
        TT_ASSERT_EQ(T[i].pin, mock_log[0].a);
        for (p = PIN_FPGA_DIG0; p <= PIN_FPGA_DIG4; p++)
            TT_ASSERT_EQ(p == T[i].pin ? 1 : 0, gpio_read((gpio_t)p));
        T[i].fn();
        TT_ASSERT_EQ(0, gpio_read((gpio_t)T[i].pin));   /* second call flips back */
    }
}

static void test_sample_single_port_read(void)
{
    fpga_status_t s;
    fresh();
    mock_gpio_set_input(PIN_FPGA_DIG5, 1);
    s = fpga_if_sample();
    TT_ASSERT_EQ(1, mock_gpio_port_reads);
    TT_ASSERT_EQ(0, mock_gpio_pin_reads);
    TT_ASSERT_EQ(1, s.saturation);
    TT_ASSERT_EQ(0, s.agc_enable);
    mock_gpio_set_input(PIN_FPGA_DIG5, 0);
    mock_gpio_set_input(PIN_FPGA_DIG6, 1);
    s = fpga_if_sample();
    TT_ASSERT_EQ(2, mock_gpio_port_reads);
    TT_ASSERT_EQ(0, s.saturation);
    TT_ASSERT_EQ(1, s.agc_enable);
}

static void test_sample_ignores_other_bits(void)
{
    fpga_status_t s;
    fresh();
    mock_gpio_set_input(PIN_FPGA_DIG0, 1); mock_gpio_set_input(PIN_FPGA_DIG4, 1);
    mock_gpio_set_input(PIN_FPGA_DIG7, 1);
    s = fpga_if_sample();
    TT_ASSERT_EQ(0, s.saturation);
    TT_ASSERT_EQ(0, s.agc_enable);
    TT_ASSERT_EQ(0, mock_log_n);   /* sampling writes nothing */
}

static void test_debounce_sequence(void)
{
    /* 0,1,0,1,1 -> enable stays 0 until the 5th sample */
    fresh();
    TT_ASSERT_EQ(0, fpga_if_agc_enable_debounced(0));
    TT_ASSERT_EQ(0, fpga_if_agc_enable_debounced(1));
    TT_ASSERT_EQ(0, fpga_if_agc_enable_debounced(0));
    TT_ASSERT_EQ(0, fpga_if_agc_enable_debounced(1));
    TT_ASSERT_EQ(1, fpga_if_agc_enable_debounced(1));
}

static void test_debounce_glitch_while_enabled(void)
{
    fresh();
    fpga_if_agc_enable_debounced(1);
    TT_ASSERT_EQ(1, fpga_if_agc_enable_debounced(1));
    TT_ASSERT_EQ(1, fpga_if_agc_enable_debounced(0));   /* single glitch ignored */
    TT_ASSERT_EQ(1, fpga_if_agc_enable_debounced(1));   /* glitch over; prev was 0 -> still hold */
    TT_ASSERT_EQ(1, fpga_if_agc_enable_debounced(1));
    TT_ASSERT_EQ(1, fpga_if_agc_enable_debounced(0));
    TT_ASSERT_EQ(0, fpga_if_agc_enable_debounced(0));   /* two in a row -> off */
}

static void test_debounce_first_one_is_not_enough(void)
{
    fresh();
    /* boot prev=0: a single 1 never enables */
    TT_ASSERT_EQ(0, fpga_if_agc_enable_debounced(1));
    TT_ASSERT_EQ(0, fpga_if_agc_enable_debounced(0));
    TT_ASSERT_EQ(0, fpga_if_agc_enable_debounced(0));
}

static void test_debounce_nonzero_is_one_and_init_resets(void)
{
    fresh();
    fpga_if_agc_enable_debounced(0x20);
    TT_ASSERT_EQ(1, fpga_if_agc_enable_debounced(0x20));
    fpga_if_init();   /* boot default: AGC off */
    TT_ASSERT_EQ(0, fpga_if_agc_enable_debounced(1));
}

int main(void)
{
    TT_RUN(test_init_all_low);
    TT_RUN(test_reset_pulse);
    TT_RUN(test_mixers);
    TT_RUN(test_toggles_touch_only_own_bit);
    TT_RUN(test_sample_single_port_read);
    TT_RUN(test_sample_ignores_other_bits);
    TT_RUN(test_debounce_sequence);
    TT_RUN(test_debounce_glitch_while_enabled);
    TT_RUN(test_debounce_first_one_is_not_enough);
    TT_RUN(test_debounce_nonzero_is_one_and_init_resets);
    return TT_RESULT();
}
