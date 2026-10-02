#include <string.h>
#include "mock_log.h"

mock_event_t mock_log[MOCK_LOG_CAP];
int mock_log_n;
int mock_log_overflow;

void mock_log_add(int kind, int a, int b, int c, const uint8_t *bytes, size_t n)
{
    mock_event_t *e;
    if (mock_log_n >= MOCK_LOG_CAP) {
        mock_log_overflow = 1;
        return;
    }
    e = &mock_log[mock_log_n++];
    memset(e, 0, sizeof *e);
    e->kind = kind;
    e->a = a;
    e->b = b;
    e->c = c;
    e->n = n;
    if (bytes != NULL) {
        memcpy(e->bytes, bytes, n < sizeof e->bytes ? n : sizeof e->bytes);
    }
}

/* Per-module resets live in their own files; mock_reset() calls them all. */
void mock_gpio_reset(void);
void mock_spi_reset(void);
void mock_i2c_reset(void);
void mock_time_reset(void);
void mock_uart_reset(void);

void mock_reset(void)
{
    mock_log_n = 0;
    mock_log_overflow = 0;
    memset(mock_log, 0, sizeof mock_log);
    mock_gpio_reset();
    mock_spi_reset();
    mock_i2c_reset();
    mock_time_reset();
    mock_uart_reset();
}
