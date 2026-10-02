/* LO PLL driver: part-independent register-table loader (ADF4372 or LMX2594).
 *
 * Both parts take 24-bit SPI words, MSB first, one CS assertion per word
 * (SPI_BUS_PLL, PIN_PLL_CS, <= 10 MHz, mode 0):
 *   ADF4372 : (R/W=0 : addr[14:0] : data[7:0])  =  addr15 << 8 | data8
 *   LMX2594 : (R/W=0 : addr[6:0]  : data[15:0]) =  addr7 << 16 | data16
 * Words come from the vendor tool export (ADI software / TICS Pro) as a
 * const uint32_t[] (see pll_tables/). */
#ifndef PLL_LO_H
#define PLL_LO_H
#include <stdint.h>

typedef struct {
    const uint32_t *words;   /* 24-bit words, sent in table order */
    uint16_t count;
    uint16_t settle_ms;      /* wait after the last word before checking lock */
    const char *name;
} pll_regs_t;

/* CE high, 1 ms, each word as one 3-byte MSB-first SPI frame, then settle_ms.
 * -EINVAL (no hardware access) for NULL table/words, count 0 or any word above
 * 0xFFFFFF. On a SPI error the transfer aborts, CE is dropped again and the
 * error code is returned. */
int  pll_init(const pll_regs_t *t);
int  pll_is_locked(void);                    /* lock-detect pin: 1 locked, 0 not */
/* Polls the lock detect every 1 ms (checks once, then once per delay) for at
 * most timeout_ms. Returns 0 or -ETIMEDOUT. */
int  pll_wait_lock(uint32_t timeout_ms);
void pll_power_down(void);                   /* CE low */

/* Table selected by PLL_PART_LMX2594 (default) / PLL_PART_ADF4372 in config.h. */
const pll_regs_t *pll_default_table(void);
/* 1 while the selected table is a placeholder (PLL_TABLE_PLACEHOLDER): the
 * synthesiser is not really programmed and lock will not be reached. */
int  pll_default_table_is_placeholder(void);

#endif
