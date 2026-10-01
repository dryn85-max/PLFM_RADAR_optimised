/* Global event log shared by all mocks; tests assert exact call ordering. */
#ifndef MOCK_LOG_H
#define MOCK_LOG_H
#include <stddef.h>
#include <stdint.h>

#define MOCK_LOG_CAP 4096

typedef enum {
    MOCK_EV_GPIO_WRITE,   /* a=pin, b=level */
    MOCK_EV_GPIO_TOGGLE,  /* a=pin, b=new level */
    MOCK_EV_SPI,          /* a=bus, b=cs pin, c=rc, bytes=tx[0..7], n=len */
    MOCK_EV_I2C_WRITE,    /* a=addr7, c=rc, bytes, n */
    MOCK_EV_I2C_READ,     /* a=addr7, c=rc, bytes=data returned, n */
    MOCK_EV_DELAY_US,     /* a=us */
    MOCK_EV_DELAY_MS,     /* a=ms */
    MOCK_EV_UART_WRITE    /* bytes, n */
} mock_ev_kind_t;

typedef struct {
    int kind;
    int a, b, c;
    uint8_t bytes[8];
    size_t n;
} mock_event_t;

extern mock_event_t mock_log[MOCK_LOG_CAP];
extern int mock_log_n;
extern int mock_log_overflow;   /* 1 once an event was dropped */
extern char mock_uart_tx[1024]; /* everything written via uart_write */

void mock_log_add(int kind, int a, int b, int c, const uint8_t *bytes, size_t n);

void mock_reset(void);                                    /* clears everything */
void mock_spi_set_rx(const uint8_t *bytes, size_t n);     /* queue MISO bytes */
void mock_spi_fail_next(int err);                         /* next spi_xfer returns err */
void mock_i2c_set_rx(const uint8_t *bytes, size_t n);     /* queue i2c_read bytes */
void mock_i2c_fail_next(int err);                         /* next i2c call returns err */
void mock_time_advance_us(uint32_t us);
void mock_time_set_hook(void (*fn)(uint32_t now_us));    /* called after every advance */
void mock_gpio_set_input(int pin, int level);
void mock_uart_push_rx(const char *s, size_t n);

#endif
