#include <errno.h>
#include <string.h>
#include "tinytest.h"
#include "mock_log.h"
#include "hal_gpio.h"
#include "hal_spi.h"
#include "hal_i2c.h"
#include "hal_time.h"
#include "hal_uart.h"

static void test_prescaler(void)
{
    TT_ASSERT_EQ(4, spi_prescaler_for(64000000u, 20000000u));
    TT_ASSERT_EQ(8, spi_prescaler_for(64000000u, 10000000u));
    TT_ASSERT_EQ(2, spi_prescaler_for(64000000u, 100000000u));
    TT_ASSERT_EQ(256, spi_prescaler_for(64000000u, 100000u));
    /* boundaries: exact hit keeps the divider, one Hz less doubles it */
    TT_ASSERT_EQ(4, spi_prescaler_for(64000000u, 16000000u));
    TT_ASSERT_EQ(8, spi_prescaler_for(64000000u, 15999999u));
    TT_ASSERT_EQ(256, spi_prescaler_for(64000000u, 0u));
}

static void test_log_order(void)
{
    const uint8_t tx[3] = {0x01, 0x02, 0x03};
    const uint8_t i2c[2] = {0xAA, 0xBB};
    mock_reset();
    gpio_write(PIN_EN_FPGA, 1);
    TT_ASSERT_EQ(0, spi_xfer(SPI_BUS_ADAR, PIN_ADAR_CS0, tx, NULL, 3));
    TT_ASSERT_EQ(0, i2c_write(0x48, i2c, 2));
    delay_ms(5);
    TT_ASSERT_EQ(4, mock_log_n);
    TT_ASSERT_EQ(MOCK_EV_GPIO_WRITE, mock_log[0].kind);
    TT_ASSERT_EQ(PIN_EN_FPGA, mock_log[0].a);
    TT_ASSERT_EQ(1, mock_log[0].b);
    TT_ASSERT_EQ(MOCK_EV_SPI, mock_log[1].kind);
    TT_ASSERT_EQ(SPI_BUS_ADAR, mock_log[1].a);
    TT_ASSERT_EQ(PIN_ADAR_CS0, mock_log[1].b);
    TT_ASSERT_EQ(3, mock_log[1].n);
    TT_ASSERT(memcmp(mock_log[1].bytes, tx, 3) == 0);
    TT_ASSERT_EQ(MOCK_EV_I2C_WRITE, mock_log[2].kind);
    TT_ASSERT_EQ(0x48, mock_log[2].a);
    TT_ASSERT_EQ(2, mock_log[2].n);
    TT_ASSERT_EQ(0xBB, mock_log[2].bytes[1]);
    TT_ASSERT_EQ(MOCK_EV_DELAY_MS, mock_log[3].kind);
    TT_ASSERT_EQ(5, mock_log[3].a);
}

static void test_spi_rx_and_errors(void)
{
    const uint8_t tx[3] = {0x80, 0x00, 0x00};
    const uint8_t miso[3] = {0x11, 0x22, 0x33};
    uint8_t rx[3] = {0, 0, 0};
    mock_reset();
    mock_spi_set_rx(miso, 3);
    TT_ASSERT_EQ(0, spi_xfer(SPI_BUS_PLL, PIN_PLL_CS, tx, rx, 3));
    TT_ASSERT(memcmp(rx, miso, 3) == 0);
    /* queue exhausted -> zeros, not stale data */
    TT_ASSERT_EQ(0, spi_xfer(SPI_BUS_PLL, PIN_PLL_CS, tx, rx, 3));
    TT_ASSERT_EQ(0, rx[0]);
    TT_ASSERT_EQ(-EINVAL, spi_xfer(SPI_BUS_COUNT, PIN_PLL_CS, tx, rx, 3));
    TT_ASSERT_EQ(-EINVAL, spi_xfer(SPI_BUS_PLL, PIN_PLL_CS, NULL, rx, 3));
    TT_ASSERT_EQ(-EINVAL, spi_xfer(SPI_BUS_PLL, PIN_PLL_CS, tx, rx, 0));
    mock_spi_fail_next(-ETIMEDOUT);
    TT_ASSERT_EQ(-ETIMEDOUT, spi_xfer(SPI_BUS_PLL, PIN_PLL_CS, tx, NULL, 3));
    TT_ASSERT_EQ(0, spi_xfer(SPI_BUS_PLL, PIN_PLL_CS, tx, NULL, 3)); /* one-shot */
}

static void test_i2c(void)
{
    uint8_t buf[2] = {0, 0};
    const uint8_t src[2] = {0x5A, 0xA5};
    mock_reset();
    mock_i2c_set_rx(src, 2);
    TT_ASSERT_EQ(0, i2c_read(0x48, buf, 2));
    TT_ASSERT_EQ(0x5A, buf[0]);
    TT_ASSERT_EQ(0xA5, buf[1]);
    mock_i2c_fail_next(-EIO);
    TT_ASSERT_EQ(-EIO, i2c_write(0x48, src, 2));
    TT_ASSERT_EQ(0, i2c_write(0x48, src, 2));
    mock_i2c_fail_next(-ETIMEDOUT);
    TT_ASSERT_EQ(-ETIMEDOUT, i2c_read(0x48, buf, 2));
    TT_ASSERT_EQ(-EINVAL, i2c_write(0x48, NULL, 2));
}

static void test_gpio_and_time(void)
{
    mock_reset();
    gpio_write(PIN_FPGA_DIG0, 1);
    gpio_write(PIN_FPGA_DIG3, 1);
    TT_ASSERT_EQ(1, gpio_read(PIN_FPGA_DIG0));
    gpio_toggle(PIN_FPGA_DIG0);
    TT_ASSERT_EQ(0, gpio_read(PIN_FPGA_DIG0));
    mock_gpio_set_input(PIN_FPGA_DIG5, 1);
    mock_gpio_set_input(PIN_FPGA_DIG7, 1);
    TT_ASSERT_EQ(0xA8, gpio_read_fpga_port()); /* DIG7|DIG5|DIG3 */
    gpio_write(PIN_LED, 5); /* any nonzero is high */
    TT_ASSERT_EQ(1, gpio_read(PIN_LED));

    TT_ASSERT_EQ(0, micros());
    delay_us(1500);
    TT_ASSERT_EQ(1500, micros());
    TT_ASSERT_EQ(1, millis());
    delay_ms(10);
    TT_ASSERT_EQ(11, millis());
    mock_time_advance_us(4294967295u); /* wrap */
    TT_ASSERT_EQ(11500u - 1u, micros());
}

static void test_uart(void)
{
    mock_reset();
    TT_ASSERT_EQ(-1, uart_getc());
    mock_uart_push_rx("ab", 2);
    TT_ASSERT_EQ('a', uart_getc());
    TT_ASSERT_EQ('b', uart_getc());
    TT_ASSERT_EQ(-1, uart_getc());
    TT_ASSERT_EQ(0, uart_write("hi\r\n", 4));
    TT_ASSERT_EQ(4, (long long)strlen(mock_uart_tx));
    TT_ASSERT(strcmp(mock_uart_tx, "hi\r\n") == 0);
    mock_reset();
    TT_ASSERT_EQ(0, (long long)strlen(mock_uart_tx));
}

static void test_log_cap(void)
{
    int i;
    mock_reset();
    for (i = 0; i < MOCK_LOG_CAP + 10; i++) {
        gpio_write(PIN_LED, i & 1);
    }
    TT_ASSERT_EQ(MOCK_LOG_CAP, mock_log_n);
    TT_ASSERT(mock_log_overflow == 1);
}

int main(void)
{
    TT_RUN(test_prescaler);
    TT_RUN(test_log_order);
    TT_RUN(test_spi_rx_and_errors);
    TT_RUN(test_i2c);
    TT_RUN(test_gpio_and_time);
    TT_RUN(test_uart);
    TT_RUN(test_log_cap);
    return TT_RESULT();
}
