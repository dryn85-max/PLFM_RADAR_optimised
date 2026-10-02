#include <errno.h>
#include <string.h>
#include "hal_uart.h"
#include "mock_log.h"

char mock_uart_tx[1024];
static size_t tx_len_;
#define RXQ_CAP 512
static uint8_t rxq_[RXQ_CAP];
static size_t rxq_head_, rxq_tail_;

void mock_uart_reset(void)
{
    memset(mock_uart_tx, 0, sizeof mock_uart_tx);
    tx_len_ = 0;
    rxq_head_ = rxq_tail_ = 0;
}

void mock_uart_push_rx(const char *s, size_t n)
{
    size_t i;
    for (i = 0; i < n && rxq_tail_ < RXQ_CAP; i++) {
        rxq_[rxq_tail_++] = (uint8_t)s[i];
    }
}

int uart_write(const char *s, size_t n)
{
    if (s == NULL) {
        return -EINVAL;
    }
    mock_log_add(MOCK_EV_UART_WRITE, 0, 0, 0, (const uint8_t *)s, n);
    if (tx_len_ + n < sizeof mock_uart_tx) {   /* keeps a terminating NUL */
        memcpy(mock_uart_tx + tx_len_, s, n);
        tx_len_ += n;
    }
    return 0;
}

int uart_getc(void)
{
    return (rxq_head_ < rxq_tail_) ? rxq_[rxq_head_++] : -1;
}
