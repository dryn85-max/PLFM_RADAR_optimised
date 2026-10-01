/* Target time base. The only delay implementation in the firmware. */
#include "hal_time.h"
#include "hal_init.h"
#include "stm32g0xx_hal.h"

void hal_time_init(void)
{
    __HAL_RCC_TIM2_CLK_ENABLE();
    /* APB prescaler is 1, so the timer clock equals PCLK1 (64 MHz) -> PSC = 63. */
    TIM2->PSC = (HAL_RCC_GetPCLK1Freq() / 1000000u) - 1u;
    TIM2->ARR = 0xFFFFFFFFu;     /* TIM2 is 32-bit on G0B1: wraps every ~71.6 min */
    TIM2->EGR = TIM_EGR_UG;
    TIM2->CR1 = TIM_CR1_CEN;
}

uint32_t millis(void)
{
    return HAL_GetTick();
}

uint32_t micros(void)
{
    return TIM2->CNT;
}

void delay_us(uint32_t us)
{
    uint32_t start = micros();
    while ((uint32_t)(micros() - start) < us) { }
}

void delay_ms(uint32_t ms)
{
    while (ms--) {
        delay_us(1000u);
    }
}
