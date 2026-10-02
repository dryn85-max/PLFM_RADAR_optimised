/* HAL configuration: only the modules this firmware uses. */
#ifndef STM32G0xx_HAL_CONF_H
#define STM32G0xx_HAL_CONF_H

#ifdef __cplusplus
extern "C" {
#endif

#define HAL_MODULE_ENABLED
#define HAL_CORTEX_MODULE_ENABLED
#define HAL_DMA_MODULE_ENABLED
#define HAL_FLASH_MODULE_ENABLED
#define HAL_GPIO_MODULE_ENABLED
#define HAL_I2C_MODULE_ENABLED
#define HAL_IWDG_MODULE_ENABLED
#define HAL_PWR_MODULE_ENABLED
#define HAL_RCC_MODULE_ENABLED
#define HAL_SPI_MODULE_ENABLED
#define HAL_TIM_MODULE_ENABLED
#define HAL_UART_MODULE_ENABLED

#define USE_HAL_I2C_REGISTER_CALLBACKS   0u
#define USE_HAL_SPI_REGISTER_CALLBACKS   0u
#define USE_HAL_TIM_REGISTER_CALLBACKS   0u
#define USE_HAL_UART_REGISTER_CALLBACKS  0u

#if !defined(HSE_VALUE)
#define HSE_VALUE            (8000000UL)
#endif
#if !defined(HSE_STARTUP_TIMEOUT)
#define HSE_STARTUP_TIMEOUT  (100UL)
#endif
#if !defined(HSI_VALUE)
#define HSI_VALUE            (16000000UL)
#endif
#if !defined(HSI48_VALUE)
#define HSI48_VALUE          48000000U
#endif
#if !defined(LSI_VALUE)
#define LSI_VALUE            (32000UL)
#endif
#if !defined(LSE_VALUE)
#define LSE_VALUE            (32768UL)
#endif
#if !defined(LSE_STARTUP_TIMEOUT)
#define LSE_STARTUP_TIMEOUT  (5000UL)
#endif
#if !defined(EXTERNAL_I2S1_CLOCK_VALUE)
#define EXTERNAL_I2S1_CLOCK_VALUE (48000UL)
#endif
#if !defined(EXTERNAL_I2S2_CLOCK_VALUE)
#define EXTERNAL_I2S2_CLOCK_VALUE 48000U
#endif

#define VDD_VALUE                 (3300UL)
#define TICK_INT_PRIORITY         ((1UL << __NVIC_PRIO_BITS) - 1UL)
#define USE_RTOS                  0U
#define PREFETCH_ENABLE           1U
#define INSTRUCTION_CACHE_ENABLE  1U
#define USE_SPI_CRC               0U

#include "stm32g0xx_hal_rcc.h"
#include "stm32g0xx_hal_gpio.h"
#include "stm32g0xx_hal_dma.h"
#include "stm32g0xx_hal_cortex.h"
#include "stm32g0xx_hal_flash.h"
#include "stm32g0xx_hal_i2c.h"
#include "stm32g0xx_hal_iwdg.h"
#include "stm32g0xx_hal_pwr.h"
#include "stm32g0xx_hal_spi.h"
#include "stm32g0xx_hal_tim.h"
#include "stm32g0xx_hal_uart.h"

#define assert_param(expr) ((void)0U)

#ifdef __cplusplus
}
#endif

#endif
