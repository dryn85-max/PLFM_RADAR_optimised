/* LMX2594 register table for a 10.5 GHz LO -- PLACEHOLDER, NOT A REAL EXPORT.
 *
 * !!! PLL_TABLE_PLACEHOLDER = 1: the data fields below are zero and do NOT
 * program the synthesiser. The firmware logs "PLL table is a placeholder" at
 * boot and the lock check fails (non-latched FAULT_PLL_LOCK). Replace with a
 * real export before any hardware use (see BACKLOG). !!!
 *
 * Word format (sent MSB first, 24 bits, one CS assertion per word):
 *     bit 23      R/W = 0 (write)
 *     bits 22..16 register address R<n> (7 bits)
 *     bits 15..0  register data (16 bits)
 *   i.e. word = addr7 << 16 | data16.
 *
 * How to generate the real table:
 *   - Tool: TI TICS Pro (version used for the export to be recorded here),
 *     device LMX2594, mode "Registers" -> export the 113-word list R112..R0.
 *   - Assumed reference clock: 100 MHz (OSCin); target output: 10.5 GHz
 *     (RFoutA). Set PLL N/R/divider and VCO core in TICS Pro accordingly.
 *   - Programming order: R112 down to R0, then write R0 once more with
 *     FCAL_EN = 1 to start VCO calibration (as the TI LMX2594 datasheet and
 *     TICS Pro describe). The LD pin is routed to PIN_PLL_LD (MUXout as lock
 *     detect, set in the exported registers).
 *   - Paste the export as the word list below (one WORD(addr, data) per line)
 *     and delete the placeholder define.
 *   - Format/order taken from the project plan (Decision 10); the LMX2594
 *     datasheet is not in the repository and was not checked.
 */
#ifndef LMX2594_10500MHZ_H
#define LMX2594_10500MHZ_H
#include <stdint.h>
#include "pll_lo.h"

#ifndef PLL_TABLE_PLACEHOLDER
#define PLL_TABLE_PLACEHOLDER 1
#endif

#define LMX2594_WORD(addr, data) \
    ((uint32_t)(((uint32_t)(addr) & 0x7Fu) << 16) | ((uint32_t)(data) & 0xFFFFu))

/* Placeholder: only the first (R112) and last (R0) positions, data zero. */
static const uint32_t LMX2594_10500MHZ_WORDS[] = {
    LMX2594_WORD(112, 0x0000),   /* PLACEHOLDER */
    LMX2594_WORD(0,   0x0000)    /* PLACEHOLDER */
};

static const pll_regs_t LMX2594_10500MHZ = {
    LMX2594_10500MHZ_WORDS,
    (uint16_t)(sizeof LMX2594_10500MHZ_WORDS / sizeof LMX2594_10500MHZ_WORDS[0]),
    10,                          /* settle_ms after the last word (VCO cal) */
    "LMX2594 10.5GHz PLACEHOLDER"
};

#endif
