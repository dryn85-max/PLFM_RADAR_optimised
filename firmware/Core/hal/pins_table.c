/* Pin plan data, see pins_table.h and the pins.h header comment. */
#include "pins_table.h"

#define A PIN_PORT_A
#define B PIN_PORT_B
#define C PIN_PORT_C

const pin_desc_t k_pin_table[PIN_COUNT] = {
    [PIN_LED]             = { A, 5,  1, 0 },
    /* Active-low chip selects idle high, everything else idles low. */
    [PIN_ADAR_CS0]        = { B, 12, 1, 1 },
    [PIN_ADAR_CS1]        = { B, 11, 1, 1 },
    [PIN_ADAR_CS2]        = { B, 10, 1, 1 },
    [PIN_ADAR_CS3]        = { B, 2,  1, 1 },
    [PIN_PLL_CS]          = { B, 1,  1, 1 },
    [PIN_PLL_CE]          = { B, 0,  1, 0 },
    [PIN_PLL_LD]          = { B, 6,  0, 0 },
    [PIN_EN_FPGA]         = { A, 0,  1, 0 },
    [PIN_EN_LO]           = { A, 1,  1, 0 },
    [PIN_EN_ADAR]         = { A, 4,  1, 0 },
    [PIN_EN_ADTR_VDD_SW]  = { A, 6,  1, 0 },
    [PIN_EN_ADTR_VSS_SW]  = { A, 7,  1, 0 },
    [PIN_EN_LNA]          = { A, 8,  1, 0 },
    [PIN_EN_PA]           = { C, 8,  1, 0 },
    [PIN_FPGA_DIG0]       = { C, 0,  1, 0 },
    [PIN_FPGA_DIG1]       = { C, 1,  1, 0 },
    [PIN_FPGA_DIG2]       = { C, 2,  1, 0 },
    [PIN_FPGA_DIG3]       = { C, 3,  1, 0 },
    [PIN_FPGA_DIG4]       = { C, 4,  1, 0 },
    [PIN_FPGA_DIG5]       = { C, 5,  0, 0 },
    [PIN_FPGA_DIG6]       = { C, 6,  0, 0 },
    [PIN_FPGA_DIG7]       = { C, 7,  0, 0 },
};
