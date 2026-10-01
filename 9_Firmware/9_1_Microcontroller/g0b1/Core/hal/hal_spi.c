/* Target SPI: ADAR1000 on SPI2, PLL on SPI1. Mode 0, MSB first, 8-bit, software CS. */
#include <errno.h>
#include "hal_spi.h"
#include "hal_init.h"
#include "pins.h"
#include "stm32g0xx_hal.h"

/* TODO(Task 4): take these from config.h once it exists. */
#ifndef SPI_ADAR_MAX_HZ
#define SPI_ADAR_MAX_HZ 20000000u
#endif
#ifndef SPI_PLL_MAX_HZ
#define SPI_PLL_MAX_HZ 10000000u
#endif
#define SPI_TIMEOUT_MS 10u

static SPI_HandleTypeDef hspi1;   /* PLL  */
static SPI_HandleTypeDef hspi2;   /* ADAR */

static SPI_HandleTypeDef *handle_for(spi_bus_t bus)
{
    switch (bus) {
    case SPI_BUS_ADAR: return &hspi2;
    case SPI_BUS_PLL:  return &hspi1;
    default:           return NULL;
    }
}

static void spi_setup(SPI_HandleTypeDef *h, SPI_TypeDef *inst, uint32_t max_hz)
{
    uint32_t div = spi_prescaler_for(HAL_RCC_GetPCLK1Freq(), max_hz);
    uint32_t k = 0;                       /* div = 2^(k+1) -> CR1.BR = k */
    while ((2u << k) < div) {
        k++;
    }
    h->Instance = inst;
    h->Init.Mode = SPI_MODE_MASTER;
    h->Init.Direction = SPI_DIRECTION_2LINES;
    h->Init.DataSize = SPI_DATASIZE_8BIT;
    h->Init.CLKPolarity = SPI_POLARITY_LOW;
    h->Init.CLKPhase = SPI_PHASE_1EDGE;
    h->Init.NSS = SPI_NSS_SOFT;
    h->Init.BaudRatePrescaler = k << SPI_CR1_BR_Pos;
    h->Init.FirstBit = SPI_FIRSTBIT_MSB;
    h->Init.TIMode = SPI_TIMODE_DISABLE;
    h->Init.CRCCalculation = SPI_CRCCALCULATION_DISABLE;
    h->Init.CRCPolynomial = 7;
    h->Init.CRCLength = SPI_CRC_LENGTH_DATASIZE;
    h->Init.NSSPMode = SPI_NSS_PULSE_DISABLE;
    if (HAL_SPI_Init(h) != HAL_OK) {
        for (;;) { }   /* fault_panic() hooks in later */
    }
}

void hal_spi_init(void)
{
    spi_setup(&hspi2, SPI2, SPI_ADAR_MAX_HZ);
    spi_setup(&hspi1, SPI1, SPI_PLL_MAX_HZ);
}

/* AF numbers below are from the G0B1 datasheet pin table; the HAL headers only
 * define the AF numbers (GPIO_AF0_SPI1/SPI2), not the pin assignment. */
void HAL_SPI_MspInit(SPI_HandleTypeDef *hspi)
{
    GPIO_InitTypeDef g = {0};
    g.Mode = GPIO_MODE_AF_PP;
    g.Pull = GPIO_NOPULL;
    g.Speed = GPIO_SPEED_FREQ_HIGH;
    __HAL_RCC_GPIOB_CLK_ENABLE();
    if (hspi->Instance == SPI2) {          /* PB13 SCK, PB14 MISO, PB15 MOSI */
        __HAL_RCC_SPI2_CLK_ENABLE();
        g.Pin = GPIO_PIN_13 | GPIO_PIN_14 | GPIO_PIN_15;
        g.Alternate = GPIO_AF0_SPI2;
        HAL_GPIO_Init(GPIOB, &g);
    } else if (hspi->Instance == SPI1) {   /* PB3 SCK, PB4 MISO, PB5 MOSI */
        __HAL_RCC_SPI1_CLK_ENABLE();
        g.Pin = GPIO_PIN_3 | GPIO_PIN_4 | GPIO_PIN_5;
        g.Alternate = GPIO_AF0_SPI1;
        HAL_GPIO_Init(GPIOB, &g);
    }
}

int spi_xfer(spi_bus_t bus, gpio_t cs, const uint8_t *tx, uint8_t *rx, size_t n)
{
    SPI_HandleTypeDef *h = handle_for(bus);
    if (h == NULL || (unsigned)cs >= PIN_COUNT || tx == NULL || n == 0u || n > 0xFFFFu) {
        return -EINVAL;
    }
    HAL_StatusTypeDef st;
    gpio_write(cs, 0);
    if (rx != NULL) {
        st = HAL_SPI_TransmitReceive(h, (uint8_t *)(uintptr_t)tx, rx, (uint16_t)n, SPI_TIMEOUT_MS);
    } else {
        st = HAL_SPI_Transmit(h, (uint8_t *)(uintptr_t)tx, (uint16_t)n, SPI_TIMEOUT_MS);
    }
    gpio_write(cs, 1);
    if (st == HAL_OK) {
        return 0;
    }
    return (st == HAL_TIMEOUT) ? -ETIMEDOUT : -EIO;
}
