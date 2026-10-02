/* ADAR1000 4-channel beamformer: register layer (C port of the register part
 * of upstream ADAR1000_Manager.cpp).
 *
 * - Up to ADAR_COUNT devices on one SPI bus (spi_bus ADAR), one CS per device.
 * - Channel index is 0-based (0..3) everywhere (upstream passed 1-based and
 *   masked with & 3, which hit the wrong channel).
 * - The only state kept is the last mode applied (adar_get_mode); every call
 *   is otherwise a short list of SPI writes.
 * - TR switching is NOT done over SPI per pulse: after adar_set_mode(TR_PIN)
 *   the FPGA drives the ADAR1000 TR pin.
 * - All functions return 0 or a negative errno (-EINVAL for dev >= ADAR_COUNT
 *   or ch > 3, no SPI traffic in that case; -EIO/-ETIMEDOUT from the bus).
 *
 * Datasheet references are to ADAR1000 Rev. B and ADTR1107 Rev. C in
 * "7_Components Datasheets and Application notes/".
 */
#ifndef ADAR1000_H
#define ADAR1000_H

#include <stdint.h>
#include "config.h"

/* ---- Register addresses (Register Map, Rev. B pp. 56-72) ---------------- */
#define REG_INTERFACE_CONFIG_A  0x000
#define REG_SCRATCHPAD          0x00A
#define REG_CH1_RX_GAIN         0x010   /* ..0x013 */
#define REG_CH1_RX_PHS_I        0x014   /* I/Q pairs, ..0x01B */
#define REG_CH1_TX_GAIN         0x01C   /* ..0x01F */
#define REG_CH1_TX_PHS_I        0x020   /* I/Q pairs, ..0x027 */
#define REG_LOAD_WORKING        0x028
#define REG_PA_CH1_BIAS_ON      0x029   /* ..0x02C */
#define REG_LNA_BIAS_ON         0x02D
#define REG_RX_ENABLES          0x02E
#define REG_TX_ENABLES          0x02F
#define REG_MISC_ENABLES        0x030
#define REG_SW_CONTROL          0x031
#define REG_ADC_CONTROL         0x032
#define REG_ADC_OUT             0x033
#define REG_BIAS_CURRENT_TX     0x036
#define REG_BIAS_CURRENT_TX_DRV 0x037
#define REG_MEM_CTL             0x038
#define REG_PA_CH1_BIAS_OFF     0x046   /* ..0x049 */
#define REG_LNA_BIAS_OFF        0x04A

/* ---- Register constants ------------------------------------------------- */
/* 0x000 INTERFACE_CONFIG_A (Table 33, p. 57): bit7 SOFTRESET, bit0 SOFTRESET_
 * (palindrome), bit4 SDOACTIVE, bit3 SDOACTIVE_. */
#define INTERFACE_CONFIG_A_SOFT_RESET  0x81
#define INTERFACE_CONFIG_A_SDO_ACTIVE  0x18
/* 0x028 LD_WRK_REGS (Table 67, p. 66): bit0 LDRX_OVERRIDE, bit1 LDTX_OVERRIDE.
 * Never assert both in one write (Theory of Operation, p. 33-34). */
#define LD_WRK_REGS_LDRX_OVERRIDE      (1u << 0)
#define LD_WRK_REGS_LDTX_OVERRIDE      (1u << 1)
/* 0x02E / 0x02F: bits [6:0] = CHx_EN (4), LNA/DRV_EN, VM_EN, VGA_EN
 * (Tables 73/74, p. 68). 0x7F enables everything. */
#define ADAR_ENABLES_ALL               0x7F
/* 0x030 MISC_ENABLES (Table 75, p. 69) */
#define MISC_SW_DRV_TR_MODE_SEL        (1u << 7)  /* 0: TR_SW_POS, 1: TR_SW_NEG */
#define MISC_BIAS_CTRL                 (1u << 6)  /* 1: bias DACs follow TR/TX_EN */
#define MISC_BIAS_EN                   (1u << 5)  /* 0 = bias DACs ENABLED (!) */
#define MISC_LNA_BIAS_OUT_EN           (1u << 4)
/* 0x031 SW_CTRL (Table 76, p. 69-70) */
#define SW_CTRL_SW_DRV_TR_STATE        (1u << 7)
#define SW_CTRL_TX_EN                  (1u << 6)
#define SW_CTRL_RX_EN                  (1u << 5)
#define SW_CTRL_SW_DRV_EN_TR           (1u << 4)
#define SW_CTRL_SW_DRV_EN_POL          (1u << 3)
#define SW_CTRL_TR_SOURCE              (1u << 2)  /* 0: SPI (TR_SPI), 1: TR pin */
#define SW_CTRL_TR_SPI                 (1u << 1)  /* SPI control: 0 = RX, 1 = TX */
#define SW_CTRL_POL                    (1u << 0)
/* 0x032 ADC_CTRL (Table 77, p. 70): bit7 clk sel (0 = 2 MHz), bit6 ADC_EN,
 * bit5 CLK_EN, bit4 ST_CONV (self-clearing), [3:1] MUX_SEL (0 = temperature),
 * bit0 ADC_EOC (read-only, high when done). */
#define ADAR1000_ADC_2MHZ_CLK          0x00
#define ADAR1000_ADC_EN                0x60       /* ADC_EN | CLK_EN */
#define ADAR1000_ADC_ST_CONV           0x70
#define ADAR1000_ADC_EOC               0x01
/* 0x038 MEM_CTRL (Table 83, p. 72) */
#define MEM_CTRL_BIAS_RAM_BYPASS       (1u << 5)
#define MEM_CTRL_BEAM_RAM_BYPASS       (1u << 6)

/* ---- Bias constants (owner decision: SAFE values) ----------------------- */
/* PA_BIASx / LNA_BIAS DAC: 0x00 -> 0 V, 0xFF -> -4.8 V, linear (DS p. 31;
 * 0x5D = -1.75 V, 0x6A = -2.0 V). The ADTR1107 VGG_PA/VGG_LNA pins must never
 * see more than -2.0 V from this firmware, so every PA/LNA bias constant is
 * limited to 0x6A at compile time (see _Static_assert below).
 *
 * Upstream values (PA ON 0x7F = -2.5 V, OFF 0x20, LNA ON 0x30) are NOT used:
 * 0x7F exceeds the limit and the ON/OFF pair looked swapped. PA ON = PA OFF =
 * 0x5D keeps the PA pinched in both TR states (no TX current), so the true
 * quiescent-current (Idq) value is still to be found on hardware (BACKLOG).
 * LNA ON = 0 V (self-biased operation), LNA OFF = 0x68 (-1.96 V, LNA debiased
 * while transmitting). */
#define kDefaultTxVgaGain      0x7F
#define kDefaultRxVgaGain      30
#define kLnaBiasOperational    0x00
#define kLnaBiasOff            0x68
#define kPaBiasTxSafe          0x5D
#define kPaBiasIdqCalibration  0x0D   /* unused: Idq calibration is not implemented */
#define kPaBiasOperational     0x5D
#define kPaBiasRxSafe          0x5D
#define kBiasDacMaxSafe        0x6A
#define kTxBiasCurrent         0x2D
#define kTxDriverBiasCurrent   0x06

_Static_assert(kLnaBiasOperational <= kBiasDacMaxSafe, "LNA ON bias beyond -2.0 V");
_Static_assert(kLnaBiasOff <= kBiasDacMaxSafe, "LNA OFF bias beyond -2.0 V");
_Static_assert(kPaBiasTxSafe <= kBiasDacMaxSafe, "PA safe bias beyond -2.0 V");
_Static_assert(kPaBiasIdqCalibration <= kBiasDacMaxSafe, "PA Idq bias beyond -2.0 V");
_Static_assert(kPaBiasOperational <= kBiasDacMaxSafe, "PA ON bias beyond -2.0 V");
_Static_assert(kPaBiasRxSafe <= kBiasDacMaxSafe, "PA OFF bias beyond -2.0 V");

/* Vector-modulator tables: 128-state phase grid, step 2.8125 deg.
 * Byte = bit5 polarity, bits[4:0] magnitude. Index with phase % 128. */
extern const uint8_t VM_I[128];
extern const uint8_t VM_Q[128];

typedef enum {
    ADAR_MODE_TR_PIN,    /* TR pin (FPGA) controls TX/RX; the normal run state */
    ADAR_MODE_SPI_TX,    /* bench: force TX over SPI */
    ADAR_MODE_SPI_RX,    /* bench: force RX over SPI */
    ADAR_MODE_NONE       /* adar_get_mode() only: no adar_set_mode() succeeded since adar_init */
} adar_mode_t;

/* Raw register access. adar_write: 3 bytes {(dev<<5)|(reg>>8 & 0x1F), reg, val};
 * reg must be <= 0x1FFF. adar_read: enables SDO on this chip, reads, disables
 * SDO again (the datasheet's single-chip readback recipe, p. 47-48). */
int adar_write(uint8_t dev, uint16_t reg, uint8_t val);
int adar_read(uint8_t dev, uint16_t reg, uint8_t *val);

/* Soft reset, ConfigA SDO, bias+beam RAM bypass, ADC on, scratchpad 0xA5
 * readback (-EIO on mismatch). NOTE: per datasheet p. 47/57 a soft reset sent
 * to chip 0 resets ALL chips on the bus and a reset sent to chips 1..3 does
 * nothing, so init devices in ascending order and never re-init dev 0 while
 * others are configured. */
int adar_init(uint8_t dev);

/* Safe bias, before the ADTR1107 VDD_PA rail is applied: PA gate bias ON =
 * kPaBiasTxSafe (pinch-off) on all four channels, LNA bias ON 0 V (OFF
 * kLnaBiasOff), BIAS_CTRL=0 so the ON values are always used. */
int adar_set_safe_bias(uint8_t dev);

/* Operational bias: PA ON/OFF = kPaBiasOperational/kPaBiasRxSafe (both 0x5D,
 * PA stays pinched), LNA ON/OFF = kLnaBiasOperational/kLnaBiasOff, TX bias
 * currents, then BIAS_CTRL + LNA_BIAS_OUT_EN so the ON/OFF pairs follow TR
 * (PA_ON pin must be high or floating: it has an internal pull-up). */
int adar_set_operational_bias(uint8_t dev);

int adar_set_rx_gain(uint8_t dev, uint8_t ch, uint8_t gain);   /* 0x010+ch, LDRX */
int adar_set_tx_gain(uint8_t dev, uint8_t ch, uint8_t gain);   /* 0x01C+ch, LDTX */
int adar_set_rx_phase(uint8_t dev, uint8_t ch, uint8_t idx);   /* idx % 128 */
int adar_set_tx_phase(uint8_t dev, uint8_t ch, uint8_t idx);

/* Callable at any time and idempotent. TR_PIN also enables all RX/TX
 * sub-circuits (0x02E/0x02F = 0x7F); the SPI modes assume that was done
 * (call TR_PIN once after adar_init). Each SPI mode is one write to 0x031. */
int adar_set_mode(uint8_t dev, adar_mode_t m);

/* Mode last applied successfully by adar_set_mode() (a failed call leaves it
 * unchanged), ADAR_MODE_NONE after adar_init() (a soft reset of chip 0 resets
 * every chip, so it clears all devices) or for dev >= ADAR_COUNT. The only
 * state this module keeps; used by the STATUS line. */
adar_mode_t adar_get_mode(uint8_t dev);
#ifdef HOST_TEST
void adar_test_reset_state(void);   /* all devices back to ADAR_MODE_NONE */
#endif

/* Temperature sensor: start conversion, poll EOC up to 100 ms, read result. */
int adar_read_temp_raw(uint8_t dev, uint8_t *raw);

#endif
