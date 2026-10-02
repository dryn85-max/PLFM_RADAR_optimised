#ifndef HAL_SPI_H
#define HAL_SPI_H
#include <stddef.h>
#include <stdint.h>
#include "hal_gpio.h"

typedef enum { SPI_BUS_ADAR, SPI_BUS_PLL, SPI_BUS_COUNT } spi_bus_t;

/* Blocking full-duplex transfer, CS (software) asserted for the whole frame.
 * tx is required, rx may be NULL. Returns 0, -EIO, -ETIMEDOUT or -EINVAL. */
int spi_xfer(spi_bus_t bus, gpio_t cs, const uint8_t *tx, uint8_t *rx, size_t n);

/* Pure: smallest divider 2^k (2..256) with pclk_hz / 2^k <= max_hz; 256 if none. */
uint32_t spi_prescaler_for(uint32_t pclk_hz, uint32_t max_hz);

#endif
