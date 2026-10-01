/* Host tests for the pin table (Core/hal/pins_table.c): DIG0..7 = PC0..PC7 in
 * order, unique pins, no Nucleo-reserved line used, safe idle levels. */
#include <stddef.h>
#include "tinytest.h"
#include "hal_gpio.h"
#include "pins.h"
#include "pins_table.h"

#define PORT_A PIN_PORT_A
#define PORT_B PIN_PORT_B
#define PORT_C PIN_PORT_C

static void test_dig_is_pc0_to_pc7_in_order(void)
{
    int i;
    for (i = 0; i < 8; i++) {
        const pin_desc_t *p = &k_pin_table[PIN_FPGA_DIG0 + i];
        TT_ASSERT_EQ(PORT_C, p->port);
        TT_ASSERT_EQ(i, p->pin);
    }
    TT_ASSERT_EQ(0, PIN_FPGA_PORT_SHIFT);
    for (i = 0; i <= 4; i++) TT_ASSERT_EQ(1, k_pin_table[PIN_FPGA_DIG0 + i].is_output);
    for (i = 5; i <= 7; i++) TT_ASSERT_EQ(0, k_pin_table[PIN_FPGA_DIG0 + i].is_output);
}

static void test_no_two_signals_share_a_pin(void)
{
    int i, j;
    for (i = 0; i < PIN_COUNT; i++) {
        for (j = i + 1; j < PIN_COUNT; j++) {
            TT_ASSERT(!(k_pin_table[i].port == k_pin_table[j].port &&
                        k_pin_table[i].pin == k_pin_table[j].pin));
        }
    }
}

static int is_reserved(const pin_desc_t *p)
{
    /* Nucleo-G0B1RE: PC13 B1; PC14/PC15 LSE; PA13/PA14 SWD; PA11/PA12 future
     * USB; PA9/PA10 (remappable onto the PA11/PA12 pads on STM32G0). PF0/PF1
     * (HSE pads) cannot occur: the table only knows ports A, B, C. */
    if (p->port == PORT_C && (p->pin == 13 || p->pin == 14 || p->pin == 15)) return 1;
    if (p->port == PORT_A && p->pin >= 9 && p->pin <= 14) return 1;
    return 0;
}

static void test_no_reserved_pin_used_and_in_range(void)
{
    int i;
    for (i = 0; i < PIN_COUNT; i++) {
        const pin_desc_t *p = &k_pin_table[i];
        TT_ASSERT(p->port <= PORT_C);      /* only A, B, C (so no PF0/PF1) */
        TT_ASSERT(p->pin <= 15);
        TT_ASSERT(!is_reserved(p));
    }
}

static void test_nothing_above_pa8_and_en_pa_is_pc8(void)
{
    int i;
    for (i = 0; i < PIN_COUNT; i++) {
        if (k_pin_table[i].port == PORT_A) TT_ASSERT(k_pin_table[i].pin <= 8);
    }
    TT_ASSERT_EQ(PORT_C, k_pin_table[PIN_EN_PA].port);
    TT_ASSERT_EQ(8, k_pin_table[PIN_EN_PA].pin);
}

static void test_idle_levels(void)
{
    int i;
    gpio_t en[] = { PIN_EN_FPGA, PIN_EN_LO, PIN_EN_ADAR, PIN_EN_ADTR_VDD_SW,
                    PIN_EN_ADTR_VSS_SW, PIN_EN_LNA, PIN_EN_PA, PIN_PLL_CE, PIN_LED };
    gpio_t cs[] = { PIN_ADAR_CS0, PIN_ADAR_CS1, PIN_ADAR_CS2, PIN_ADAR_CS3, PIN_PLL_CS };
    for (i = 0; i < (int)(sizeof en / sizeof en[0]); i++) {
        TT_ASSERT_EQ(1, k_pin_table[en[i]].is_output);
        TT_ASSERT_EQ(0, k_pin_table[en[i]].idle_high);
    }
    for (i = 0; i < (int)(sizeof cs / sizeof cs[0]); i++) {
        TT_ASSERT_EQ(1, k_pin_table[cs[i]].is_output);
        TT_ASSERT_EQ(1, k_pin_table[cs[i]].idle_high);
    }
    for (i = 0; i <= 4; i++) TT_ASSERT_EQ(0, k_pin_table[PIN_FPGA_DIG0 + i].idle_high);
    TT_ASSERT_EQ(0, k_pin_table[PIN_PLL_LD].is_output);
}

int main(void)
{
    TT_RUN(test_dig_is_pc0_to_pc7_in_order);
    TT_RUN(test_no_two_signals_share_a_pin);
    TT_RUN(test_no_reserved_pin_used_and_in_range);
    TT_RUN(test_nothing_above_pa8_and_en_pa_is_pc8);
    TT_RUN(test_idle_levels);
    return TT_RESULT();
}
