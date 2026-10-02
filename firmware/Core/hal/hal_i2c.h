#ifndef HAL_I2C_H
#define HAL_I2C_H
#include <stddef.h>
#include <stdint.h>

/* 100 ms timeout. Return 0, -EIO, -ETIMEDOUT or -EINVAL. */
int i2c_write(uint8_t addr7, const uint8_t *buf, size_t n);
int i2c_read(uint8_t addr7, uint8_t *buf, size_t n);

#endif
