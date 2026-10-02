/* ADF4372 register table for a 10.5 GHz LO -- PLACEHOLDER, NOT A REAL EXPORT.
 *
 * !!! PLL_TABLE_PLACEHOLDER = 1: the data fields below are zero and do NOT
 * program the synthesiser. The firmware logs "PLL table is a placeholder" at
 * boot and the lock check fails (non-latched FAULT_PLL_LOCK). Replace with a
 * real export before any hardware use (see BACKLOG). !!!
 *
 * Word format (sent MSB first, 24 bits, one CS assertion per word):
 *     bit 23      R/W = 0 (write)
 *     bits 22..8  register address (15 bits)
 *     bits 7..0   register data (8 bits)
 *   i.e. word = addr15 << 8 | data8.
 *
 * How to generate the real table:
 *   - Tool: Analog Devices ADF4372 evaluation software (version to be recorded
 *     here), device ADF4372, reference 100 MHz, RFout 10.5 GHz; export the
 *     register list ("Register Values" / hex dump).
 *   - Programming order: as the ADF4372 datasheet specifies (the evaluation
 *     software export lists them in the required order); keep that order.
 *   - Lock detect (MUXOUT = digital lock detect) goes to PIN_PLL_LD.
 *   - Paste the export as one WORD(addr, data) per line and delete the
 *     placeholder define.
 *   - Format taken from the project plan (Decision 10); the ADF4372 datasheet
 *     is not in the repository and was not checked.
 */
#ifndef ADF4372_10500MHZ_H
#define ADF4372_10500MHZ_H
#include <stdint.h>
#include "pll_lo.h"

#ifndef PLL_TABLE_PLACEHOLDER
#define PLL_TABLE_PLACEHOLDER 1
#endif

#define ADF4372_WORD(addr, data) \
    ((uint32_t)(((uint32_t)(addr) & 0x7FFFu) << 8) | ((uint32_t)(data) & 0xFFu))

/* Placeholder: a single zero-data word at address 0x000. */
static const uint32_t ADF4372_10500MHZ_WORDS[] = {
    ADF4372_WORD(0x000, 0x00)    /* PLACEHOLDER */
};

static const pll_regs_t ADF4372_10500MHZ = {
    ADF4372_10500MHZ_WORDS,
    (uint16_t)(sizeof ADF4372_10500MHZ_WORDS / sizeof ADF4372_10500MHZ_WORDS[0]),
    10,
    "ADF4372 10.5GHz PLACEHOLDER"
};

#endif
