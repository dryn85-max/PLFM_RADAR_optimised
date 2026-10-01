/* Host tests for the ADAR1000 register layer (Core/drivers/adar1000.c).
 * Expected register/bit values were checked against ADAR1000 Rev. B and
 * ADTR1107 Rev. C (see citations in adar1000.c). */
#include <errno.h>
#include <string.h>
#include "tinytest.h"
#include "mock_log.h"
#include "adar1000.h"
#include "hal_gpio.h"
#include "hal_spi.h"
#include "hal_time.h"
#include "config.h"

/* ---- helpers --------------------------------------------------------- */

static int spi_count(void)
{
    int i, n = 0;
    for (i = 0; i < mock_log_n; i++) {
        if (mock_log[i].kind == MOCK_EV_SPI) n++;
    }
    return n;
}

/* k-th SPI event (0-based) */
static const mock_event_t *spi_ev(int k)
{
    int i;
    for (i = 0; i < mock_log_n; i++) {
        if (mock_log[i].kind == MOCK_EV_SPI && k-- == 0) return &mock_log[i];
    }
    return NULL;
}

/* Assert that SPI event k is a 3-byte write {dev, reg, val}. */
static void expect_write(int k, int dev, unsigned reg, unsigned val)
{
    const mock_event_t *e = spi_ev(k);
    TT_ASSERT(e != NULL);
    if (e == NULL) return;
    TT_ASSERT_EQ(3, e->n);
    TT_ASSERT_EQ(PIN_ADAR_CS0 + dev, e->b);
    TT_ASSERT_EQ(SPI_BUS_ADAR, e->a);
    TT_ASSERT_EQ(((dev & 3) << 5) | ((reg >> 8) & 0x1F), e->bytes[0]);
    TT_ASSERT_EQ(reg & 0xFF, e->bytes[1]);
    TT_ASSERT_EQ(val, e->bytes[2]);
}

/* Queue MISO bytes for one adar_read(): the SDO-enable write consumes 3,
 * the read frame returns {0,0,val}, the SDO-disable write consumes 3. */
static void queue_read(uint8_t val)
{
    uint8_t b[9] = {0, 0, 0, 0, 0, 0, 0, 0, 0};
    b[5] = val;
    mock_spi_set_rx(b, sizeof b);
}

static void queue_pad(void)   /* one write frame worth of MISO */
{
    uint8_t z[3] = {0, 0, 0};
    mock_spi_set_rx(z, sizeof z);
}

/* ---- tests ----------------------------------------------------------- */

static void test_write_encoding_dev0(void)
{
    mock_reset();
    TT_ASSERT_EQ(0, adar_write(0, 0x0FF, 0xAB));
    TT_ASSERT_EQ(1, spi_count());
    expect_write(0, 0, 0x0FF, 0xAB);
    TT_ASSERT_EQ(0x00, spi_ev(0)->bytes[0]);   /* write: R/W=0, addr=0, hi=0 */
}

static void test_write_encoding_high_bits(void)
{
    mock_reset();
    TT_ASSERT_EQ(0, adar_write(0, 0x400, 0x11));
    TT_ASSERT_EQ(0x04, spi_ev(0)->bytes[0]);
    TT_ASSERT_EQ(0x00, spi_ev(0)->bytes[1]);
    TT_ASSERT_EQ(0, adar_write(0, 0x1FFF, 0x22));   /* RAM space, bit 12 */
    TT_ASSERT_EQ(0x1F, spi_ev(1)->bytes[0]);
    TT_ASSERT_EQ(0xFF, spi_ev(1)->bytes[1]);
    TT_ASSERT_EQ(-EINVAL, adar_write(0, 0x2000, 0));
    TT_ASSERT_EQ(2, spi_count());
}

#if ADAR_COUNT >= 4
static void test_write_encoding_dev3(void)
{
    mock_reset();
    TT_ASSERT_EQ(0, adar_write(3, 0x030, 0x50));
    expect_write(0, 3, 0x030, 0x50);
    TT_ASSERT_EQ(0x60, spi_ev(0)->bytes[0]);   /* chip id 0b11 in bits 6:5 */
    TT_ASSERT_EQ(0, adar_write(2, 0x400, 0));
    TT_ASSERT_EQ(0x44, spi_ev(1)->bytes[0]);   /* chip 2: 0b10<<5 | 0x04 */
    TT_ASSERT_EQ(PIN_ADAR_CS2, spi_ev(1)->b);
}
#endif

static void test_dev_range_ch_range(void)
{
    uint8_t v = 0;
    mock_reset();
    TT_ASSERT_EQ(-EINVAL, adar_write(ADAR_COUNT, 0x10, 0));
    TT_ASSERT_EQ(-EINVAL, adar_read(ADAR_COUNT, 0x10, &v));
    TT_ASSERT_EQ(-EINVAL, adar_read(0, 0x10, NULL));
    TT_ASSERT_EQ(-EINVAL, adar_init(ADAR_COUNT));
    TT_ASSERT_EQ(-EINVAL, adar_set_safe_bias(ADAR_COUNT));
    TT_ASSERT_EQ(-EINVAL, adar_set_operational_bias(ADAR_COUNT));
    TT_ASSERT_EQ(-EINVAL, adar_set_rx_gain(0, 4, 1));
    TT_ASSERT_EQ(-EINVAL, adar_set_tx_gain(0, 4, 1));
    TT_ASSERT_EQ(-EINVAL, adar_set_rx_phase(0, 4, 1));
    TT_ASSERT_EQ(-EINVAL, adar_set_tx_phase(0, 255, 1));
    TT_ASSERT_EQ(-EINVAL, adar_set_rx_gain(ADAR_COUNT, 0, 1));
    TT_ASSERT_EQ(-EINVAL, adar_set_mode(ADAR_COUNT, ADAR_MODE_TR_PIN));
    TT_ASSERT_EQ(-EINVAL, adar_set_mode(0, (adar_mode_t)3));
    TT_ASSERT_EQ(-EINVAL, adar_read_temp_raw(ADAR_COUNT, &v));
    TT_ASSERT_EQ(-EINVAL, adar_read_temp_raw(0, NULL));
    TT_ASSERT_EQ(0, spi_count());   /* nothing reached the bus */
}

static void test_read_sequence(void)
{
    uint8_t v = 0;
    const mock_event_t *e;
    mock_reset();
    queue_read(0x5A);
    TT_ASSERT_EQ(0, adar_read(0, 0x00A, &v));
    TT_ASSERT_EQ(0x5A, v);
    TT_ASSERT_EQ(3, spi_count());
    expect_write(0, 0, 0x000, 0x18);          /* SDO active (Table 33: bits 4,3) */
    e = spi_ev(1);
    TT_ASSERT_EQ(3, e->n);
    TT_ASSERT_EQ(0x80, e->bytes[0]);          /* R/W=1, chip 0, addr hi 0 */
    TT_ASSERT_EQ(0x0A, e->bytes[1]);
    expect_write(2, 0, 0x000, 0x00);          /* SDO inactive */
}

static void test_read_sequence_high_reg(void)
{
    uint8_t v = 0;
    mock_reset();
    queue_read(0x77);
    TT_ASSERT_EQ(0, adar_read(0, 0x400, &v));
    TT_ASSERT_EQ(0x84, spi_ev(1)->bytes[0]);
    TT_ASSERT_EQ(0x00, spi_ev(1)->bytes[1]);
}

static void test_read_spi_error_still_disables_sdo(void)
{
    uint8_t v = 0xEE;
    mock_reset();
    mock_spi_fail_next(-EIO);    /* the SDO-enable write fails */
    TT_ASSERT_EQ(-EIO, adar_read(0, 0x00A, &v));
    TT_ASSERT_EQ(0xEE, v);       /* value untouched on error */
    TT_ASSERT_EQ(1, spi_count());   /* no read frame after a failed enable */
}

static void test_write_spi_error(void)
{
    mock_reset();
    mock_spi_fail_next(-ETIMEDOUT);
    TT_ASSERT_EQ(-ETIMEDOUT, adar_write(0, 0x10, 1));
}

static void test_channel3_rx(void)
{
    mock_reset();
    TT_ASSERT_EQ(0, adar_set_rx_gain(0, 3, 0x42));
    TT_ASSERT_EQ(2, spi_count());
    expect_write(0, 0, 0x013, 0x42);
    expect_write(1, 0, 0x028, 0x01);          /* LDRX_OVERRIDE, Table 67 bit 0 */

    mock_reset();
    TT_ASSERT_EQ(0, adar_set_rx_phase(0, 3, 40));
    TT_ASSERT_EQ(3, spi_count());
    expect_write(0, 0, 0x01A, VM_I[40]);
    expect_write(1, 0, 0x01B, VM_Q[40]);
    expect_write(2, 0, 0x028, 0x01);
}

static void test_channel_map_all(void)
{
    int ch;
    for (ch = 0; ch < 4; ch++) {
        mock_reset();
        adar_set_rx_gain(0, (uint8_t)ch, 1);
        expect_write(0, 0, 0x010 + ch, 1);
        mock_reset();
        adar_set_tx_gain(0, (uint8_t)ch, 1);
        expect_write(0, 0, 0x01C + ch, 1);
        mock_reset();
        adar_set_rx_phase(0, (uint8_t)ch, 5);
        expect_write(0, 0, 0x014 + 2 * ch, VM_I[5]);
        expect_write(1, 0, 0x015 + 2 * ch, VM_Q[5]);
        mock_reset();
        adar_set_tx_phase(0, (uint8_t)ch, 5);
        expect_write(0, 0, 0x020 + 2 * ch, VM_I[5]);
        expect_write(1, 0, 0x021 + 2 * ch, VM_Q[5]);
    }
}

static void test_channel3_tx(void)
{
    mock_reset();
    TT_ASSERT_EQ(0, adar_set_tx_gain(0, 3, 0x7F));
    TT_ASSERT_EQ(2, spi_count());
    expect_write(0, 0, 0x01F, 0x7F);
    expect_write(1, 0, 0x028, 0x02);          /* LDTX_OVERRIDE, Table 67 bit 1 */

    mock_reset();
    TT_ASSERT_EQ(0, adar_set_tx_phase(0, 3, 127));
    TT_ASSERT_EQ(3, spi_count());
    expect_write(0, 0, 0x026, VM_I[127]);
    expect_write(1, 0, 0x027, VM_Q[127]);
    expect_write(2, 0, 0x028, 0x02);   /* upstream wrongly loaded RX (0x01) */
}

static void test_phase_index_wraps(void)
{
    mock_reset();
    TT_ASSERT_EQ(0, adar_set_rx_phase(0, 0, 130));   /* 130 % 128 = 2 */
    expect_write(0, 0, 0x014, VM_I[2]);
    expect_write(1, 0, 0x015, VM_Q[2]);
}

static void test_vm_tables_verbatim(void)
{
    /* spot values straight from datasheet Rev. B Tables 10-13 (p. 35-37) */
    TT_ASSERT_EQ(0x3F, VM_I[0]);   TT_ASSERT_EQ(0x20, VM_Q[0]);     /* 0 deg   */
    TT_ASSERT_EQ(0x21, VM_I[32]);  TT_ASSERT_EQ(0x3D, VM_Q[32]);    /* 90 deg  */
    TT_ASSERT_EQ(0x1F, VM_I[64]);  TT_ASSERT_EQ(0x20, VM_Q[64]);    /* 180 deg */
    TT_ASSERT_EQ(0x01, VM_I[96]);  TT_ASSERT_EQ(0x1D, VM_Q[96]);    /* 270 deg */
    TT_ASSERT_EQ(0x3F, VM_I[127]); TT_ASSERT_EQ(0x01, VM_Q[127]);   /* 357.19 */
    TT_ASSERT_EQ(128, (int)(sizeof VM_I));
    TT_ASSERT_EQ(128, (int)(sizeof VM_Q));
}

static void test_mode_tr_pin(void)
{
    mock_reset();
    TT_ASSERT_EQ(0, adar_set_mode(0, ADAR_MODE_TR_PIN));
    TT_ASSERT_EQ(3, spi_count());
    expect_write(0, 0, 0x02E, 0x7F);          /* Table 73: all RX enables */
    expect_write(1, 0, 0x02F, 0x7F);          /* Table 74: all TX enables */
    /* Table 76: SW_DRV_TR_STATE(7) | SW_DRV_EN_TR(4) | TR_SOURCE(2) */
    expect_write(2, 0, 0x031, 0x94);
}

static void test_mode_spi_tx_rx(void)
{
    mock_reset();
    TT_ASSERT_EQ(0, adar_set_mode(0, ADAR_MODE_SPI_TX));
    TT_ASSERT_EQ(1, spi_count());
    /* 0x80 | SW_DRV_EN_TR 0x10 | TX_EN 0x40 | TR_SPI 0x02 */
    expect_write(0, 0, 0x031, 0xD2);

    mock_reset();
    TT_ASSERT_EQ(0, adar_set_mode(0, ADAR_MODE_SPI_RX));
    TT_ASSERT_EQ(1, spi_count());
    /* 0x80 | SW_DRV_EN_TR 0x10 | RX_EN 0x20; TR_SPI=0, TR_SOURCE=0 */
    expect_write(0, 0, 0x031, 0xB0);
}

static void test_mode_never_sets_both_en_and_tr_source(void)
{
    int m;
    for (m = 0; m < 3; m++) {
        const mock_event_t *e;
        uint8_t v;
        mock_reset();
        adar_set_mode(0, (adar_mode_t)m);
        e = spi_ev(spi_count() - 1);
        v = e->bytes[2];
        TT_ASSERT_EQ(0x031, ((e->bytes[0] & 0x1F) << 8) | e->bytes[1]);
        /* datasheet p.30: TX_EN and RX_EN together power both paths down */
        TT_ASSERT(!((v & 0x40) && (v & 0x20)));
        TT_ASSERT(v & 0x10);               /* switch driver always enabled */
        TT_ASSERT(v & 0x80);               /* ADTR1107: TX = CTRL_SW low */
        TT_ASSERT_EQ(0, v & 0x09);         /* POL and SW_DRV_EN_POL never set */
    }
}

static void test_auto_callable_any_time(void)
{
    mock_reset();
    TT_ASSERT_EQ(0, adar_set_mode(0, ADAR_MODE_SPI_TX));
    TT_ASSERT_EQ(0, adar_set_mode(0, ADAR_MODE_SPI_RX));
    TT_ASSERT_EQ(0, adar_set_mode(0, ADAR_MODE_TR_PIN));
    TT_ASSERT_EQ(5, spi_count());
    expect_write(4, 0, 0x031, 0x94);
    TT_ASSERT_EQ(0, adar_set_mode(0, ADAR_MODE_TR_PIN));   /* idempotent */
    TT_ASSERT_EQ(8, spi_count());
}

static void test_init_sequence(void)
{
    mock_reset();
    queue_pad();                 /* soft reset frame */
    queue_pad(); queue_pad(); queue_pad();   /* ConfigA, mem ctl, adc */
    queue_pad();                 /* scratchpad write */
    queue_read(0xA5);
    TT_ASSERT_EQ(0, adar_init(0));
    expect_write(0, 0, 0x000, 0x81);     /* SOFTRESET | SOFTRESET_ (Table 33) */
    expect_write(1, 0, 0x000, 0x18);     /* SDO active */
    expect_write(2, 0, 0x038, 0x60);     /* BEAM_RAM_BYPASS | BIAS_RAM_BYPASS */
    expect_write(3, 0, 0x032, 0x60);     /* 2 MHz clk, ADC_EN, CLK_EN */
    expect_write(4, 0, 0x00A, 0xA5);     /* scratchpad */
    expect_write(5, 0, 0x000, 0x18);     /* read: SDO on */
    TT_ASSERT_EQ(0x80, spi_ev(6)->bytes[0]);
    TT_ASSERT_EQ(0x0A, spi_ev(6)->bytes[1]);
    expect_write(7, 0, 0x000, 0x00);
    TT_ASSERT_EQ(8, spi_count());
    /* >= 10 ms between soft reset and the next register write */
    {
        int i, saw_reset = 0, delay_ok = 0;
        for (i = 0; i < mock_log_n; i++) {
            if (mock_log[i].kind == MOCK_EV_SPI && mock_log[i].bytes[2] == 0x81) saw_reset = 1;
            else if (saw_reset && mock_log[i].kind == MOCK_EV_DELAY_MS && mock_log[i].a >= 10) delay_ok = 1;
            else if (saw_reset && mock_log[i].kind == MOCK_EV_SPI) break;
        }
        TT_ASSERT(delay_ok);
    }
}

static void test_init_scratchpad_mismatch(void)
{
    mock_reset();
    queue_pad(); queue_pad(); queue_pad(); queue_pad(); queue_pad();
    queue_read(0x00);            /* reads back 0 instead of 0xA5 */
    TT_ASSERT_EQ(-EIO, adar_init(0));
}

static void test_init_spi_error_propagates(void)
{
    mock_reset();
    mock_spi_fail_next(-ETIMEDOUT);
    TT_ASSERT_EQ(-ETIMEDOUT, adar_init(0));
    TT_ASSERT_EQ(1, spi_count());        /* aborts at the first failure */
}

static void test_safe_bias(void)
{
    int ch;
    mock_reset();
    TT_ASSERT_EQ(0, adar_set_safe_bias(0));
    for (ch = 0; ch < 4; ch++) expect_write(ch, 0, 0x029 + ch, 0x5D);   /* PA ON */
    expect_write(4, 0, 0x02D, 0x00);                                     /* LNA ON */
    expect_write(5, 0, 0x04A, 0x00);                                     /* LNA OFF */
    /* BIAS_CTRL=0 (ON values always used), LNA_BIAS_OUT_EN=1 (Table 75) */
    expect_write(6, 0, 0x030, 0x10);
    expect_write(7, 0, 0x028, 0x02);
    TT_ASSERT_EQ(8, spi_count());
}

static void test_operational_bias(void)
{
    int ch;
    mock_reset();
    TT_ASSERT_EQ(0, adar_set_operational_bias(0));
    for (ch = 0; ch < 4; ch++) {
        expect_write(ch, 0, 0x029 + ch, 0x7F);        /* PA ON  (kPaBiasOperational) */
        expect_write(4 + ch, 0, 0x046 + ch, 0x20);    /* PA OFF (kPaBiasRxSafe) */
    }
    expect_write(8, 0, 0x02D, 0x30);                  /* LNA ON */
    expect_write(9, 0, 0x04A, 0x00);                  /* LNA OFF */
    expect_write(10, 0, 0x036, 0x2D);
    expect_write(11, 0, 0x037, 0x06);
    expect_write(12, 0, 0x028, 0x02);
    /* BIAS_CTRL(6) | LNA_BIAS_OUT_EN(4); BIAS_EN(5) stays 0 = enabled */
    expect_write(13, 0, 0x030, 0x50);
    TT_ASSERT_EQ(14, spi_count());
}

static void test_operational_bias_enables_last(void)
{
    /* MISC_ENABLES (bias follows TR) must be the last write so the new
     * ON/OFF values are in place before the DACs start switching. */
    mock_reset();
    adar_set_operational_bias(0);
    TT_ASSERT_EQ(0x30, spi_ev(spi_count() - 1)->bytes[1]);
}

static void test_temp_read_ok(void)
{
    uint8_t raw = 0;
    mock_reset();
    queue_pad();             /* ST_CONV write frame */
    queue_read(0x01);        /* EOC already set at first poll */
    queue_read(0x6B);        /* ADC_OUT */
    TT_ASSERT_EQ(0, adar_read_temp_raw(0, &raw));
    TT_ASSERT_EQ(0x6B, raw);
    expect_write(0, 0, 0x032, 0x70);     /* ADC_EN | CLK_EN | ST_CONV (Table 77) */
    TT_ASSERT_EQ(0x80, spi_ev(2)->bytes[0]);
    TT_ASSERT_EQ(0x32, spi_ev(2)->bytes[1]);
    TT_ASSERT_EQ(0x80, spi_ev(5)->bytes[0]);
    TT_ASSERT_EQ(0x33, spi_ev(5)->bytes[1]);
    TT_ASSERT_EQ(7, spi_count());        /* 1 + 3 + 3 frames */
}

static void test_temp_read_polls_until_eoc(void)
{
    uint8_t raw = 0;
    mock_reset();
    queue_pad();
    queue_read(0x00);        /* converting */
    queue_read(0x00);
    queue_read(0xFE);        /* bit 0 clear in a byte with other bits set */
    queue_read(0x81);        /* done (other bits set too) */
    queue_read(0x55);
    TT_ASSERT_EQ(0, adar_read_temp_raw(0, &raw));
    TT_ASSERT_EQ(0x55, raw);
}

static void test_temp_read_timeout(void)
{
    uint8_t raw = 0x33;
    uint32_t t0, dt;
    mock_reset();
    t0 = micros();
    TT_ASSERT_EQ(-ETIMEDOUT, adar_read_temp_raw(0, &raw));   /* MISO stays 0 */
    dt = micros() - t0;
    TT_ASSERT(dt >= 100000u);            /* waited the full 100 ms of mock time */
    TT_ASSERT(dt < 110000u);             /* ... and did not overshoot wildly */
    TT_ASSERT_EQ(0x33, raw);
}

static void test_temp_read_timeout_wraparound(void)
{
    uint8_t raw = 0;
    mock_reset();
    mock_time_advance_us(0xFFFFFFFFu - 50000u);   /* micros() wraps mid-wait */
    TT_ASSERT_EQ(-ETIMEDOUT, adar_read_temp_raw(0, &raw));
}

static void test_temp_read_spi_error(void)
{
    uint8_t raw = 0;
    mock_reset();
    mock_spi_fail_next(-EIO);
    TT_ASSERT_EQ(-EIO, adar_read_temp_raw(0, &raw));
    TT_ASSERT_EQ(1, spi_count());
}

int main(void)
{
    TT_RUN(test_write_encoding_dev0);
    TT_RUN(test_write_encoding_high_bits);
#if ADAR_COUNT >= 4
    TT_RUN(test_write_encoding_dev3);
#endif
    TT_RUN(test_dev_range_ch_range);
    TT_RUN(test_read_sequence);
    TT_RUN(test_read_sequence_high_reg);
    TT_RUN(test_read_spi_error_still_disables_sdo);
    TT_RUN(test_write_spi_error);
    TT_RUN(test_channel3_rx);
    TT_RUN(test_channel_map_all);
    TT_RUN(test_channel3_tx);
    TT_RUN(test_phase_index_wraps);
    TT_RUN(test_vm_tables_verbatim);
    TT_RUN(test_mode_tr_pin);
    TT_RUN(test_mode_spi_tx_rx);
    TT_RUN(test_mode_never_sets_both_en_and_tr_source);
    TT_RUN(test_auto_callable_any_time);
    TT_RUN(test_init_sequence);
    TT_RUN(test_init_scratchpad_mismatch);
    TT_RUN(test_init_spi_error_propagates);
    TT_RUN(test_safe_bias);
    TT_RUN(test_operational_bias);
    TT_RUN(test_operational_bias_enables_last);
    TT_RUN(test_temp_read_ok);
    TT_RUN(test_temp_read_polls_until_eoc);
    TT_RUN(test_temp_read_timeout);
    TT_RUN(test_temp_read_timeout_wraparound);
    TT_RUN(test_temp_read_spi_error);
    return TT_RESULT();
}
