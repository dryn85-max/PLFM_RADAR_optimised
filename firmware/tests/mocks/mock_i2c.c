#include <errno.h>
#include "hal_i2c.h"
#include "mock_log.h"

#define RXQ_CAP 256
static uint8_t rxq_[RXQ_CAP];
static size_t rxq_head_, rxq_tail_;
static int fail_;

void mock_i2c_reset(void) { rxq_head_ = rxq_tail_ = 0; fail_ = 0; }

void mock_i2c_set_rx(const uint8_t *bytes, size_t n)
{
    size_t i;
    for (i = 0; i < n && rxq_tail_ < RXQ_CAP; i++) {
        rxq_[rxq_tail_++] = bytes[i];
    }
}

void mock_i2c_fail_next(int err) { fail_ = err; }

int i2c_write(uint8_t addr7, const uint8_t *buf, size_t n)
{
    int rc = 0;
    if (buf == NULL || n == 0) {
        return -EINVAL;
    }
    if (fail_ != 0) {
        rc = fail_;
        fail_ = 0;
    }
    mock_log_add(MOCK_EV_I2C_WRITE, addr7, 0, rc, buf, n);
    return rc;
}

int i2c_read(uint8_t addr7, uint8_t *buf, size_t n)
{
    size_t i;
    int rc = 0;
    if (buf == NULL || n == 0) {
        return -EINVAL;
    }
    if (fail_ != 0) {
        rc = fail_;
        fail_ = 0;
    }
    if (rc == 0) {
        for (i = 0; i < n; i++) {
            buf[i] = (rxq_head_ < rxq_tail_) ? rxq_[rxq_head_++] : 0;
        }
    }
    mock_log_add(MOCK_EV_I2C_READ, addr7, 0, rc, rc == 0 ? buf : NULL, n);
    return rc;
}
