#include <string.h>
#include "hal_gpio.h"
#include "mock_log.h"

static uint8_t level_[PIN_COUNT];
int mock_gpio_port_reads;   /* gpio_read_fpga_port() calls (reads are not logged) */
int mock_gpio_pin_reads;    /* gpio_read() calls */

void mock_gpio_reset(void)
{
    memset(level_, 0, sizeof level_);
    mock_gpio_port_reads = 0;
    mock_gpio_pin_reads = 0;
}

void mock_gpio_set_input(int pin, int level)
{
    if (pin >= 0 && pin < PIN_COUNT) {
        level_[pin] = level ? 1 : 0;   /* not logged: it is the environment, not a call */
    }
}

void gpio_write(gpio_t pin, int level)
{
    if ((int)pin < 0 || pin >= PIN_COUNT) {
        return;
    }
    level_[pin] = level ? 1 : 0;
    mock_log_add(MOCK_EV_GPIO_WRITE, pin, level_[pin], 0, NULL, 0);
}

int gpio_read(gpio_t pin)
{
    mock_gpio_pin_reads++;
    return ((int)pin >= 0 && pin < PIN_COUNT) ? level_[pin] : 0;
}

void gpio_toggle(gpio_t pin)
{
    if ((int)pin < 0 || pin >= PIN_COUNT) {
        return;
    }
    level_[pin] ^= 1u;
    mock_log_add(MOCK_EV_GPIO_TOGGLE, pin, level_[pin], 0, NULL, 0);
}

uint8_t gpio_read_fpga_port(void)
{
    uint8_t v = 0;
    int i;
    mock_gpio_port_reads++;
    for (i = 0; i < 8; i++) {
        v |= (uint8_t)(level_[PIN_FPGA_DIG0 + i] << i);
    }
    return v;
}
