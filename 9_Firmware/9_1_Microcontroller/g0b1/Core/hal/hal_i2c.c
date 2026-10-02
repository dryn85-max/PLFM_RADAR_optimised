/* Target I2C1, 100 kHz, 7-bit addressing (ADS7830 on PB8 SCL / PB9 SDA, AF6). */
#include <errno.h>
#include "hal_i2c.h"
#include "hal_init.h"
#include "stm32g0xx_hal.h"

/* I2CCLK = PCLK1 = 64 MHz (RCC_CCIPR.I2C1SEL default).
 * 0x10B17DB5: PRESC=1 (tPRESC 31.25 ns), SCLDEL=11 (375 ns), SDADEL=1,
 * SCLH=125 (3.94 us), SCLL=181 (5.69 us): SCL period ~9.6 us + filter/sync/edges
 * -> just under 100 kHz. Computed per RM0444 I2C timing formulae; not measured. */
#define I2C1_TIMING 0x10B17DB5u
#define I2C_TIMEOUT_MS 100u

static I2C_HandleTypeDef hi2c1;

void hal_i2c_init(void)
{
    hi2c1.Instance = I2C1;
    hi2c1.Init.Timing = I2C1_TIMING;
    hi2c1.Init.OwnAddress1 = 0;
    hi2c1.Init.AddressingMode = I2C_ADDRESSINGMODE_7BIT;
    hi2c1.Init.DualAddressMode = I2C_DUALADDRESS_DISABLE;
    hi2c1.Init.OwnAddress2 = 0;
    hi2c1.Init.OwnAddress2Masks = I2C_OA2_NOMASK;
    hi2c1.Init.GeneralCallMode = I2C_GENERALCALL_DISABLE;
    hi2c1.Init.NoStretchMode = I2C_NOSTRETCH_DISABLE;
    if (HAL_I2C_Init(&hi2c1) != HAL_OK ||
        HAL_I2CEx_ConfigAnalogFilter(&hi2c1, I2C_ANALOGFILTER_ENABLE) != HAL_OK ||
        HAL_I2CEx_ConfigDigitalFilter(&hi2c1, 0) != HAL_OK) {
        Error_Handler();   /* never returns: e-stop + latch + spin */
    }
}

void HAL_I2C_MspInit(I2C_HandleTypeDef *hi2c)
{
    if (hi2c->Instance == I2C1) {
        GPIO_InitTypeDef g = {0};
        __HAL_RCC_GPIOB_CLK_ENABLE();
        __HAL_RCC_I2C1_CLK_ENABLE();
        g.Pin = GPIO_PIN_8 | GPIO_PIN_9;      /* PB8 SCL, PB9 SDA */
        g.Mode = GPIO_MODE_AF_OD;
        g.Pull = GPIO_PULLUP;
        g.Speed = GPIO_SPEED_FREQ_LOW;
        g.Alternate = GPIO_AF6_I2C1;
        HAL_GPIO_Init(GPIOB, &g);
    }
}

static int map(HAL_StatusTypeDef st)
{
    if (st == HAL_OK) {
        return 0;
    }
    return (st == HAL_TIMEOUT) ? -ETIMEDOUT : -EIO;
}

int i2c_write(uint8_t addr7, const uint8_t *buf, size_t n)
{
    if (buf == NULL || n == 0u || n > 0xFFFFu) {
        return -EINVAL;
    }
    return map(HAL_I2C_Master_Transmit(&hi2c1, (uint16_t)(addr7 << 1),
                                       (uint8_t *)(uintptr_t)buf, (uint16_t)n, I2C_TIMEOUT_MS));
}

int i2c_read(uint8_t addr7, uint8_t *buf, size_t n)
{
    if (buf == NULL || n == 0u || n > 0xFFFFu) {
        return -EINVAL;
    }
    return map(HAL_I2C_Master_Receive(&hi2c1, (uint16_t)(addr7 << 1), buf, (uint16_t)n,
                                      I2C_TIMEOUT_MS));
}
