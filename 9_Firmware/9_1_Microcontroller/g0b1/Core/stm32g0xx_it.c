#include "stm32g0xx_hal.h"
#include "hal_init.h"

void NMI_Handler(void)
{
    for (;;) { }
}

void HardFault_Handler(void)
{
    for (;;) { }   /* replaced by fault_panic() in a later task */
}

void SVC_Handler(void) { }
void PendSV_Handler(void) { }

void SysTick_Handler(void)
{
    HAL_IncTick();
}

void USART2_LPUART2_IRQHandler(void)
{
    hal_uart_irq();
}
