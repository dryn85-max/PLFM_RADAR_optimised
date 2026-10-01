/* Pin map: NUCLEO-G0B1RE (STM32G0B1RET6). Included only by Core/hal/*.c and main.c.
 * Role -> MCU pin; AFs to be verified against the datasheet before the README
 * wiring table is finalised.
 *
 *  ADAR1000 SPI2 SCK/MISO/MOSI    PB13 / PB14 / PB15 (AF0), 16 MHz (64/4)
 *  ADAR CS0..CS3                  PB12, PB11, PB10, PB2
 *  PLL SPI1 SCK/MISO/MOSI         PB3 / PB4 / PB5 (AF0), 8 MHz (64/8)
 *  PLL CS / CE / LD               PB1 / PB0 (out) / PB6 (in)
 *  I2C1 SCL / SDA (ADS7830)       PB8 / PB9 (AF6), 100 kHz
 *  USART2 TX / RX (ST-LINK VCP)   PA2 / PA3 (AF1), 115200 8N1
 *  LED LD4                        PA5
 *  EN_FPGA, EN_LO, EN_ADAR        PA0, PA1, PA4 (active high)
 *  EN_ADTR_VDD_SW, EN_ADTR_VSS_SW PA6, PA7
 *  EN_LNA (3V3), EN_PA (5 V)      PA8, PA9
 *  FPGA DIG0..DIG4 (outputs)      PC0..PC4: new_chirp, new_elevation,
 *                                 new_azimuth, mixers_enable, fpga_reset_n
 *  FPGA DIG5..DIG7 (inputs)       PC5..PC7: agc_saturation, agc_enable, reserved
 *
 * Target mapping table (port, pin, direction) lives in hal_gpio.c (Task 3).
 */
#ifndef PINS_H
#define PINS_H

#define PIN_FPGA_PORT_SHIFT 0   /* DIG0 is PC0: IDR bits 0..7 = DIG0..7 */

#endif
