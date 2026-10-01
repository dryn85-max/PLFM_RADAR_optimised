/* Target GPIO: logical gpio_t -> port/pin table, pins from pins.h. */
#include "hal_gpio.h"
#include "hal_init.h"
#include "pins.h"
#include "stm32g0xx_hal.h"

typedef struct {
    GPIO_TypeDef *port;
    uint16_t pin;
    uint8_t is_output;
    uint8_t idle_high;   /* initial output level */
} gpio_desc_t;

/* Indexed by gpio_t. Keep in sync with hal_gpio.h and the pins.h header comment. */
static const gpio_desc_t k_gpio[PIN_COUNT] = {
    [PIN_LED]             = { GPIOA, GPIO_PIN_5,  1, 0 },
    /* Active-low chip selects idle high, everything else idles low. */
    [PIN_ADAR_CS0]        = { GPIOB, GPIO_PIN_12, 1, 1 },
    [PIN_ADAR_CS1]        = { GPIOB, GPIO_PIN_11, 1, 1 },
    [PIN_ADAR_CS2]        = { GPIOB, GPIO_PIN_10, 1, 1 },
    [PIN_ADAR_CS3]        = { GPIOB, GPIO_PIN_2,  1, 1 },
    [PIN_PLL_CS]          = { GPIOB, GPIO_PIN_1,  1, 1 },
    [PIN_PLL_CE]          = { GPIOB, GPIO_PIN_0,  1, 0 },
    [PIN_PLL_LD]          = { GPIOB, GPIO_PIN_6,  0, 0 },
    [PIN_EN_FPGA]         = { GPIOA, GPIO_PIN_0,  1, 0 },
    [PIN_EN_LO]           = { GPIOA, GPIO_PIN_1,  1, 0 },
    [PIN_EN_ADAR]         = { GPIOA, GPIO_PIN_4,  1, 0 },
    [PIN_EN_ADTR_VDD_SW]  = { GPIOA, GPIO_PIN_6,  1, 0 },
    [PIN_EN_ADTR_VSS_SW]  = { GPIOA, GPIO_PIN_7,  1, 0 },
    [PIN_EN_LNA]          = { GPIOA, GPIO_PIN_8,  1, 0 },
    [PIN_EN_PA]           = { GPIOA, GPIO_PIN_9,  1, 0 },
    [PIN_FPGA_DIG0]       = { GPIOC, GPIO_PIN_0,  1, 0 },
    [PIN_FPGA_DIG1]       = { GPIOC, GPIO_PIN_1,  1, 0 },
    [PIN_FPGA_DIG2]       = { GPIOC, GPIO_PIN_2,  1, 0 },
    [PIN_FPGA_DIG3]       = { GPIOC, GPIO_PIN_3,  1, 0 },
    [PIN_FPGA_DIG4]       = { GPIOC, GPIO_PIN_4,  1, 0 },
    [PIN_FPGA_DIG5]       = { GPIOC, GPIO_PIN_5,  0, 0 },
    [PIN_FPGA_DIG6]       = { GPIOC, GPIO_PIN_6,  0, 0 },
    [PIN_FPGA_DIG7]       = { GPIOC, GPIO_PIN_7,  0, 0 },
};

void hal_gpio_init(void)
{
    __HAL_RCC_GPIOA_CLK_ENABLE();
    __HAL_RCC_GPIOB_CLK_ENABLE();
    __HAL_RCC_GPIOC_CLK_ENABLE();

    for (int i = 0; i < PIN_COUNT; i++) {
        const gpio_desc_t *d = &k_gpio[i];
        GPIO_InitTypeDef g = {0};
        if (d->is_output) {
            /* Latch the level first so the pin never glitches to the wrong state. */
            HAL_GPIO_WritePin(d->port, d->pin, d->idle_high ? GPIO_PIN_SET : GPIO_PIN_RESET);
            g.Mode = GPIO_MODE_OUTPUT_PP;
        } else {
            g.Mode = GPIO_MODE_INPUT;
        }
        g.Pin = d->pin;
        g.Pull = GPIO_NOPULL;
        g.Speed = GPIO_SPEED_FREQ_LOW;
        HAL_GPIO_Init(d->port, &g);
    }
}

void gpio_write(gpio_t pin, int level)
{
    if ((unsigned)pin >= PIN_COUNT) {
        return;
    }
    HAL_GPIO_WritePin(k_gpio[pin].port, k_gpio[pin].pin, level ? GPIO_PIN_SET : GPIO_PIN_RESET);
}

int gpio_read(gpio_t pin)
{
    if ((unsigned)pin >= PIN_COUNT) {
        return 0;
    }
    return HAL_GPIO_ReadPin(k_gpio[pin].port, k_gpio[pin].pin) == GPIO_PIN_SET ? 1 : 0;
}

void gpio_toggle(gpio_t pin)
{
    if ((unsigned)pin >= PIN_COUNT) {
        return;
    }
    HAL_GPIO_TogglePin(k_gpio[pin].port, k_gpio[pin].pin);
}

uint8_t gpio_read_fpga_port(void)
{
    return (uint8_t)((GPIOC->IDR >> PIN_FPGA_PORT_SHIFT) & 0xFFu);
}
