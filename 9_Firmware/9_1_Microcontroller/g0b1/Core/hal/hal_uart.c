/* Target USART2 (ST-LINK VCP, PA2/PA3 AF1), 115200 8N1; RX via IRQ into a 128 B ring. */
#include <errno.h>
#include "hal_uart.h"
#include "hal_init.h"
#include "stm32g0xx_hal.h"

#define RX_RING_SIZE 128u   /* power of two */
#define UART_TX_TIMEOUT_MS 50u

static UART_HandleTypeDef huart2;
static volatile uint8_t rx_buf[RX_RING_SIZE];
static volatile uint8_t rx_head;   /* written by the ISR only */
static volatile uint8_t rx_tail;   /* written by uart_getc() only */

void hal_uart_init(void)
{
    huart2.Instance = USART2;
    huart2.Init.BaudRate = 115200;
    huart2.Init.WordLength = UART_WORDLENGTH_8B;
    huart2.Init.StopBits = UART_STOPBITS_1;
    huart2.Init.Parity = UART_PARITY_NONE;
    huart2.Init.Mode = UART_MODE_TX_RX;
    huart2.Init.HwFlowCtl = UART_HWCONTROL_NONE;
    huart2.Init.OverSampling = UART_OVERSAMPLING_16;
    huart2.Init.OneBitSampling = UART_ONE_BIT_SAMPLE_DISABLE;
    huart2.Init.ClockPrescaler = UART_PRESCALER_DIV1;
    huart2.AdvancedInit.AdvFeatureInit = UART_ADVFEATURE_NO_INIT;
    if (HAL_UART_Init(&huart2) != HAL_OK) {
        Error_Handler();   /* never returns: e-stop + latch + spin */
    }
    SET_BIT(USART2->CR1, USART_CR1_RXNEIE_RXFNEIE);
    HAL_NVIC_SetPriority(USART2_LPUART2_IRQn, 3, 0);
    HAL_NVIC_EnableIRQ(USART2_LPUART2_IRQn);
}

/* USART2 on PA2 (TX) / PA3 (RX), AF1. */
void HAL_UART_MspInit(UART_HandleTypeDef *huart)
{
    GPIO_InitTypeDef g = {0};
    if (huart->Instance == USART2) {
        __HAL_RCC_USART2_CLK_ENABLE();
        __HAL_RCC_GPIOA_CLK_ENABLE();
        g.Pin = GPIO_PIN_2 | GPIO_PIN_3;
        g.Mode = GPIO_MODE_AF_PP;
        g.Pull = GPIO_PULLUP;
        g.Speed = GPIO_SPEED_FREQ_LOW;
        g.Alternate = GPIO_AF1_USART2;
        HAL_GPIO_Init(GPIOA, &g);
    }
}

void hal_uart_irq(void)
{
    uint32_t isr = USART2->ISR;
    if (isr & USART_ISR_RXNE_RXFNE) {
        uint8_t b = (uint8_t)USART2->RDR;           /* read clears RXNE */
        uint8_t next = (uint8_t)((rx_head + 1u) & (RX_RING_SIZE - 1u));
        if (next != rx_tail) {                      /* full: drop the new byte */
            rx_buf[rx_head] = b;
            rx_head = next;
        }
    }
    if (isr & (USART_ISR_ORE | USART_ISR_FE | USART_ISR_NE | USART_ISR_PE)) {
        USART2->ICR = USART_ICR_ORECF | USART_ICR_FECF | USART_ICR_NECF | USART_ICR_PECF;
    }
}

int uart_getc(void)
{
    if (rx_tail == rx_head) {
        return -1;
    }
    uint8_t b = rx_buf[rx_tail];
    rx_tail = (uint8_t)((rx_tail + 1u) & (RX_RING_SIZE - 1u));
    return b;
}

int uart_write(const char *s, size_t n)
{
    if (s == NULL || n > 0xFFFFu) {
        return -EINVAL;
    }
    if (n == 0u) {
        return 0;
    }
    HAL_StatusTypeDef st = HAL_UART_Transmit(&huart2, (uint8_t *)(uintptr_t)s, (uint16_t)n,
                                             UART_TX_TIMEOUT_MS);
    if (st == HAL_OK) {
        return 0;
    }
    return (st == HAL_TIMEOUT) ? -ETIMEDOUT : -EIO;
}
