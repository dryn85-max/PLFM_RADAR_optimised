#include "stm32g0xx_hal.h"

void HAL_MspInit(void)
{
    __HAL_RCC_SYSCFG_CLK_ENABLE();
    __HAL_RCC_PWR_CLK_ENABLE();
}

/* Peripheral MSP callbacks live next to their drivers: Core/hal/hal_{uart,spi,i2c}.c */
