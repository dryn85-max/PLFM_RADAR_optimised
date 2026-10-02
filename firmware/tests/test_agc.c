/* Host tests for agc.c: semantics of upstream ADAR1000_AGC (attack/decay/holdoff/clamps),
 * arrays sized ADAR_COUNT*4, writes only for changed channels, 0-based channels. */
#include <errno.h>
#include <string.h>
#include "tinytest.h"
#include "mock_log.h"
#include "agc.h"
#include "fpga_if.h"
#include "adar1000.h"
#include "config.h"
#include "hal_time.h"
#include "hal_gpio.h"

#define NCH (ADAR_COUNT * 4)

static void fresh(agc_t *a)
{
    mock_reset();
    fpga_if_init();      /* debounce state is file-static */
    agc_init(a);
    mock_log_n = 0;
}

static int spi_count(void)
{
    int i, n = 0;
    for (i = 0; i < mock_log_n; i++) if (mock_log[i].kind == MOCK_EV_SPI) n++;
    return n;
}

/* gain-register writes in the log: returns count, fills dev/reg/val */
static int gain_writes(int *dev, int *reg, int *val, int cap)
{
    int i, n = 0;
    for (i = 0; i < mock_log_n; i++) {
        const mock_event_t *e = &mock_log[i];
        int r;
        if (e->kind != MOCK_EV_SPI) continue;
        r = ((e->bytes[0] & 0x1F) << 8) | e->bytes[1];
        if (r >= REG_CH1_RX_GAIN && r <= REG_CH1_RX_GAIN + 3 && n < cap) {
            dev[n] = e->bytes[0] >> 5; reg[n] = r; val[n] = e->bytes[2]; n++;
        }
    }
    return n;
}

static void test_defaults(void)
{
    agc_t a;
    int g;
    fresh(&a);
    TT_ASSERT_EQ(30, a.base);
    TT_ASSERT_EQ(4, a.step_down);
    TT_ASSERT_EQ(1, a.step_up);
    TT_ASSERT_EQ(0, a.min_gain);
    TT_ASSERT_EQ(127, a.max_gain);
    TT_ASSERT_EQ(4, a.holdoff_frames);
    TT_ASSERT_EQ(0, a.enabled);
    TT_ASSERT_EQ(0, a.holdoff_counter);
    TT_ASSERT_EQ(0, a.last_saturated);
    TT_ASSERT_EQ(0, a.sat_events);
    TT_ASSERT_EQ(NCH, (int)(sizeof a.cal_offset / sizeof a.cal_offset[0]));
    TT_ASSERT_EQ(NCH, (int)(sizeof a.written / sizeof a.written[0]));
    for (g = 0; g < NCH; g++) { TT_ASSERT_EQ(0, a.cal_offset[g]); TT_ASSERT_EQ(-1, a.written[g]); }
}

static void test_attack(void)
{
    agc_t a;
    fresh(&a);
    a.enabled = 1;
    TT_ASSERT_EQ(1, agc_update(&a, 1));
    TT_ASSERT_EQ(26, a.base);
    TT_ASSERT_EQ(1, a.last_saturated);
    TT_ASSERT_EQ(1, a.sat_events);
    TT_ASSERT_EQ(0, a.holdoff_counter);
}

static void test_attack_floor(void)
{
    agc_t a;
    fresh(&a);
    a.enabled = 1; a.base = 5;
    TT_ASSERT_EQ(1, agc_update(&a, 1));     /* 5 < 4+0? no: 5>=4 -> 1 */
    TT_ASSERT_EQ(1, a.base);
    TT_ASSERT_EQ(1, agc_update(&a, 1));     /* 1 < 4 -> min */
    TT_ASSERT_EQ(0, a.base);
    TT_ASSERT_EQ(0, agc_update(&a, 1));     /* already at the floor: no change */
    TT_ASSERT_EQ(0, a.base);
    TT_ASSERT_EQ(3, a.sat_events);
    /* non-zero min_gain: exact boundary base == step_down + min -> subtract */
    fresh(&a);
    a.enabled = 1; a.min_gain = 10; a.base = 14;
    agc_update(&a, 1);
    TT_ASSERT_EQ(10, a.base);
    a.base = 13;
    agc_update(&a, 1);
    TT_ASSERT_EQ(10, a.base);
}

static void test_holdoff_and_recovery(void)
{
    agc_t a;
    fresh(&a);
    a.enabled = 1;
    TT_ASSERT_EQ(0, agc_update(&a, 0));
    TT_ASSERT_EQ(0, agc_update(&a, 0));
    TT_ASSERT_EQ(0, agc_update(&a, 0));
    TT_ASSERT_EQ(30, a.base);
    TT_ASSERT_EQ(3, a.holdoff_counter);
    TT_ASSERT_EQ(1, agc_update(&a, 0));     /* 4th clear frame */
    TT_ASSERT_EQ(31, a.base);
    TT_ASSERT_EQ(0, a.holdoff_counter);
    TT_ASSERT_EQ(0, a.last_saturated);
}

static void test_saturation_resets_holdoff(void)
{
    agc_t a;
    fresh(&a);
    a.enabled = 1;
    agc_update(&a, 0); agc_update(&a, 0); agc_update(&a, 0);
    agc_update(&a, 1);
    TT_ASSERT_EQ(0, a.holdoff_counter);
    TT_ASSERT_EQ(26, a.base);
    agc_update(&a, 0); agc_update(&a, 0); agc_update(&a, 0);
    TT_ASSERT_EQ(26, a.base);                /* needs 4 fresh clear frames */
    agc_update(&a, 0);
    TT_ASSERT_EQ(27, a.base);
}

static void test_ceiling(void)
{
    agc_t a;
    fresh(&a);
    a.enabled = 1; a.base = 127; a.holdoff_frames = 1;
    TT_ASSERT_EQ(0, agc_update(&a, 0));
    TT_ASSERT_EQ(127, a.base);
    a.base = 126;
    TT_ASSERT_EQ(1, agc_update(&a, 0));
    TT_ASSERT_EQ(127, a.base);
    /* base above max (config error) is pulled back to max, like upstream */
    a.base = 200; a.step_up = 1;
    TT_ASSERT_EQ(1, agc_update(&a, 0));
    TT_ASSERT_EQ(127, a.base);
    /* big step_up must not wrap uint8 */
    a.base = 120; a.step_up = 200; a.max_gain = 255;
    agc_update(&a, 0);
    TT_ASSERT_EQ(255, a.base);
    a.base = 250; a.step_up = 200;
    agc_update(&a, 0);
    TT_ASSERT_EQ(255, a.base);
}

static void test_holdoff_zero_steps_every_frame(void)
{
    agc_t a;
    fresh(&a);
    a.enabled = 1; a.holdoff_frames = 0;
    agc_update(&a, 0);
    TT_ASSERT_EQ(31, a.base);
    agc_update(&a, 0);
    TT_ASSERT_EQ(32, a.base);
}

static void test_disabled_is_noop(void)
{
    agc_t a;
    fresh(&a);
    TT_ASSERT_EQ(0, agc_update(&a, 1));
    TT_ASSERT_EQ(30, a.base);
    TT_ASSERT_EQ(0, a.sat_events);
    TT_ASSERT_EQ(0, a.last_saturated);
    TT_ASSERT_EQ(0, a.holdoff_counter);
    TT_ASSERT_EQ(0, agc_update(&a, 0));
    TT_ASSERT_EQ(0, a.holdoff_counter);
}

static void test_effective_clamp(void)
{
    agc_t a;
    fresh(&a);
    a.cal_offset[0] = -50; a.cal_offset[NCH - 1] = 100;
    TT_ASSERT_EQ(0, agc_effective(&a, 0));            /* 30-50 -> min */
    TT_ASSERT_EQ(30, agc_effective(&a, 1));
    TT_ASSERT_EQ(127, agc_effective(&a, NCH - 1));    /* 30+100 -> max */
    TT_ASSERT_EQ(0, agc_effective(&a, NCH));          /* out of range -> min_gain */
    TT_ASSERT_EQ(0, agc_effective(&a, 255));
    a.min_gain = 8;
    TT_ASSERT_EQ(8, agc_effective(&a, 0));
    TT_ASSERT_EQ(8, agc_effective(&a, NCH));
    a.cal_offset[1] = 5;
    TT_ASSERT_EQ(35, agc_effective(&a, 1));
    a.cal_offset[1] = -22;
    TT_ASSERT_EQ(8, agc_effective(&a, 1));            /* exactly min */
    a.cal_offset[1] = -23;
    TT_ASSERT_EQ(8, agc_effective(&a, 1));
    a.base = 255; a.max_gain = 255; a.cal_offset[1] = 127;
    TT_ASSERT_EQ(255, agc_effective(&a, 1));          /* no int8/uint8 wrap */
}

static void test_apply_first_writes_all_then_none(void)
{
    agc_t a;
    int dev[32], reg[32], val[32], n, g;
    fresh(&a);
    TT_ASSERT_EQ(NCH, agc_apply(&a));
    TT_ASSERT_EQ(NCH * 2, spi_count());      /* gain write + LD pulse per channel */
    n = gain_writes(dev, reg, val, 32);
    TT_ASSERT_EQ(NCH, n);
    for (g = 0; g < n; g++) {
        TT_ASSERT_EQ(g / 4, dev[g]);
        TT_ASSERT_EQ(REG_CH1_RX_GAIN + (g % 4), reg[g]);   /* 0-based channels */
        TT_ASSERT_EQ(30, val[g]);
        TT_ASSERT_EQ(30, a.written[g]);
    }
    mock_log_n = 0;
    TT_ASSERT_EQ(0, agc_apply(&a));
    TT_ASSERT_EQ(0, spi_count());
}

static void test_apply_only_changed_channels(void)
{
    agc_t a;
    int dev[32], reg[32], val[32];
    fresh(&a);
    agc_apply(&a);
    mock_log_n = 0;
    a.cal_offset[2] = 7;                    /* only channel 2 of dev 0 changes */
    TT_ASSERT_EQ(1, agc_apply(&a));
    TT_ASSERT_EQ(2, spi_count());
    TT_ASSERT_EQ(1, gain_writes(dev, reg, val, 32));
    TT_ASSERT_EQ(0, dev[0]);
    TT_ASSERT_EQ(REG_CH1_RX_GAIN + 2, reg[0]);
    TT_ASSERT_EQ(37, val[0]);
    mock_log_n = 0;
    a.base = 20;                            /* all change */
    TT_ASSERT_EQ(NCH, agc_apply(&a));
}

static void test_apply_clamped_offset_skips_write(void)
{
    /* base moves but the clamped effective value of a channel does not */
    agc_t a;
    fresh(&a);
    a.cal_offset[0] = -100;                 /* stays at min 0 while base 30 -> 26 */
    agc_apply(&a);
    mock_log_n = 0;
    a.base = 26;
    TT_ASSERT_EQ(NCH - 1, agc_apply(&a));
}

static void test_invalidate(void)
{
    agc_t a;
    int g;
    fresh(&a);
    agc_apply(&a);
    agc_invalidate(&a);
    for (g = 0; g < NCH; g++) TT_ASSERT_EQ(-1, a.written[g]);
    mock_log_n = 0;
    TT_ASSERT_EQ(NCH, agc_apply(&a));
    mock_log_n = 0;
    TT_ASSERT_EQ(0, agc_apply(&a));
}

static void test_apply_error_not_cached(void)
{
    agc_t a;
    fresh(&a);
    mock_spi_fail_next(-EIO);               /* first gain write fails */
    TT_ASSERT_EQ(-EIO, agc_apply(&a));
    TT_ASSERT_EQ(-1, a.written[0]);         /* failed channel stays unknown */
    mock_log_n = 0;
    TT_ASSERT_EQ(NCH, agc_apply(&a));       /* retried in full next time */
}

static void test_apply_error_midway(void)
{
    agc_t a;
    fresh(&a);
    agc_apply(&a);
    a.base = 31;
    mock_log_n = 0;
    mock_spi_fail_next(-ETIMEDOUT);
    TT_ASSERT_EQ(-ETIMEDOUT, agc_apply(&a));
    TT_ASSERT_EQ(30, a.written[0]);         /* not marked as written */
    TT_ASSERT_EQ(NCH, agc_apply(&a));
    TT_ASSERT_EQ(31, a.written[NCH - 1]);
}

/* ---- agc_tick ---- */
static void set_dig(int sat, int en)
{
    mock_gpio_set_input(PIN_FPGA_DIG5, sat);
    mock_gpio_set_input(PIN_FPGA_DIG6, en);
}

static void frame(void) { mock_time_advance_us(AGC_PERIOD_MS * 1000u); }

static void test_tick_period(void)
{
    agc_t a;
    fresh(&a);
    set_dig(0, 1);
    agc_tick(&a);                           /* t=0: before the first period */
    TT_ASSERT_EQ(0, mock_gpio_port_reads);
    mock_time_advance_us(AGC_PERIOD_MS * 1000u - 1);
    agc_tick(&a);
    TT_ASSERT_EQ(0, mock_gpio_port_reads);
    mock_time_advance_us(1);
    agc_tick(&a);
    TT_ASSERT_EQ(1, mock_gpio_port_reads);  /* exactly one IDR read per frame */
    agc_tick(&a);                           /* same instant: not again */
    TT_ASSERT_EQ(1, mock_gpio_port_reads);
}

static void test_tick_debounce_then_run(void)
{
    agc_t a;
    fresh(&a);
    set_dig(1, 1);                          /* DIG6=1, saturated */
    frame(); agc_tick(&a);                  /* 1st sample: not confirmed */
    TT_ASSERT_EQ(0, a.enabled);
    TT_ASSERT_EQ(0, spi_count());
    TT_ASSERT_EQ(30, a.base);
    frame(); agc_tick(&a);                  /* 2nd agreeing sample: enabled, update+apply */
    TT_ASSERT_EQ(1, a.enabled);
    TT_ASSERT_EQ(26, a.base);
    TT_ASSERT_EQ(NCH * 2, spi_count());     /* first apply writes everything */
    set_dig(1, 1);
    mock_log_n = 0;
    frame(); agc_tick(&a);
    TT_ASSERT_EQ(22, a.base);
    TT_ASSERT_EQ(NCH * 2, spi_count());     /* base changed -> all channels rewritten */
}

static void test_tick_no_write_when_unchanged(void)
{
    agc_t a;
    fresh(&a);
    a.holdoff_frames = 4;
    set_dig(0, 1);
    frame(); agc_tick(&a);
    frame(); agc_tick(&a);                  /* enabled, clear #1: base unchanged but first apply */
    TT_ASSERT_EQ(1, a.enabled);
    mock_log_n = 0;
    frame(); agc_tick(&a);                  /* clear #2 */
    frame(); agc_tick(&a);                  /* clear #3 */
    TT_ASSERT_EQ(0, spi_count());           /* nothing changed, nothing written */
    frame(); agc_tick(&a);                  /* clear #4 -> +1 */
    TT_ASSERT_EQ(NCH * 2, spi_count());
}

static void test_tick_fpga_disable(void)
{
    agc_t a;
    fresh(&a);
    set_dig(0, 1);
    frame(); agc_tick(&a);
    frame(); agc_tick(&a);
    TT_ASSERT_EQ(1, a.enabled);
    set_dig(1, 0);                          /* FPGA turns AGC off while saturated */
    frame(); agc_tick(&a);                  /* glitch window: still enabled */
    frame(); agc_tick(&a);                  /* confirmed off */
    TT_ASSERT_EQ(0, a.enabled);
    {
        uint8_t base = a.base;
        mock_log_n = 0;
        frame(); agc_tick(&a);
        TT_ASSERT_EQ(base, a.base);         /* disabled: no update */
        TT_ASSERT_EQ(0, spi_count());
    }
}

int main(void)
{
    TT_RUN(test_defaults);
    TT_RUN(test_attack);
    TT_RUN(test_attack_floor);
    TT_RUN(test_holdoff_and_recovery);
    TT_RUN(test_saturation_resets_holdoff);
    TT_RUN(test_ceiling);
    TT_RUN(test_holdoff_zero_steps_every_frame);
    TT_RUN(test_disabled_is_noop);
    TT_RUN(test_effective_clamp);
    TT_RUN(test_apply_first_writes_all_then_none);
    TT_RUN(test_apply_only_changed_channels);
    TT_RUN(test_apply_clamped_offset_skips_write);
    TT_RUN(test_invalidate);
    TT_RUN(test_apply_error_not_cached);
    TT_RUN(test_apply_error_midway);
    TT_RUN(test_tick_period);
    TT_RUN(test_tick_debounce_then_run);
    TT_RUN(test_tick_no_write_when_unchanged);
    TT_RUN(test_tick_fpga_disable);
    return TT_RESULT();
}
