/* Host tests for app.c: boot order, fault paths (PLL placeholder, ADAR, latched boot),
 * superloop timers, IWDG refresh and the Gap-3 intents that need app_loop/app_init.
 * Links the real modules over the mocks. */
#include <errno.h>
#include <string.h>
#include "tinytest.h"
#include "mock_log.h"
#include "app.h"
#include "adar1000.h"
#include "agc.h"
#include "config.h"
#include "fault.h"
#include "hal_gpio.h"
#include "hal_spi.h"
#include "hal_time.h"
#include "pll_lo.h"
#include "thermal.h"

static void reset_all(void)
{
    mock_reset();
    fault_test_power_cycle();
}

/* Healthy hardware: PLL lock detect high, every SPI read returns 0xA5
 * (scratchpad readback OK, ADC EOC set). */
static void healthy(void)
{
    reset_all();
    mock_gpio_set_input(PIN_PLL_LD, 1);
    mock_spi_set_default_rx(0xA5);
}

static int reg_of(const mock_event_t *e) { return ((e->bytes[0] & 0x1F) << 8) | e->bytes[1]; }

static int find_gpio(int pin, int level, int from)
{
    int i;
    for (i = from; i < mock_log_n; i++)
        if (mock_log[i].kind == MOCK_EV_GPIO_WRITE && mock_log[i].a == pin && mock_log[i].b == level)
            return i;
    return -1;
}

/* val < 0: any value; mask != 0: (value & mask) != 0 instead of equality */
static int find_adar_write(int reg, int val, int mask, int from)
{
    int i;
    for (i = from; i < mock_log_n; i++) {
        const mock_event_t *e = &mock_log[i];
        if (e->kind != MOCK_EV_SPI || e->a != SPI_BUS_ADAR || (e->bytes[0] & 0x80) || reg_of(e) != reg)
            continue;
        if (val < 0 || (mask ? (e->bytes[2] & mask) != 0 : e->bytes[2] == val))
            return i;
    }
    return -1;
}

static int find_adar_read(int reg, int from)
{
    int i;
    for (i = from; i < mock_log_n; i++) {
        const mock_event_t *e = &mock_log[i];
        if (e->kind == MOCK_EV_SPI && e->a == SPI_BUS_ADAR && (e->bytes[0] & 0x80) && reg_of(e) == reg)
            return i;
    }
    return -1;
}

static int find_kind(int kind, int from)
{
    int i;
    for (i = from; i < mock_log_n; i++)
        if (mock_log[i].kind == kind) return i;
    return -1;
}

static int find_bus(int bus, int from)
{
    int i;
    for (i = from; i < mock_log_n; i++)
        if (mock_log[i].kind == MOCK_EV_SPI && mock_log[i].a == bus) return i;
    return -1;
}

static int count_kind(int kind)
{
    int i, n = 0;
    for (i = 0; i < mock_log_n; i++) if (mock_log[i].kind == kind) n++;
    return n;
}

static void feed_uart(const char *s) { mock_uart_push_rx(s, strlen(s)); }

static void test_happy_path_order(void)
{
    int a, b, c, d, e, f, g, h, i, i2, j, k, l, m, n, n2, n3, o, p, q, w;
    healthy();
    app_init();
    TT_ASSERT_EQ(FAULT_NONE, fault_active());

    a = find_gpio(PIN_EN_FPGA, 1, 0);
    b = find_gpio(PIN_EN_LO, 1, a);
    c = find_gpio(PIN_EN_ADAR, 1, b);
    d = find_gpio(PIN_EN_ADTR_VDD_SW, 1, c);
    e = find_gpio(PIN_EN_ADTR_VSS_SW, 1, d);
    f = find_bus(SPI_BUS_PLL, e);                       /* PLL words after the base rails */
    g = find_adar_write(REG_INTERFACE_CONFIG_A, INTERFACE_CONFIG_A_SOFT_RESET, 0, f);
    h = find_adar_read(REG_SCRATCHPAD, g);              /* scratchpad readback */
    i = find_adar_write(REG_PA_CH1_BIAS_ON, kPaBiasTxSafe, 0, h);
    /* I1: ADTR1107 CTRL_SW is driven to receive (SPI RX mode, 0xB0) before any
     * RF rail rises: TR_SW_POS floats after power-up/soft reset (DS p. 38/44). */
    i2 = find_adar_write(REG_SW_CONTROL, 0xB0, 0, i);
    j = find_gpio(PIN_EN_LNA, 1, i);
    k = find_gpio(PIN_EN_PA, 1, j);
    l = find_adar_write(REG_PA_CH1_BIAS_OFF, kPaBiasRxSafe, 0, k);   /* operational bias */
    m = find_adar_write(REG_SW_CONTROL, 1, SW_CTRL_TR_SOURCE, l);   /* TR-pin mode */
    n = find_adar_write(REG_CH1_RX_PHS_I, -1, 0, m);    /* beam 0 */
    /* B: boot gains as upstream: TX VGA 0x7F, RX VGA = AGC base (30), after the beam */
    n2 = find_adar_write(REG_CH1_TX_GAIN, kDefaultTxVgaGain, 0, n);
    n3 = find_adar_write(REG_CH1_RX_GAIN, kDefaultRxVgaGain, 0, n);
    o = find_gpio(PIN_FPGA_DIG4, 0, n);                 /* FPGA reset pulse: low ... */
    p = find_gpio(PIN_FPGA_DIG4, 1, o);                 /* ... released */
    q = find_gpio(PIN_FPGA_DIG3, 1, p);                 /* mixers on */
    w = find_kind(MOCK_EV_I2C_WRITE, q);                /* ADS7830 warm-up last */
    TT_ASSERT(a >= 0 && b > a && c > b && d > c && e > d);
    TT_ASSERT(f > e && g > f && h > g && i > h && i2 > i && j > i2 && k > j);
    TT_ASSERT(n2 > n && n3 > n && o > n2 && o > n3);
    TT_ASSERT(l > k && m > l && n > m && o > n && p > o && q > p && w > q);
    TT_ASSERT_EQ(0x48, mock_log[w].a);
    TT_ASSERT_EQ(0x8C, mock_log[w].bytes[0]);
    TT_ASSERT_EQ(1, count_kind(MOCK_EV_I2C_WRITE));     /* exactly one warm-up read */
    /* every device was put in TR-pin mode */
    {
        int cnt = 0, x;
        for (x = 0; x < mock_log_n; x++) {
            const mock_event_t *ev = &mock_log[x];
            if (ev->kind == MOCK_EV_SPI && ev->a == SPI_BUS_ADAR && !(ev->bytes[0] & 0x80) &&
                reg_of(ev) == REG_SW_CONTROL && (ev->bytes[2] & SW_CTRL_TR_SOURCE)) cnt++;
        }
        TT_ASSERT_EQ(ADAR_COUNT, cnt);
    }
    /* the DIG3/mixers pin is never high before the FPGA reset is released */
    TT_ASSERT(find_gpio(PIN_FPGA_DIG3, 1, 0) > p);
    TT_ASSERT_EQ(0, thermal_last());                    /* warm-up result is discarded */
    TT_ASSERT_EQ(0, thermal_last_err());
}

static void test_boot_gains_and_agc_cache(void)
{
    int g, ch, found;
    healthy();
    app_init();
    TT_ASSERT_EQ(FAULT_NONE, fault_active());
    for (g = 0; g < ADAR_COUNT * 4; g++) {
        /* AGC cache matches what was written (so the AGC does not rewrite it) */
        TT_ASSERT_EQ(kDefaultRxVgaGain, app_agc()->written[g]);
        ch = g % 4;
        found = 0;
        {
            int x;
            for (x = 0; x < mock_log_n; x++) {
                const mock_event_t *e = &mock_log[x];
                if (e->kind == MOCK_EV_SPI && e->a == SPI_BUS_ADAR && !(e->bytes[0] & 0x80) &&
                    ((e->bytes[0] >> 5) & 7) == g / 4 && reg_of(e) == REG_CH1_TX_GAIN + ch &&
                    e->bytes[2] == kDefaultTxVgaGain) found = 1;
            }
        }
        TT_ASSERT(found);
    }
}

/* A: no PA/LNA gate bias DAC write beyond 0x6A (-2.0 V) during a whole boot. */
static void test_boot_bias_writes_within_limit(void)
{
    int x, n = 0;
    healthy();
    app_init();
    for (x = 0; x < mock_log_n; x++) {
        const mock_event_t *e = &mock_log[x];
        int reg;
        if (e->kind != MOCK_EV_SPI || e->a != SPI_BUS_ADAR || (e->bytes[0] & 0x80)) continue;
        reg = reg_of(e);
        if ((reg >= REG_PA_CH1_BIAS_ON && reg <= REG_LNA_BIAS_ON) ||
            (reg >= REG_PA_CH1_BIAS_OFF && reg <= REG_LNA_BIAS_OFF)) {
            TT_ASSERT(e->bytes[2] <= 0x6A);
            n++;
        }
    }
    TT_ASSERT(n >= 10 * ADAR_COUNT);
}

static void test_warmup_errors_ignored(void)
{
    healthy();
    mock_i2c_fail_next(-EIO);                           /* the warm-up write fails */
    app_init();
    TT_ASSERT_EQ(FAULT_NONE, fault_active());
    TT_ASSERT(find_gpio(PIN_FPGA_DIG3, 1, 0) >= 0);     /* boot completed */
    TT_ASSERT_EQ(0, thermal_last_err());
}

static void test_pll_placeholder_lock_fails(void)
{
    uint32_t t0;
    int i;
    reset_all();                                        /* LD stays low: the placeholder table cannot lock */
    TT_ASSERT_EQ(1, pll_default_table_is_placeholder());
    mock_spi_set_default_rx(0xA5);
    t0 = millis();
    app_init();
    TT_ASSERT_EQ(FAULT_PLL_LOCK, fault_active());
    TT_ASSERT_EQ(0, fault_is_latched());                /* non-latched */
    TT_ASSERT((uint32_t)(millis() - t0) >= PLL_LOCK_TIMEOUT_MS);
    TT_ASSERT(find_bus(SPI_BUS_PLL, 0) >= 0);           /* the table was sent */
    TT_ASSERT_EQ(-1, find_bus(SPI_BUS_ADAR, 0));        /* no ADAR traffic */
    TT_ASSERT_EQ(-1, find_gpio(PIN_EN_LNA, 1, 0));      /* RF rails never enabled */
    TT_ASSERT_EQ(-1, find_gpio(PIN_EN_PA, 1, 0));
    TT_ASSERT_EQ(-1, find_gpio(PIN_FPGA_DIG3, 1, 0));   /* mixers never on */
    TT_ASSERT(find_gpio(PIN_EN_FPGA, 1, 0) >= 0);       /* base rails stay (non-latched class) */
    TT_ASSERT_EQ(0, count_kind(MOCK_EV_I2C_WRITE));
    for (i = 0; i < mock_log_n; i++)
        if (mock_log[i].kind == MOCK_EV_GPIO_WRITE &&
            (mock_log[i].a == PIN_EN_PA || mock_log[i].a == PIN_EN_LNA))
            TT_ASSERT_EQ(0, mock_log[i].b);
    /* the board keeps answering; no periodic work while faulted */
    mock_log_n = 0;
    mock_gpio_port_reads = 0;
    feed_uart("status\n");
    mock_time_advance_us(10u * 1000u * 1000u);
    app_loop();
    TT_ASSERT(strstr(mock_uart_tx, " fault=1 latched=0 ") != NULL);
    TT_ASSERT_EQ(0, count_kind(MOCK_EV_I2C_WRITE));
    TT_ASSERT_EQ(0, mock_gpio_port_reads);
}

static void test_pll_spi_error_is_pll_fault(void)
{
    healthy();
    mock_spi_fail_next(-EIO);                           /* first PLL word fails */
    app_init();
    TT_ASSERT_EQ(FAULT_PLL_LOCK, fault_active());
    TT_ASSERT_EQ(-1, find_gpio(PIN_EN_PA, 1, 0));
}

static void test_adar_scratchpad_failure(void)
{
    reset_all();
    mock_gpio_set_input(PIN_PLL_LD, 1);                 /* lock ok, but SPI reads return 0 */
    app_init();
    TT_ASSERT_EQ(FAULT_ADAR_COMM, fault_active());
    TT_ASSERT_EQ(0, fault_is_latched());
    TT_ASSERT_EQ(-1, find_gpio(PIN_EN_LNA, 1, 0));
    TT_ASSERT_EQ(-1, find_gpio(PIN_EN_PA, 1, 0));
    TT_ASSERT_EQ(-1, find_gpio(PIN_FPGA_DIG3, 1, 0));
    TT_ASSERT_EQ(-1, find_gpio(PIN_FPGA_DIG4, 1, 0));   /* FPGA stays in reset */
}

static void test_latched_boot(void)
{
    int i;
    healthy();
    fault_raise(FAULT_OVERTEMP);
    fault_test_simulate_reset();                        /* IWDG reset: latch survives */
    mock_log_n = 0;
    app_init();
    TT_ASSERT_EQ(0, count_kind(MOCK_EV_SPI));
    TT_ASSERT_EQ(0, count_kind(MOCK_EV_I2C_WRITE));
    TT_ASSERT_EQ(0, count_kind(MOCK_EV_DELAY_MS) + count_kind(MOCK_EV_DELAY_US));
    for (i = 0; i < mock_log_n; i++)
        if (mock_log[i].kind == MOCK_EV_GPIO_WRITE) TT_ASSERT_EQ(0, mock_log[i].b);   /* only e-stop lows */
    TT_ASSERT(strstr(mock_uart_tx, "BOOT latched fault=10\r\n") != NULL);
    mock_log_n = 0;
    feed_uart("tx\nstatus\n");
    app_loop();
    TT_ASSERT(strstr(mock_uart_tx, "ERR latched\r\n") != NULL);
    TT_ASSERT(strstr(mock_uart_tx, " fault=10 latched=1 ") != NULL);
    TT_ASSERT_EQ(0, count_kind(MOCK_EV_SPI));
}

/* Gap-3 intent 3: the loop refreshes the watchdog every pass, in every state. */
static void test_loop_refreshes_iwdg(void)
{
    int i, before;
    healthy();
    app_init();
    for (i = 0; i < 5; i++) {
        before = mock_iwdg_refreshes;
        app_loop();
        TT_ASSERT_EQ(before + 1, mock_iwdg_refreshes);
    }
    /* non-latched fault */
    fault_raise(FAULT_PLL_LOCK);
    before = mock_iwdg_refreshes;
    app_loop();
    TT_ASSERT_EQ(before + 1, mock_iwdg_refreshes);
    /* latched fault, and after a reset into the latched boot */
    fault_raise(FAULT_OVERTEMP);
    before = mock_iwdg_refreshes;
    app_loop();
    TT_ASSERT_EQ(before + 1, mock_iwdg_refreshes);
    fault_test_simulate_reset();
    app_init();
    before = mock_iwdg_refreshes;
    app_loop();
    TT_ASSERT_EQ(before + 1, mock_iwdg_refreshes);
}

/* Gap-3 intent 5: timers start at millis() in app_init, no spurious tick even
 * when millis() starts large. */
static void test_cold_start_no_spurious_ticks(void)
{
    healthy();
    mock_time_advance_us(3000u * 1000u * 1000u);        /* "uptime" 3,000,000 ms before the app starts */
    app_init();
    mock_log_n = 0;
    mock_gpio_port_reads = 0;
    app_loop();
    TT_ASSERT_EQ(0, count_kind(MOCK_EV_I2C_WRITE));     /* no thermal read */
    TT_ASSERT_EQ(0, mock_gpio_port_reads);              /* no AGC frame */
    mock_time_advance_us(249u * 1000u);
    app_loop();
    TT_ASSERT_EQ(0, mock_gpio_port_reads);
    mock_time_advance_us(1000u);                        /* +250 ms: first AGC frame */
    app_loop();
    TT_ASSERT_EQ(1, mock_gpio_port_reads);
    TT_ASSERT_EQ(0, count_kind(MOCK_EV_I2C_WRITE));
    mock_time_advance_us(4750u * 1000u);                /* +5000 ms: first thermal read */
    app_loop();
    TT_ASSERT_EQ(1, count_kind(MOCK_EV_I2C_WRITE));
    TT_ASSERT_EQ(FAULT_NONE, fault_active());
}

static void test_pll_lock_loss_in_run(void)
{
    healthy();
    app_init();
    mock_gpio_set_input(PIN_PLL_LD, 0);
    mock_time_advance_us(99u * 1000u);
    app_loop();
    TT_ASSERT_EQ(FAULT_NONE, fault_active());           /* not checked before 100 ms */
    mock_time_advance_us(1000u);
    gpio_write(PIN_EN_PA, 1);
    gpio_write(PIN_EN_LNA, 1);
    app_loop();
    TT_ASSERT_EQ(FAULT_PLL_LOCK, fault_active());
    TT_ASSERT_EQ(0, fault_is_latched());
    TT_ASSERT_EQ(0, gpio_read(PIN_EN_PA));              /* RF off, base rails stay */
    TT_ASSERT_EQ(0, gpio_read(PIN_EN_LNA));
    TT_ASSERT_EQ(1, gpio_read(PIN_EN_FPGA));
    /* no periodic work while the fault is active */
    mock_log_n = 0;
    mock_gpio_port_reads = 0;
    mock_time_advance_us(6u * 1000u * 1000u);
    app_loop();
    TT_ASSERT_EQ(0, mock_gpio_port_reads);
    TT_ASSERT_EQ(0, count_kind(MOCK_EV_I2C_WRITE));
}

static void test_overtemp_in_run_latches_and_stops_work(void)
{
    uint8_t raw = 154;                                  /* 75.2 C: first count >= 75.0 */
    healthy();
    app_init();
    mock_time_advance_us(5000u * 1000u);
    mock_i2c_set_rx(&raw, 1);
    mock_gpio_set_input(PIN_PLL_LD, 1);
    app_loop();
    TT_ASSERT_EQ(FAULT_OVERTEMP, fault_latched_code());
    TT_ASSERT_EQ(0, gpio_read(PIN_EN_PA));
    TT_ASSERT_EQ(0, gpio_read(PIN_EN_FPGA));
    mock_log_n = 0;
    mock_gpio_port_reads = 0;
    mock_time_advance_us(10u * 1000u * 1000u);
    app_loop();
    TT_ASSERT_EQ(0, count_kind(MOCK_EV_I2C_WRITE));
    TT_ASSERT_EQ(0, mock_gpio_port_reads);
}

static void test_agc_runs_in_loop(void)
{
    int i, writes = 0;
    healthy();
    app_init();
    mock_gpio_set_input(PIN_FPGA_DIG6, 1);              /* host AGC enable */
    mock_gpio_set_input(PIN_FPGA_DIG5, 1);              /* saturation */
    mock_time_advance_us(250u * 1000u);
    app_loop();                                         /* frame 1: debounce, still off */
    TT_ASSERT_EQ(0, app_agc()->enabled);
    mock_time_advance_us(250u * 1000u);
    mock_log_n = 0;
    app_loop();                                         /* frame 2: enabled, base 30 -> 26, all channels written */
    TT_ASSERT_EQ(1, app_agc()->enabled);
    TT_ASSERT_EQ(26, app_agc()->base);
    for (i = 0; i < mock_log_n; i++) {
        const mock_event_t *e = &mock_log[i];
        if (e->kind == MOCK_EV_SPI && e->a == SPI_BUS_ADAR && reg_of(e) >= REG_CH1_RX_GAIN &&
            reg_of(e) <= REG_CH1_RX_GAIN + 3) {
            TT_ASSERT_EQ(26, e->bytes[2]);
            writes++;
        }
    }
    TT_ASSERT_EQ(ADAR_COUNT * 4, writes);
}

int main(void)
{
    TT_RUN(test_happy_path_order);
    TT_RUN(test_boot_gains_and_agc_cache);
    TT_RUN(test_boot_bias_writes_within_limit);
    TT_RUN(test_warmup_errors_ignored);
    TT_RUN(test_pll_placeholder_lock_fails);
    TT_RUN(test_pll_spi_error_is_pll_fault);
    TT_RUN(test_adar_scratchpad_failure);
    TT_RUN(test_latched_boot);
    TT_RUN(test_loop_refreshes_iwdg);
    TT_RUN(test_cold_start_no_spurious_ticks);
    TT_RUN(test_pll_lock_loss_in_run);
    TT_RUN(test_overtemp_in_run_latches_and_stops_work);
    TT_RUN(test_agc_runs_in_loop);
    return TT_RESULT();
}
