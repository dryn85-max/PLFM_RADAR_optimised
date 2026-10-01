/* Host tests for the LO PLL driver (Core/drivers/pll_lo.c) and the example tables. */
#include <errno.h>
#include <string.h>
#include "tinytest.h"
#include "mock_log.h"
#include "pll_lo.h"
#include "hal_gpio.h"
#include "hal_spi.h"
#include "hal_time.h"
#include "config.h"
#include "pll_tables/adf4372_10500MHz.h"
#include "pll_tables/lmx2594_10500MHz.h"

static int count_kind(int kind)
{
    int i, n = 0;
    for (i = 0; i < mock_log_n; i++) if (mock_log[i].kind == kind) n++;
    return n;
}

static const mock_event_t *nth(int kind, int k)
{
    int i;
    for (i = 0; i < mock_log_n; i++) {
        if (mock_log[i].kind == kind && k-- == 0) return &mock_log[i];
    }
    return NULL;
}

static const uint32_t W3[] = { 0x123456u, 0x000000u, 0xFFFFFFu };
static const pll_regs_t T3 = { W3, 3, 5, "test" };

static void test_word_msb_first_single_frame(void)
{
    static const uint32_t w[] = { 0x123456u };
    pll_regs_t t = { w, 1, 0, "one" };
    const mock_event_t *e;
    mock_reset();
    TT_ASSERT_EQ(0, pll_init(&t));
    TT_ASSERT_EQ(1, count_kind(MOCK_EV_SPI));
    e = nth(MOCK_EV_SPI, 0);
    TT_ASSERT(e != NULL);
    if (e == NULL) return;
    TT_ASSERT_EQ(SPI_BUS_PLL, e->a);
    TT_ASSERT_EQ(PIN_PLL_CS, e->b);
    TT_ASSERT_EQ(3, e->n);
    TT_ASSERT_EQ(0x12, e->bytes[0]);
    TT_ASSERT_EQ(0x34, e->bytes[1]);
    TT_ASSERT_EQ(0x56, e->bytes[2]);
}

static void test_order_and_ce_first(void)
{
    const mock_event_t *e;
    mock_reset();
    TT_ASSERT_EQ(0, pll_init(&T3));
    /* first event: CE high; then a 1 ms delay; then the frames */
    TT_ASSERT_EQ(MOCK_EV_GPIO_WRITE, mock_log[0].kind);
    TT_ASSERT_EQ(PIN_PLL_CE, mock_log[0].a);
    TT_ASSERT_EQ(1, mock_log[0].b);
    TT_ASSERT_EQ(MOCK_EV_DELAY_MS, mock_log[1].kind);
    TT_ASSERT_EQ(1, mock_log[1].a);
    TT_ASSERT_EQ(MOCK_EV_SPI, mock_log[2].kind);
    TT_ASSERT_EQ(3, count_kind(MOCK_EV_SPI));
    e = nth(MOCK_EV_SPI, 0); TT_ASSERT_EQ(0x12, e->bytes[0]);
    e = nth(MOCK_EV_SPI, 1); TT_ASSERT_EQ(0x00, e->bytes[0]); TT_ASSERT_EQ(0x00, e->bytes[2]);
    e = nth(MOCK_EV_SPI, 2); TT_ASSERT_EQ(0xFF, e->bytes[0]); TT_ASSERT_EQ(0xFF, e->bytes[2]);
    /* settle after the last word */
    TT_ASSERT_EQ(MOCK_EV_DELAY_MS, mock_log[mock_log_n - 1].kind);
    TT_ASSERT_EQ(5, mock_log[mock_log_n - 1].a);
}

static void test_spi_error_aborts(void)
{
    mock_reset();
    mock_spi_fail_next(-EIO);
    TT_ASSERT_EQ(-EIO, pll_init(&T3));
    TT_ASSERT_EQ(1, count_kind(MOCK_EV_SPI));      /* no further words */
    TT_ASSERT_EQ(0, gpio_read(PIN_PLL_CE));        /* synthesiser left disabled */
}

static void test_bad_args_no_traffic(void)
{
    static const uint32_t bad[] = { 0x000001u, 0x1000000u };   /* 25-bit word */
    pll_regs_t b1 = { bad, 2, 0, "bad" };
    pll_regs_t b2 = { NULL, 1, 0, "null" };
    pll_regs_t b3 = { W3, 0, 0, "empty" };
    mock_reset();
    TT_ASSERT_EQ(-EINVAL, pll_init(NULL));
    TT_ASSERT_EQ(-EINVAL, pll_init(&b1));   /* validated up front: not even word 0 is sent */
    TT_ASSERT_EQ(-EINVAL, pll_init(&b2));
    TT_ASSERT_EQ(-EINVAL, pll_init(&b3));
    TT_ASSERT_EQ(0, mock_log_n);
}

static void test_is_locked_and_power_down(void)
{
    mock_reset();
    TT_ASSERT_EQ(0, pll_is_locked());
    mock_gpio_set_input(PIN_PLL_LD, 1);
    TT_ASSERT_EQ(1, pll_is_locked());
    gpio_write(PIN_PLL_CE, 1);
    pll_power_down();
    TT_ASSERT_EQ(0, gpio_read(PIN_PLL_CE));
}

static uint32_t g_lock_at_us;
static void lock_hook(uint32_t now_us)
{
    if (now_us >= g_lock_at_us) mock_gpio_set_input(PIN_PLL_LD, 1);
}

static void test_wait_lock_at_40ms(void)
{
    mock_reset();
    g_lock_at_us = 40000u;
    mock_time_set_hook(lock_hook);
    TT_ASSERT_EQ(0, pll_wait_lock(100));
    TT_ASSERT_EQ(40, count_kind(MOCK_EV_DELAY_MS));  /* polled every 1 ms */
}

static void test_wait_lock_timeout(void)
{
    mock_reset();
    g_lock_at_us = 150000u;                         /* locks only after the 100 ms window */
    mock_time_set_hook(lock_hook);
    TT_ASSERT_EQ(-ETIMEDOUT, pll_wait_lock(100));
    TT_ASSERT_EQ(100, count_kind(MOCK_EV_DELAY_MS)); /* gave up at the deadline, not later */
}

static void test_wait_lock_already_locked_and_zero_timeout(void)
{
    mock_reset();
    mock_gpio_set_input(PIN_PLL_LD, 1);
    TT_ASSERT_EQ(0, pll_wait_lock(0));              /* checks once even with 0 timeout */
    TT_ASSERT_EQ(0, count_kind(MOCK_EV_DELAY_MS));
    mock_reset();
    TT_ASSERT_EQ(-ETIMEDOUT, pll_wait_lock(0));
}

/* ---- example tables ---------------------------------------------------- */

static void test_tables_format(void)
{
    uint16_t i;
    const pll_regs_t *l = &LMX2594_10500MHZ, *a = &ADF4372_10500MHZ;
    TT_ASSERT(l->count > 0 && a->count > 0);
    TT_ASSERT(l->name != NULL && a->name != NULL);
    for (i = 0; i < l->count; i++) {
        TT_ASSERT(l->words[i] <= 0xFFFFFFu);
        TT_ASSERT_EQ(0, (l->words[i] >> 23) & 1);          /* R/W = write */
        if (i > 0) TT_ASSERT(((l->words[i] >> 16) & 0x7F) < ((l->words[i - 1] >> 16) & 0x7F));
    }
    TT_ASSERT_EQ(112, (l->words[0] >> 16) & 0x7F);           /* R112 first ... */
    TT_ASSERT_EQ(0, (l->words[l->count - 1] >> 16) & 0x7F);  /* ... R0 last */
    for (i = 0; i < a->count; i++) {
        TT_ASSERT(a->words[i] <= 0xFFFFFFu);
        TT_ASSERT_EQ(0, (a->words[i] >> 23) & 1);          /* R/W = write */
    }
}

static void test_tables_are_flagged_placeholders(void)
{
#ifndef PLL_TABLE_PLACEHOLDER
    TT_ASSERT(0 && "tables must define PLL_TABLE_PLACEHOLDER until a real export exists");
#else
    uint16_t i;
    TT_ASSERT_EQ(1, PLL_TABLE_PLACEHOLDER);
    /* placeholders carry no invented register data */
    for (i = 0; i < LMX2594_10500MHZ.count; i++) TT_ASSERT_EQ(0, LMX2594_10500MHZ.words[i] & 0xFFFF);
    for (i = 0; i < ADF4372_10500MHZ.count; i++) TT_ASSERT_EQ(0, ADF4372_10500MHZ.words[i] & 0xFF);
#endif
}

static void test_default_table_selection(void)
{
    const pll_regs_t *t = pll_default_table();
    TT_ASSERT(t != NULL);
#ifdef PLL_PART_ADF4372
    TT_ASSERT(t->words == ADF4372_10500MHZ.words || t->count == ADF4372_10500MHZ.count);
#else
    TT_ASSERT_EQ(LMX2594_10500MHZ.count, t->count);
    TT_ASSERT(strstr(t->name, "LMX2594") != NULL);
#endif
    TT_ASSERT_EQ(1, pll_default_table_is_placeholder());
}

int main(void)
{
    TT_RUN(test_word_msb_first_single_frame);
    TT_RUN(test_order_and_ce_first);
    TT_RUN(test_spi_error_aborts);
    TT_RUN(test_bad_args_no_traffic);
    TT_RUN(test_is_locked_and_power_down);
    TT_RUN(test_wait_lock_at_40ms);
    TT_RUN(test_wait_lock_timeout);
    TT_RUN(test_wait_lock_already_locked_and_zero_timeout);
    TT_RUN(test_tables_format);
    TT_RUN(test_tables_are_flagged_placeholders);
    TT_RUN(test_default_table_selection);
    return TT_RESULT();
}
