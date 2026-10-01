#include "fpga_if.h"
#include "hal_gpio.h"
#include "hal_time.h"

#define FPGA_RESET_PULSE_MS 10

static uint8_t s_dig6_prev;
static uint8_t s_agc_enabled;

void fpga_if_init(void)
{
    gpio_write(PIN_FPGA_DIG0, 0);
    gpio_write(PIN_FPGA_DIG1, 0);
    gpio_write(PIN_FPGA_DIG2, 0);
    gpio_write(PIN_FPGA_DIG3, 0);
    gpio_write(PIN_FPGA_DIG4, 0);
    s_dig6_prev = 0;
    s_agc_enabled = 0;
}

void fpga_if_reset_pulse(void)
{
    gpio_write(PIN_FPGA_DIG4, 0);
    delay_ms(FPGA_RESET_PULSE_MS);
    gpio_write(PIN_FPGA_DIG4, 1);
}

void fpga_if_set_mixers(int on)       { gpio_write(PIN_FPGA_DIG3, on != 0); }
void fpga_if_toggle_chirp(void)       { gpio_toggle(PIN_FPGA_DIG0); }
void fpga_if_toggle_elevation(void)   { gpio_toggle(PIN_FPGA_DIG1); }
void fpga_if_toggle_azimuth(void)     { gpio_toggle(PIN_FPGA_DIG2); }

fpga_status_t fpga_if_sample(void)
{
    uint8_t v = gpio_read_fpga_port();
    fpga_status_t s;
    s.saturation = (uint8_t)((v >> 5) & 1u);
    s.agc_enable = (uint8_t)((v >> 6) & 1u);
    return s;
}

int fpga_if_agc_enable_debounced(uint8_t dig6_now)
{
    uint8_t now = dig6_now != 0;
    if (now == s_dig6_prev) {
        s_agc_enabled = now;
    }
    s_dig6_prev = now;
    return s_agc_enabled;
}
