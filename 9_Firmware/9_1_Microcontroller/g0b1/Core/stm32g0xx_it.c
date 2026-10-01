#include "stm32g0xx_hal.h"
#include "hal_init.h"
#include "fault.h"

void NMI_Handler(void)
{
    for (;;) { }
}

void HardFault_Handler(void)
{
    fault_panic();   /* e-stop, latch, spin without IWDG refresh */
    for (;;) { }
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
