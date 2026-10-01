#ifndef HAL_UART_H
#define HAL_UART_H
#include <stddef.h>

int uart_write(const char *s, size_t n);   /* blocking, 50 ms timeout; 0 or negative errno */
int uart_getc(void);                        /* -1 if no byte (RX ring buffer filled by IRQ) */

#endif
