#include <errno.h>
#include "pll_lo.h"
#include "config.h"
#include "hal_gpio.h"
#include "hal_spi.h"
#include "hal_time.h"

#ifdef PLL_PART_ADF4372
#include "pll_tables/adf4372_10500MHz.h"
#define PLL_DEFAULT_TABLE (&ADF4372_10500MHZ)
#else
#include "pll_tables/lmx2594_10500MHz.h"
#define PLL_DEFAULT_TABLE (&LMX2594_10500MHZ)
#endif

int pll_init(const pll_regs_t *t)
{
    uint16_t i;
    int rc;
    if (t == NULL || t->words == NULL || t->count == 0) {
        return -EINVAL;
    }
    for (i = 0; i < t->count; i++) {         /* validate before touching hardware */
        if (t->words[i] > 0xFFFFFFu) {
            return -EINVAL;
        }
    }
    gpio_write(PIN_PLL_CE, 1);
    delay_ms(1);
    for (i = 0; i < t->count; i++) {
        uint8_t b[3];
        b[0] = (uint8_t)(t->words[i] >> 16);
        b[1] = (uint8_t)(t->words[i] >> 8);
        b[2] = (uint8_t)t->words[i];
        rc = spi_xfer(SPI_BUS_PLL, PIN_PLL_CS, b, NULL, sizeof b);
        if (rc != 0) {
            gpio_write(PIN_PLL_CE, 0);
            return rc;
        }
    }
    delay_ms(t->settle_ms);
    return 0;
}

int pll_is_locked(void)
{
    return gpio_read(PIN_PLL_LD) ? 1 : 0;
}

int pll_wait_lock(uint32_t timeout_ms)
{
    uint32_t i;
    /* Counts delays instead of comparing millis(): immune to counter wrap. */
    for (i = 0; i < timeout_ms; i++) {
        if (pll_is_locked()) {
            return 0;
        }
        delay_ms(1);
    }
    return pll_is_locked() ? 0 : -ETIMEDOUT;
}

void pll_power_down(void)
{
    gpio_write(PIN_PLL_CE, 0);
}

const pll_regs_t *pll_default_table(void)
{
    return PLL_DEFAULT_TABLE;
}

int pll_default_table_is_placeholder(void)
{
#ifdef PLL_TABLE_PLACEHOLDER
    return PLL_TABLE_PLACEHOLDER ? 1 : 0;
#else
    return 0;
#endif
}
