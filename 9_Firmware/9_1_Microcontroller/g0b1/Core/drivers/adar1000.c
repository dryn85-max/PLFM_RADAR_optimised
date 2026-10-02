/* ADAR1000 register layer. C port of the register part of upstream
 * ADAR1000_Manager.cpp; see adar1000.h for the contract.
 *
 * Datasheet citations: "DS" = ADAR1000 Rev. B, "ADTR" = ADTR1107 Rev. C
 * (hardware/datasheets/).
 */
#include <errno.h>
#include "adar1000.h"
#include "hal_gpio.h"
#include "hal_spi.h"
#include "hal_time.h"

/* ---- Vector modulator tables ------------------------------------------- */
// Ported verbatim from 9_1_1_C_Cpp_Libraries/ADAR1000_Manager.cpp (do not edit values).
// ADAR1000 Vector Modulator lookup tables (128-state phase grid, 2.8125 deg step).
//
// Source: Analog Devices ADAR1000 datasheet Rev. B, Tables 13-16, page 34
//   [port note: in the Rev. B PDF in this repo the phase tables are Tables 10-13,
//    pp. 35-37; the values below were re-spot-checked against them]
//   (hardware/datasheets/ADAR1000.pdf)
// Cross-checked against the ADI Linux mainline driver (GPL-2.0, NOT vendored):
//   https://git.kernel.org/pub/scm/linux/kernel/git/torvalds/linux.git/tree/
//     drivers/iio/beamformer/adar1000.c  (adar1000_phase_values[])
// The 128 byte values themselves are factual data from the datasheet and are
// not subject to copyright; only the ADI driver code is GPL.
//
// Byte format (per datasheet):
//   bit  [7:6] reserved (0)
//   bit  [5]   polarity:  1 = positive lobe (sign(I) or sign(Q) >= 0)
//                          0 = negative lobe
//   bits [4:0] 5-bit unsigned magnitude (0..31)
// At magnitude=0 the polarity bit is physically meaningless; the datasheet
// uses POL=1 (e.g. VM_Q at 0 deg = 0x20, VM_I at 90 deg = 0x21).
//
// Index mapping is uniform: VM_I[k] / VM_Q[k] correspond to phase angle
// k * 360/128 = k * 2.8125 degrees.  Callers index as VM_*[phase % 128].
const uint8_t VM_I[128] = {
    0x3F, 0x3F, 0x3F, 0x3F, 0x3F, 0x3E, 0x3E, 0x3D,  // [  0]   0.0000 deg
    0x3D, 0x3C, 0x3C, 0x3B, 0x3A, 0x39, 0x38, 0x37,  // [  8]  22.5000 deg
    0x36, 0x35, 0x34, 0x33, 0x32, 0x30, 0x2F, 0x2E,  // [ 16]  45.0000 deg
    0x2C, 0x2B, 0x2A, 0x28, 0x27, 0x25, 0x24, 0x22,  // [ 24]  67.5000 deg
    0x21, 0x01, 0x03, 0x04, 0x06, 0x07, 0x08, 0x0A,  // [ 32]  90.0000 deg
    0x0B, 0x0D, 0x0E, 0x0F, 0x11, 0x12, 0x13, 0x14,  // [ 40] 112.5000 deg
    0x16, 0x17, 0x18, 0x19, 0x19, 0x1A, 0x1B, 0x1C,  // [ 48] 135.0000 deg
    0x1C, 0x1D, 0x1E, 0x1E, 0x1E, 0x1F, 0x1F, 0x1F,  // [ 56] 157.5000 deg
    0x1F, 0x1F, 0x1F, 0x1F, 0x1F, 0x1E, 0x1E, 0x1D,  // [ 64] 180.0000 deg
    0x1D, 0x1C, 0x1C, 0x1B, 0x1A, 0x19, 0x18, 0x17,  // [ 72] 202.5000 deg
    0x16, 0x15, 0x14, 0x13, 0x12, 0x10, 0x0F, 0x0E,  // [ 80] 225.0000 deg
    0x0C, 0x0B, 0x0A, 0x08, 0x07, 0x05, 0x04, 0x02,  // [ 88] 247.5000 deg
    0x01, 0x21, 0x23, 0x24, 0x26, 0x27, 0x28, 0x2A,  // [ 96] 270.0000 deg
    0x2B, 0x2D, 0x2E, 0x2F, 0x31, 0x32, 0x33, 0x34,  // [104] 292.5000 deg
    0x36, 0x37, 0x38, 0x39, 0x39, 0x3A, 0x3B, 0x3C,  // [112] 315.0000 deg
    0x3C, 0x3D, 0x3E, 0x3E, 0x3E, 0x3F, 0x3F, 0x3F,  // [120] 337.5000 deg
};

const uint8_t VM_Q[128] = {
    0x20, 0x21, 0x23, 0x24, 0x26, 0x27, 0x28, 0x2A,  // [  0]   0.0000 deg
    0x2B, 0x2D, 0x2E, 0x2F, 0x30, 0x31, 0x33, 0x34,  // [  8]  22.5000 deg
    0x35, 0x36, 0x37, 0x38, 0x38, 0x39, 0x3A, 0x3A,  // [ 16]  45.0000 deg
    0x3B, 0x3C, 0x3C, 0x3C, 0x3D, 0x3D, 0x3D, 0x3D,  // [ 24]  67.5000 deg
    0x3D, 0x3D, 0x3D, 0x3D, 0x3D, 0x3C, 0x3C, 0x3C,  // [ 32]  90.0000 deg
    0x3B, 0x3A, 0x3A, 0x39, 0x38, 0x38, 0x37, 0x36,  // [ 40] 112.5000 deg
    0x35, 0x34, 0x33, 0x31, 0x30, 0x2F, 0x2E, 0x2D,  // [ 48] 135.0000 deg
    0x2B, 0x2A, 0x28, 0x27, 0x26, 0x24, 0x23, 0x21,  // [ 56] 157.5000 deg
    0x20, 0x01, 0x03, 0x04, 0x06, 0x07, 0x08, 0x0A,  // [ 64] 180.0000 deg
    0x0B, 0x0D, 0x0E, 0x0F, 0x10, 0x11, 0x13, 0x14,  // [ 72] 202.5000 deg
    0x15, 0x16, 0x17, 0x18, 0x18, 0x19, 0x1A, 0x1A,  // [ 80] 225.0000 deg
    0x1B, 0x1C, 0x1C, 0x1C, 0x1D, 0x1D, 0x1D, 0x1D,  // [ 88] 247.5000 deg
    0x1D, 0x1D, 0x1D, 0x1D, 0x1D, 0x1C, 0x1C, 0x1C,  // [ 96] 270.0000 deg
    0x1B, 0x1A, 0x1A, 0x19, 0x18, 0x18, 0x17, 0x16,  // [104] 292.5000 deg
    0x15, 0x14, 0x13, 0x11, 0x10, 0x0F, 0x0E, 0x0D,  // [112] 315.0000 deg
    0x0B, 0x0A, 0x08, 0x07, 0x06, 0x04, 0x03, 0x01,  // [120] 337.5000 deg
};

/* ---- Internals ---------------------------------------------------------- */

#define ADAR_ADC_TIMEOUT_US   100000u   /* 100 ms */
#define ADAR_ADC_POLL_US      200u
#define ADAR_RESET_WAIT_MS    10u

static int dev_ok(uint8_t dev) { return dev < ADAR_COUNT; }

static adar_mode_t s_mode[ADAR_COUNT];   /* valid only where s_mode_valid[] is set */
static uint8_t     s_mode_valid[ADAR_COUNT];

adar_mode_t adar_get_mode(uint8_t dev)
{
    return (dev_ok(dev) && s_mode_valid[dev]) ? s_mode[dev] : ADAR_MODE_NONE;
}

static void forget_modes(uint8_t dev)
{
    uint8_t i;
    for (i = 0; i < ADAR_COUNT; i++) {
        if (dev == 0 || i == dev) {
            s_mode_valid[i] = 0;
        }
    }
}

#ifdef HOST_TEST
void adar_test_reset_state(void) { forget_modes(0); }
#endif

static gpio_t cs_pin(uint8_t dev)
{
    return (gpio_t)((int)PIN_ADAR_CS0 + (int)dev);
}

/* Write a list of {reg, val} pairs, stopping at the first error. */
typedef struct { uint16_t reg; uint8_t val; } reg_val_t;

static int write_list(uint8_t dev, const reg_val_t *l, unsigned n)
{
    unsigned i;
    for (i = 0; i < n; i++) {
        int rc = adar_write(dev, l[i].reg, l[i].val);
        if (rc != 0) {
            return rc;
        }
    }
    return 0;
}

/* ---- Raw access --------------------------------------------------------- */

/* SPI instruction (DS "SPI general operation", Fig. 2-4, p. 9-10; address
 * decoding p. 34 Table 8): 24 bits = R/W(1) chip_id[14:13](2) addr[12:0](13)
 * then 8 data bits. R/W = 0 for write. Broadcast ("write all", A[14:11]=0001,
 * DS p. 10) is deliberately not exposed: it is not needed with CS per chip. */
int adar_write(uint8_t dev, uint16_t reg, uint8_t val)
{
    uint8_t tx[3];
    if (!dev_ok(dev) || reg > 0x1FFFu) {
        return -EINVAL;
    }
    tx[0] = (uint8_t)(((dev & 0x03u) << 5) | ((reg >> 8) & 0x1Fu));
    tx[1] = (uint8_t)(reg & 0xFFu);
    tx[2] = val;
    return spi_xfer(SPI_BUS_ADAR, cs_pin(dev), tx, NULL, sizeof tx);
}

/* Readback: SDO is enabled only on the addressed chip for the duration of the
 * read (DS "SDO Readback Problem and Solution", p. 47-48, Tables 28/29: with
 * several chips sharing MISO, SDO of the others must stay disabled). */
int adar_read(uint8_t dev, uint16_t reg, uint8_t *val)
{
    uint8_t tx[3], rx[3] = {0, 0, 0};
    int rc, rc2;
    if (!dev_ok(dev) || val == NULL || reg > 0x1FFFu) {
        return -EINVAL;
    }
    rc = adar_write(dev, REG_INTERFACE_CONFIG_A, INTERFACE_CONFIG_A_SDO_ACTIVE);
    if (rc != 0) {
        return rc;
    }
    tx[0] = (uint8_t)(0x80u | ((dev & 0x03u) << 5) | ((reg >> 8) & 0x1Fu));
    tx[1] = (uint8_t)(reg & 0xFFu);
    tx[2] = 0;
    rc = spi_xfer(SPI_BUS_ADAR, cs_pin(dev), tx, rx, sizeof tx);
    rc2 = adar_write(dev, REG_INTERFACE_CONFIG_A, 0);   /* SDO inactive again */
    if (rc != 0) {
        return rc;
    }
    if (rc2 != 0) {
        return rc2;
    }
    *val = rx[2];
    return 0;
}

/* ---- Init --------------------------------------------------------------- */

int adar_init(uint8_t dev)
{
    reg_val_t seq[] = {
        { REG_INTERFACE_CONFIG_A, INTERFACE_CONFIG_A_SDO_ACTIVE },
        { REG_MEM_CTL, MEM_CTRL_BIAS_RAM_BYPASS | MEM_CTRL_BEAM_RAM_BYPASS },
        { REG_ADC_CONTROL, ADAR1000_ADC_2MHZ_CLK | ADAR1000_ADC_EN },
        { REG_SCRATCHPAD, 0xA5 },
    };
    uint8_t rb = 0;
    int rc;

    if (!dev_ok(dev)) {
        return -EINVAL;
    }
    forget_modes(dev);   /* a reset of chip 0 resets all chips (p. 47) */
    /* Soft reset (DS Table 33 p. 57: bit7 SOFTRESET, bit0 SOFTRESET_; 0x81).
     * Only effective on chip 0, where it resets every chip on the bus (p. 47). */
    rc = adar_write(dev, REG_INTERFACE_CONFIG_A, INTERFACE_CONFIG_A_SOFT_RESET);
    if (rc != 0) {
        return rc;
    }
    delay_ms(ADAR_RESET_WAIT_MS);
    /* ConfigA 0x18 = SDO active (Table 33). 0x038 bits 6,5 = beam/bias RAM
     * bypass: use register values (Table 83 p. 72). 0x032 = ADC_EN|CLK_EN,
     * 2 MHz clock (Table 77 p. 70). */
    rc = write_list(dev, seq, sizeof seq / sizeof seq[0]);
    if (rc != 0) {
        return rc;
    }
    delay_ms(1);
    rc = adar_read(dev, REG_SCRATCHPAD, &rb);
    if (rc != 0) {
        return rc;
    }
    /* Upstream ignored a mismatch and marked the device initialised (defect). */
    return (rb == 0xA5) ? 0 : -EIO;
}

/* ---- Bias --------------------------------------------------------------- */

int adar_set_safe_bias(uint8_t dev)
{
    reg_val_t seq[] = {
        { REG_PA_CH1_BIAS_ON + 0, kPaBiasTxSafe },
        { REG_PA_CH1_BIAS_ON + 1, kPaBiasTxSafe },
        { REG_PA_CH1_BIAS_ON + 2, kPaBiasTxSafe },
        { REG_PA_CH1_BIAS_ON + 3, kPaBiasTxSafe },
        { REG_LNA_BIAS_ON, kLnaBiasOperational },   /* 0 V */
        { REG_LNA_BIAS_OFF, kLnaBiasOff },
        /* BIAS_CTRL = 0: DACs always use the ON values (DS Table 16 p. 39,
         * Table 18 p. 40), so PA gate sits at the safe level in RX and TX.
         * LNA_BIAS_OUT_EN = 1 drives VGG_LNA to 0 V (ADTR step 5, p. 24). */
        { REG_MISC_ENABLES, MISC_LNA_BIAS_OUT_EN },
        { REG_LOAD_WORKING, LD_WRK_REGS_LDTX_OVERRIDE },
    };
    if (!dev_ok(dev)) {
        return -EINVAL;
    }
    return write_list(dev, seq, sizeof seq / sizeof seq[0]);
}

int adar_set_operational_bias(uint8_t dev)
{
    reg_val_t seq[] = {
        { REG_PA_CH1_BIAS_ON + 0, kPaBiasOperational },
        { REG_PA_CH1_BIAS_ON + 1, kPaBiasOperational },
        { REG_PA_CH1_BIAS_ON + 2, kPaBiasOperational },
        { REG_PA_CH1_BIAS_ON + 3, kPaBiasOperational },
        { REG_PA_CH1_BIAS_OFF + 0, kPaBiasRxSafe },
        { REG_PA_CH1_BIAS_OFF + 1, kPaBiasRxSafe },
        { REG_PA_CH1_BIAS_OFF + 2, kPaBiasRxSafe },
        { REG_PA_CH1_BIAS_OFF + 3, kPaBiasRxSafe },
        { REG_LNA_BIAS_ON, kLnaBiasOperational },
        { REG_LNA_BIAS_OFF, kLnaBiasOff },
        { REG_BIAS_CURRENT_TX, kTxBiasCurrent },        /* [6:3] VGA, [2:0] VM (Table 81) */
        { REG_BIAS_CURRENT_TX_DRV, kTxDriverBiasCurrent }, /* [2:0] (Table 82) */
        { REG_LOAD_WORKING, LD_WRK_REGS_LDTX_OVERRIDE },
        /* BIAS_CTRL (bit 6) makes the bias DACs follow TR / TX_EN and pick the
         * ON/OFF values (DS Tables 15-18 p. 39-40, setup examples Tables 19/21
         * p. 41); LNA_BIAS_OUT_EN (bit 4) connects the LNA bias DAC (Table 75
         * p. 69). BIAS_EN (bit 5) must stay 0: "0 = enabled". Written last so
         * the new ON/OFF values are in place before the DACs start switching.
         * In TR-pin mode the PA_ON pin must also be high (p. 39). */
        { REG_MISC_ENABLES, MISC_BIAS_CTRL | MISC_LNA_BIAS_OUT_EN },
    };
    if (!dev_ok(dev)) {
        return -EINVAL;
    }
    return write_list(dev, seq, sizeof seq / sizeof seq[0]);
}

/* ---- Gain and phase ----------------------------------------------------- */

int adar_set_rx_gain(uint8_t dev, uint8_t ch, uint8_t gain)
{
    int rc;
    if (!dev_ok(dev) || ch > 3) {
        return -EINVAL;
    }
    rc = adar_write(dev, (uint16_t)(REG_CH1_RX_GAIN + ch), gain);
    if (rc != 0) {
        return rc;
    }
    return adar_write(dev, REG_LOAD_WORKING, LD_WRK_REGS_LDRX_OVERRIDE);
}

int adar_set_tx_gain(uint8_t dev, uint8_t ch, uint8_t gain)
{
    int rc;
    if (!dev_ok(dev) || ch > 3) {
        return -EINVAL;
    }
    rc = adar_write(dev, (uint16_t)(REG_CH1_TX_GAIN + ch), gain);
    if (rc != 0) {
        return rc;
    }
    return adar_write(dev, REG_LOAD_WORKING, LD_WRK_REGS_LDTX_OVERRIDE);
}

static int set_phase(uint8_t dev, uint8_t ch, uint8_t idx, uint16_t base_i, uint8_t load)
{
    int rc;
    uint16_t reg_i;
    if (!dev_ok(dev) || ch > 3) {
        return -EINVAL;
    }
    idx &= 0x7Fu;   /* index modulo 128, as upstream */
    reg_i = (uint16_t)(base_i + 2u * ch);
    rc = adar_write(dev, reg_i, VM_I[idx]);
    if (rc != 0) {
        return rc;
    }
    rc = adar_write(dev, (uint16_t)(reg_i + 1u), VM_Q[idx]);
    if (rc != 0) {
        return rc;
    }
    return adar_write(dev, REG_LOAD_WORKING, load);
}

int adar_set_rx_phase(uint8_t dev, uint8_t ch, uint8_t idx)
{
    return set_phase(dev, ch, idx, REG_CH1_RX_PHS_I, LD_WRK_REGS_LDRX_OVERRIDE);
}

/* Upstream adarSetTxPhase loaded the RX working registers (0x01) instead of TX
 * (defect); fixed here. */
int adar_set_tx_phase(uint8_t dev, uint8_t ch, uint8_t idx)
{
    return set_phase(dev, ch, idx, REG_CH1_TX_PHS_I, LD_WRK_REGS_LDTX_OVERRIDE);
}

/* ---- TX/RX mode --------------------------------------------------------- */

/* SW_DRV_TR_STATE (0x031 bit 7) is set in every mode. With SW_DRV_TR_MODE_SEL
 * = 0 the TR_SW_POS driver outputs 3.3 V in receive and 0 V in transmit when
 * the bit is 1 (DS Table 14 p. 38, Table 24 p. 42, TR_SW_POS setup p. 45).
 * ADTR1107 CTRL_SW: low = transmit, high = receive (ADTR Table 8, p. 6; low =
 * 0 V, high = 3.3 V, p. 24). So bit 7 = 1 makes TR_SW_POS -> CTRL_SW correct.
 * SW_DRV_EN_POL and POL stay 0 (the polarization switch is not used). */
#define SW_BASE  (SW_CTRL_SW_DRV_TR_STATE | SW_CTRL_SW_DRV_EN_TR)

static int set_mode_hw(uint8_t dev, adar_mode_t m)
{
    if (!dev_ok(dev)) {
        return -EINVAL;
    }
    switch (m) {
    case ADAR_MODE_TR_PIN: {
        /* TR_SOURCE = 1: the TR pin selects TX/RX and follows TX_EN/RX_EN
         * automatically (DS p. 30, p. 37 "TR Pin Control"). All sub-circuit
         * enables on (0x7F, p. 37). TR_SPI / TX_EN / RX_EN are don't-care. */
        reg_val_t seq[] = {
            { REG_RX_ENABLES, ADAR_ENABLES_ALL },
            { REG_TX_ENABLES, ADAR_ENABLES_ALL },
            { REG_SW_CONTROL, SW_BASE | SW_CTRL_TR_SOURCE },
        };
        return write_list(dev, seq, sizeof seq / sizeof seq[0]);
    }
    case ADAR_MODE_SPI_TX:
        /* TR_SOURCE = 0, TR_SPI = 1 (transmit), TX_EN = 1, RX_EN = 0 (p. 30:
         * both high would power both paths down). */
        return adar_write(dev, REG_SW_CONTROL,
                          SW_BASE | SW_CTRL_TX_EN | SW_CTRL_TR_SPI);
    case ADAR_MODE_SPI_RX:
        /* TR_SOURCE = 0, TR_SPI = 0 (receive), RX_EN = 1, TX_EN = 0. */
        return adar_write(dev, REG_SW_CONTROL, SW_BASE | SW_CTRL_RX_EN);
    default:
        return -EINVAL;
    }
}

int adar_set_mode(uint8_t dev, adar_mode_t m)
{
    int rc = set_mode_hw(dev, m);
    if (rc == 0) {
        s_mode[dev] = m;
        s_mode_valid[dev] = 1;
    }
    return rc;
}

/* ---- Temperature -------------------------------------------------------- */

/* DS p. 30 ADC conversion cycle and Table 77 (p. 70): write ST_CONV (MUX_SEL =
 * 0, temperature), wait for ADC_EOC (bit 0) then read 0x033. Upstream polled
 * with no real bound on a stuck ADC; this has a 100 ms timeout. */
int adar_read_temp_raw(uint8_t dev, uint8_t *raw)
{
    uint32_t t0;
    uint8_t st = 0, v = 0;
    int rc;

    if (!dev_ok(dev) || raw == NULL) {
        return -EINVAL;
    }
    rc = adar_write(dev, REG_ADC_CONTROL, ADAR1000_ADC_ST_CONV);
    if (rc != 0) {
        return rc;
    }
    t0 = micros();
    for (;;) {
        rc = adar_read(dev, REG_ADC_CONTROL, &st);
        if (rc != 0) {
            return rc;
        }
        if ((st & ADAR1000_ADC_EOC) != 0) {
            break;
        }
        if ((uint32_t)(micros() - t0) >= ADAR_ADC_TIMEOUT_US) {
            return -ETIMEDOUT;
        }
        delay_us(ADAR_ADC_POLL_US);
    }
    rc = adar_read(dev, REG_ADC_OUT, &v);
    if (rc != 0) {
        return rc;
    }
    *raw = v;
    return 0;
}
