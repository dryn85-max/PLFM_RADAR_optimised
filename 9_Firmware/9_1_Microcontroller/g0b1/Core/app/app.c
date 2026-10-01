#include "app.h"
#include "adar1000.h"
#include "beam.h"
#include "cmd.h"
#include "config.h"
#include "diag_log.h"
#include "fault.h"
#include "fpga_if.h"
#include "hal_gpio.h"
#include "hal_time.h"
#include "hal_uart.h"
#include "pll_lo.h"
#include "sequencer.h"
#include "strfmt.h"
#include "thermal.h"

#define PLL_CHECK_PERIOD_MS 100
#define UART_BURST          64   /* max characters parsed per loop pass */

static agc_t    s_agc;
static uint32_t s_pll_check_ms;

agc_t *app_agc(void) { return &s_agc; }

static void say_boot_latched(fault_t f)
{
    char buf[40];
    sbuf_t sb;
    sb_init(&sb, buf, sizeof buf);
    sb_puts(&sb, "BOOT latched fault=");
    sb_put_int(&sb, (int)f);
    sb_puts(&sb, "\r\n");
    (void)uart_write(buf, sb.len);
}

void app_init(void)
{
    uint8_t d;
    int16_t warmup;
    int rc;

    fault_init();
    agc_init(&s_agc);
    cmd_init(&s_agc);
    if (fault_is_latched()) {
        sequencer_emergency_stop();   /* make sure everything is off */
        say_boot_latched(fault_latched_code());
        return;
    }

    /* Base rails, FPGA held in reset (DIG4 low). */
    fpga_if_init();
    (void)sequencer_power_up();
    iwdg_refresh();

    /* LO PLL must lock before any RF rail is considered. */
    rc = pll_init(pll_default_table());
    if (rc == 0) {
        rc = pll_wait_lock(PLL_LOCK_TIMEOUT_MS);
    }
    if (rc != 0) {
        DIAG_ERR("PLL", "lock failed (%d)", rc);
        fault_raise(FAULT_PLL_LOCK);
        return;
    }
    iwdg_refresh();

    /* ADAR1000s in ascending order (a reset of device 0 resets the whole bus). */
    for (d = 0; d < ADAR_COUNT; d++) {
        rc = adar_init(d);
        if (rc == 0) {
            rc = adar_set_safe_bias(d);
        }
        if (rc != 0) {
            DIAG_ERR("BF", "ADAR%u init failed (%d)", (unsigned)d, rc);
            fault_raise(FAULT_ADAR_COMM);
            return;
        }
    }
    agc_invalidate(&s_agc);
    iwdg_refresh();

    /* Safe bias is in place: LNA 3V3, then PA 5 V. */
    (void)sequencer_rf_up();
    for (d = 0; d < ADAR_COUNT; d++) {
        rc = adar_set_operational_bias(d);
        if (rc == 0) {
            rc = adar_set_mode(d, ADAR_MODE_TR_PIN);   /* adar_init leaves SPI TR control */
        }
        if (rc != 0) {
            DIAG_ERR("BF", "ADAR%u bias/mode failed (%d)", (unsigned)d, rc);
            fault_raise(FAULT_ADAR_COMM);
            return;
        }
    }
    rc = beam_apply(0);
    if (rc != 0) {
        DIAG_ERR("BF", "beam init failed (%d)", rc);
        fault_raise(FAULT_ADAR_COMM);
        return;
    }
    iwdg_refresh();

    fpga_if_reset_pulse();
    fpga_if_set_mixers(1);

    /* The ADS7830 internal reference turns on only after the first PD1=1
     * command: do one throw-away read now, ignore result and errors. */
    (void)thermal_read_deci_c(&warmup);
    thermal_init();
    s_agc.last_tick_ms = millis();   /* AGC frame timer starts now (no spurious tick at t=0) */
    s_pll_check_ms = millis();
    DIAG("SYS", "init done");
}

void app_loop(void)
{
    int c, n = 0;

    while (n < UART_BURST && (c = uart_getc()) >= 0) {
        cmd_feed(c);
        n++;
    }
    if (fault_active() == FAULT_NONE) {
        agc_tick(&s_agc);
    }
    if (fault_active() == FAULT_NONE) {
        thermal_tick();
    }
    if (fault_active() == FAULT_NONE &&
        (uint32_t)(millis() - s_pll_check_ms) >= PLL_CHECK_PERIOD_MS) {
        s_pll_check_ms = millis();
        if (!pll_is_locked()) {
            DIAG_ERR("PLL", "lock lost");
            fault_raise(FAULT_PLL_LOCK);
        }
    }
    iwdg_refresh();
}
