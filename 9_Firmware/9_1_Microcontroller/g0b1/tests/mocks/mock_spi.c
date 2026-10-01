#include <errno.h>
#include <string.h>
#include "hal_spi.h"
#include "mock_log.h"

#define RXQ_CAP 512
static uint8_t rxq_[RXQ_CAP];
static size_t rxq_head_, rxq_tail_;
static int fail_;
static uint8_t default_rx_;

void mock_spi_reset(void) { rxq_head_ = rxq_tail_ = 0; fail_ = 0; default_rx_ = 0; }
void mock_spi_set_default_rx(uint8_t b) { default_rx_ = b; }

void mock_spi_set_rx(const uint8_t *bytes, size_t n)
{
    size_t i;
    for (i = 0; i < n && rxq_tail_ < RXQ_CAP; i++) {
        rxq_[rxq_tail_++] = bytes[i];
    }
}

void mock_spi_fail_next(int err) { fail_ = err; }

int spi_xfer(spi_bus_t bus, gpio_t cs, const uint8_t *tx, uint8_t *rx, size_t n)
{
    size_t i;
    int rc = 0;
    if ((int)bus < 0 || bus >= SPI_BUS_COUNT || (int)cs < 0 || cs >= PIN_COUNT ||
        tx == NULL || n == 0) {
        return -EINVAL;
    }
    if (fail_ != 0) {
        rc = fail_;
        fail_ = 0;
    }
    mock_log_add(MOCK_EV_SPI, bus, cs, rc, tx, n);
    if (rc != 0) {
        return rc;
    }
    if (rx != NULL) {
        for (i = 0; i < n; i++) {
            rx[i] = (rxq_head_ < rxq_tail_) ? rxq_[rxq_head_++] : default_rx_;
        }
    } else {
        /* MISO still clocks: consume the queue to stay frame-aligned */
        for (i = 0; i < n && rxq_head_ < rxq_tail_; i++) {
            rxq_head_++;
        }
    }
    return 0;
}
