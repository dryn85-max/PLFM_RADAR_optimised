/* ADS7830 8-bit 8-channel I2C ADC, single-ended reads (port of upstream
 * ADS7830.c; HAL-free, timeouts come from hal_i2c, errors are negative errno).
 * Datasheet: TI SBAS302C, "7_Components Datasheets and Application notes/ads7830.pdf". */
#ifndef ADS7830_H
#define ADS7830_H
#include <stdint.h>

/* Slave address 10010 A1 A0 (datasheet p. 13): 0x48..0x4B. */
#define ADS7830_ADDR_MIN   0x48
#define ADS7830_ADDR_MAX   0x4B

/* Command byte: SD C2 C1 C0 PD1 PD0 X X (p. 13). pd_bits are already in
 * place (bits 3:2), Table 1 (p. 13). */
#define ADS7830_PD_AUTO            0x00   /* power down between conversions */
#define ADS7830_PD_REF_OFF_ADC_ON  0x04   /* internal reference off, ADC on */
#define ADS7830_PD_REF_ON_ADC_OFF  0x08
#define ADS7830_PD_REF_ON_ADC_ON   0x0C   /* internal 2.5 V reference on */

/* Command byte for a single-ended read of channel ch (0..7, not checked).
 * The channel select field is NOT ch<<4: Table 2 (p. 14) maps C2C1C0 =
 * 000,100,001,101,010,110,011,111 to CH0..CH7, i.e. (ch>>1) | ((ch&1)<<2). */
uint8_t ads7830_command(uint8_t ch, uint8_t pd_bits);

/* One single-ended conversion: write command, wait 1 ms, read 1 byte.
 * Returns 0..255, or a negative errno (-EINVAL for bad address/channel/pd_bits
 * without bus traffic; -EIO/-ETIMEDOUT from the bus). 0xFF is a valid reading,
 * never an error marker. */
int ads7830_read_se(uint8_t addr7, uint8_t ch, uint8_t pd_bits);

#endif
