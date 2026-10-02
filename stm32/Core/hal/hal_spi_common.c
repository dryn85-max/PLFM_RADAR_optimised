/* Shared by target and host builds. */
#include "hal_spi.h"

uint32_t spi_prescaler_for(uint32_t pclk_hz, uint32_t max_hz)
{
    uint32_t div = 2u;
    while (div < 256u && (pclk_hz / div) > max_hz) {
        div <<= 1;
    }
    return div;
}
