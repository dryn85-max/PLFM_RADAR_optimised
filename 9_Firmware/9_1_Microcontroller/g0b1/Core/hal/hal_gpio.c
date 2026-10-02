/* Target GPIO: gpio_t -> GPIOx/pin via the pure-data table in pins_table.c. */
#include "hal_gpio.h"
#include "hal_init.h"
#include "pins.h"
#include "pins_table.h"
#include "stm32g0xx_hal.h"

static GPIO_TypeDef *port_of(uint8_t port)
{
    return port == PIN_PORT_A ? GPIOA : port == PIN_PORT_B ? GPIOB : GPIOC;
}

static uint16_t pin_mask(gpio_t pin)
{
    return (uint16_t)(1u << k_pin_table[pin].pin);
}

void hal_gpio_init(void)
{
    __HAL_RCC_GPIOA_CLK_ENABLE();
    __HAL_RCC_GPIOB_CLK_ENABLE();
    __HAL_RCC_GPIOC_CLK_ENABLE();

    for (int i = 0; i < PIN_COUNT; i++) {
        const pin_desc_t *d = &k_pin_table[i];
        GPIO_TypeDef *port = port_of(d->port);
        uint16_t mask = pin_mask((gpio_t)i);
        GPIO_InitTypeDef g = {0};
        if (d->is_output) {
            /* Latch the level first so the pin never glitches to the wrong state. */
            HAL_GPIO_WritePin(port, mask, d->idle_high ? GPIO_PIN_SET : GPIO_PIN_RESET);
            g.Mode = GPIO_MODE_OUTPUT_PP;
        } else {
            g.Mode = GPIO_MODE_INPUT;
        }
        g.Pin = mask;
        g.Pull = GPIO_NOPULL;
        g.Speed = GPIO_SPEED_FREQ_LOW;
        HAL_GPIO_Init(port, &g);
    }
}

void gpio_write(gpio_t pin, int level)
{
    if ((unsigned)pin >= PIN_COUNT) {
        return;
    }
    HAL_GPIO_WritePin(port_of(k_pin_table[pin].port), pin_mask(pin), level ? GPIO_PIN_SET : GPIO_PIN_RESET);
}

int gpio_read(gpio_t pin)
{
    if ((unsigned)pin >= PIN_COUNT) {
        return 0;
    }
    return HAL_GPIO_ReadPin(port_of(k_pin_table[pin].port), pin_mask(pin)) == GPIO_PIN_SET ? 1 : 0;
}

void gpio_toggle(gpio_t pin)
{
    if ((unsigned)pin >= PIN_COUNT) {
        return;
    }
    HAL_GPIO_TogglePin(port_of(k_pin_table[pin].port), pin_mask(pin));
}

uint8_t gpio_read_fpga_port(void)
{
    return (uint8_t)((GPIOC->IDR >> PIN_FPGA_PORT_SHIFT) & 0xFFu);
}
