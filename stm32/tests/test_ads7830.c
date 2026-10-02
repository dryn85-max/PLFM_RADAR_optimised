/* Host tests for the ADS7830 driver. Command byte format and channel encoding:
 * ADS7830 datasheet SBAS302C, Command Byte (p. 13) and Table 2 (p. 14). */
#include <errno.h>
#include "tinytest.h"
#include "mock_log.h"
#include "ads7830.h"

static int count_kind(int kind)
{
    int i, n = 0;
    for (i = 0; i < mock_log_n; i++) if (mock_log[i].kind == kind) n++;
    return n;
}

static void test_command_byte_all_channels(void)
{
    /* SD=1 rows of Table 2: C2C1C0 = 000,100,001,101,010,110,011,111 for CH0..CH7.
     * Hard-coded from the table, NOT derived from the formula under test. */
    static const uint8_t expect_pd0[8]  = { 0x80, 0xC0, 0x90, 0xD0, 0xA0, 0xE0, 0xB0, 0xF0 };
    uint8_t ch;
    for (ch = 0; ch < 8; ch++) {
        mock_reset();
        TT_ASSERT_EQ(expect_pd0[ch], ads7830_command(ch, ADS7830_PD_AUTO));
        TT_ASSERT_EQ(expect_pd0[ch] | 0x0C, ads7830_command(ch, ADS7830_PD_REF_ON_ADC_ON));
        TT_ASSERT_EQ(expect_pd0[ch] | 0x04, ads7830_command(ch, ADS7830_PD_REF_OFF_ADC_ON));
        TT_ASSERT_EQ(expect_pd0[ch] | 0x08, ads7830_command(ch, ADS7830_PD_REF_ON_ADC_OFF));
    }
}

static void test_read_sequence(void)
{
    static const uint8_t v[1] = { 0x4C };
    mock_reset();
    mock_i2c_set_rx(v, 1);
    TT_ASSERT_EQ(0x4C, ads7830_read_se(0x48, 2, ADS7830_PD_REF_ON_ADC_ON));
    /* write(cmd) -> delay -> read(1) */
    TT_ASSERT_EQ(3, mock_log_n);
    TT_ASSERT_EQ(MOCK_EV_I2C_WRITE, mock_log[0].kind);
    TT_ASSERT_EQ(0x48, mock_log[0].a);
    TT_ASSERT_EQ(1, mock_log[0].n);
    TT_ASSERT_EQ(0x90 | 0x0C, mock_log[0].bytes[0]);
    TT_ASSERT_EQ(MOCK_EV_DELAY_MS, mock_log[1].kind);
    TT_ASSERT_EQ(MOCK_EV_I2C_READ, mock_log[2].kind);
    TT_ASSERT_EQ(0x48, mock_log[2].a);
    TT_ASSERT_EQ(1, mock_log[2].n);
}

static void test_ff_is_data_not_error(void)
{
    static const uint8_t v[1] = { 0xFF };
    mock_reset();
    mock_i2c_set_rx(v, 1);
    TT_ASSERT_EQ(255, ads7830_read_se(0x48, 0, 0x0C));
}

static void test_zero_is_data(void)
{
    mock_reset();
    TT_ASSERT_EQ(0, ads7830_read_se(0x49, 7, 0));
}

static void test_write_error_no_read(void)
{
    int rc;
    mock_reset();
    mock_i2c_fail_next(-EIO);
    rc = ads7830_read_se(0x48, 0, 0x0C);
    TT_ASSERT_EQ(-EIO, rc);
    TT_ASSERT_EQ(0, count_kind(MOCK_EV_I2C_READ));
    mock_reset();
    mock_i2c_fail_next(-ETIMEDOUT);
    TT_ASSERT_EQ(-ETIMEDOUT, ads7830_read_se(0x48, 0, 0x0C));
    TT_ASSERT_EQ(0, count_kind(MOCK_EV_I2C_READ));
}

static void hook_fail_read(uint32_t now_us)
{
    (void)now_us;
    mock_i2c_fail_next(-ETIMEDOUT);
}

static void test_read_error(void)
{
    mock_reset();
    mock_time_set_hook(hook_fail_read);   /* fires after delay_ms, before the read */
    TT_ASSERT_EQ(-ETIMEDOUT, ads7830_read_se(0x48, 1, 0x0C));
    TT_ASSERT_EQ(1, count_kind(MOCK_EV_I2C_WRITE));
    TT_ASSERT_EQ(1, count_kind(MOCK_EV_I2C_READ));
}

static void test_bad_arguments_no_traffic(void)
{
    mock_reset();
    TT_ASSERT_EQ(-EINVAL, ads7830_read_se(0x48, 8, 0x0C));      /* channel */
    TT_ASSERT_EQ(-EINVAL, ads7830_read_se(0x48, 255, 0x0C));
    TT_ASSERT_EQ(-EINVAL, ads7830_read_se(0x48, 0, 0x01));      /* stray pd bits */
    TT_ASSERT_EQ(-EINVAL, ads7830_read_se(0x48, 0, 0x10));
    TT_ASSERT_EQ(-EINVAL, ads7830_read_se(0x47, 0, 0x0C));      /* 10010 A1A0 = 0x48..0x4B */
    TT_ASSERT_EQ(-EINVAL, ads7830_read_se(0x4C, 0, 0x0C));
    TT_ASSERT_EQ(0, mock_log_n);
}

static void test_never_positive_on_error(void)
{
    static const int errs[] = { -EIO, -ETIMEDOUT, -EINVAL };
    unsigned i;
    for (i = 0; i < sizeof errs / sizeof errs[0]; i++) {
        mock_reset();
        mock_i2c_fail_next(errs[i]);
        TT_ASSERT(ads7830_read_se(0x48, 0, 0x0C) < 0);
    }
}

int main(void)
{
    TT_RUN(test_command_byte_all_channels);
    TT_RUN(test_read_sequence);
    TT_RUN(test_ff_is_data_not_error);
    TT_RUN(test_zero_is_data);
    TT_RUN(test_write_error_no_read);
    TT_RUN(test_read_error);
    TT_RUN(test_bad_arguments_no_traffic);
    TT_RUN(test_never_positive_on_error);
    return TT_RESULT();
}
