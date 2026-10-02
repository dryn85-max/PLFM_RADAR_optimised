#ifndef HAL_GPIO_H
#define HAL_GPIO_H
#include <stdint.h>

/* Logical pins. The MCU pin behind each one is defined only in pins.h. */
typedef enum {
    PIN_LED,
    PIN_ADAR_CS0, PIN_ADAR_CS1, PIN_ADAR_CS2, PIN_ADAR_CS3,
    PIN_PLL_CS, PIN_PLL_CE, PIN_PLL_LD,
    PIN_EN_FPGA, PIN_EN_LO, PIN_EN_ADAR,
    PIN_EN_ADTR_VDD_SW, PIN_EN_ADTR_VSS_SW,
    PIN_EN_LNA, PIN_EN_PA,
    PIN_FPGA_DIG0, PIN_FPGA_DIG1, PIN_FPGA_DIG2, PIN_FPGA_DIG3,
    PIN_FPGA_DIG4, PIN_FPGA_DIG5, PIN_FPGA_DIG6, PIN_FPGA_DIG7,
    PIN_COUNT
} gpio_t;

void gpio_write(gpio_t pin, int level);   /* any nonzero level = high */
int  gpio_read(gpio_t pin);               /* 0 or 1 */
void gpio_toggle(gpio_t pin);
uint8_t gpio_read_fpga_port(void);        /* DIG0..7 as bits 0..7, one IDR read */

#endif
