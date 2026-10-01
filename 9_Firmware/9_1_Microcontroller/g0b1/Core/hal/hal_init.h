#ifndef HAL_INIT_H
#define HAL_INIT_H
/* Target-only init entry points (no host mock). Call order in main():
 * hal_gpio_init() first (all outputs low), then clock config, then the rest. */
void hal_gpio_init(void);
void hal_time_init(void);   /* after SystemClock_Config() */
void hal_uart_init(void);
void hal_spi_init(void);
void hal_i2c_init(void);
void hal_uart_irq(void);    /* called from USART2_LPUART2_IRQHandler */
#endif
