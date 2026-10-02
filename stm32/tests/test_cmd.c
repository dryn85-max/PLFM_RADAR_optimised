/* Host tests for cmd.c: command grammar, ranges, latched behaviour, status line.
 * Links the real fault/sequencer/adar1000/beam/agc/thermal modules over the mocks. */
#include <errno.h>
#include <string.h>
#include "tinytest.h"
#include "mock_log.h"
#include "cmd.h"
#include "agc.h"
#include "adar1000.h"
#include "fault.h"
#include "thermal.h"
#include "fpga_if.h"
#include "hal_gpio.h"
#include "hal_time.h"
#include "config.h"

#define NCH (ADAR_COUNT * 4)

static agc_t g_agc;
static char  g_out[256];

static void fresh(void)
{
    mock_reset();
    adar_test_reset_state();
    fault_test_power_cycle();
    fault_init();
    fpga_if_init();
    agc_init(&g_agc);
    thermal_init();
    cmd_init(&g_agc);
    mock_log_n = 0;
}

static const char *run(const char *line)
{
    cmd_exec(line, g_out, sizeof g_out);
    return g_out;
}

static int count_kind(int kind)
{
    int i, n = 0;
    for (i = 0; i < mock_log_n; i++) if (mock_log[i].kind == kind) n++;
    return n;
}

static int toggles_of(int pin)
{
    int i, n = 0;
    for (i = 0; i < mock_log_n; i++)
        if (mock_log[i].kind == MOCK_EV_GPIO_TOGGLE && mock_log[i].a == pin) n++;
    return n;
}

static void expect(const char *line, const char *reply)
{
    const char *r = run(line);
    if (strcmp(r, reply) != 0) {
        printf("  line \"%s\": expected \"%s\", got \"%s\"\n", line, reply, r);
    }
    TT_ASSERT(strcmp(r, reply) == 0);
}

static void test_beam_valid(void)
{
    fresh();
    expect("beam 0 0", "OK");
    TT_ASSERT(count_kind(MOCK_EV_SPI) > 0);
    TT_ASSERT_EQ(0, toggles_of(PIN_FPGA_DIG1));   /* el unchanged */
    TT_ASSERT_EQ(0, toggles_of(PIN_FPGA_DIG2));   /* az unchanged */
    mock_log_n = 0;
    expect("beam 10 -20", "OK");
    TT_ASSERT_EQ(1, toggles_of(PIN_FPGA_DIG1));
    TT_ASSERT_EQ(1, toggles_of(PIN_FPGA_DIG2));
    TT_ASSERT_EQ(0, toggles_of(PIN_FPGA_DIG0));
    mock_log_n = 0;
    expect("beam 10 -20", "OK");                   /* same again: no toggles */
    TT_ASSERT_EQ(0, toggles_of(PIN_FPGA_DIG1) + toggles_of(PIN_FPGA_DIG2));
    expect("beam -180 60", "OK");                  /* limits */
    expect("beam 180 -60", "OK");
    expect("beam +5  -5", "OK");                   /* explicit plus, double space */
}

static void test_beam_invalid(void)
{
    fresh();
    expect("beam", "ERR args");
    expect("beam 1", "ERR args");
    expect("beam 1 2 3", "ERR args");
    expect("beam 1x 2", "ERR args");
    expect("beam 1 2x", "ERR args");
    expect("beam - 0", "ERR args");
    expect("beam 1.5 0", "ERR args");
    expect("beam 181 0", "ERR range");
    expect("beam -181 0", "ERR range");
    expect("beam 0 61", "ERR range");
    expect("beam 0 -61", "ERR range");
    expect("beam 99999999999999 0", "ERR range");  /* oversized saturates, never wraps into range */
    TT_ASSERT_EQ(0, count_kind(MOCK_EV_SPI));       /* nothing sent for any invalid form */
    TT_ASSERT_EQ(0, count_kind(MOCK_EV_GPIO_TOGGLE));
}

static void test_beam_spi_error_keeps_state(void)
{
    fresh();
    mock_spi_fail_next(-EIO);
    expect("beam 10 20", "ERR spi");
    TT_ASSERT_EQ(0, count_kind(MOCK_EV_GPIO_TOGGLE));
    TT_ASSERT(strstr(run("status"), " az=0 el=0 ") != NULL);
}

/* M6: a beam write that fails part-way answers ERR spi, changes no state, never
 * toggles DIG1/DIG2, and best-effort re-applies the previous elevation. */
static void test_beam_partial_failure_reapplies_previous(void)
{
    int i, spi = 0, per_beam = ADAR_COUNT * 4 * 2 * 3;   /* RX+TX phase, I/Q/load each */
    fresh();
    expect("beam 10 20", "OK");
    TT_ASSERT(strstr(run("status"), " az=10 el=20 ") != NULL);
    mock_log_n = 0;
    mock_spi_fail_after(4, -EIO);                      /* fails mid-way through the new table */
    expect("beam -30 -40", "ERR spi");
    TT_ASSERT_EQ(0, count_kind(MOCK_EV_GPIO_TOGGLE));
    TT_ASSERT(strstr(run("status"), " az=10 el=20 ") != NULL);
    for (i = 0; i < mock_log_n; i++) if (mock_log[i].kind == MOCK_EV_SPI) spi++;
    TT_ASSERT_EQ(5 + per_beam, spi);                   /* 4 ok + 1 failed + full re-apply of el=20 */
}

static void test_gain_valid(void)
{
    int i, first = -1;
    fresh();
    expect("gain 2 45", "OK");
    TT_ASSERT_EQ(45, g_agc.written[2]);
    TT_ASSERT_EQ(-1, g_agc.written[1]);
    for (i = 0; i < mock_log_n; i++)
        if (mock_log[i].kind == MOCK_EV_SPI) { first = i; break; }
    TT_ASSERT(first >= 0);
    if (first >= 0) {
        TT_ASSERT_EQ(0, mock_log[first].bytes[0] >> 5);
        TT_ASSERT_EQ(REG_CH1_RX_GAIN + 2, mock_log[first].bytes[1]);
        TT_ASSERT_EQ(45, mock_log[first].bytes[2]);
    }
    mock_log_n = 0;
    expect("gain 0 0", "OK");
    expect("gain 1 127", "OK");
    TT_ASSERT_EQ(0, g_agc.written[0]);
    TT_ASSERT_EQ(127, g_agc.written[1]);
    /* highest channel: device (NCH-1)/4, register REG_CH1_RX_GAIN + 3 */
    {
        char line[32];
        int last = NCH - 1, k;
        mock_log_n = 0;
        strcpy(line, "gain ");
        k = (int)strlen(line);
        if (last >= 10) line[k++] = (char)('0' + last / 10);
        line[k++] = (char)('0' + last % 10);
        strcpy(line + k, " 9");
        expect(line, "OK");
        TT_ASSERT_EQ(9, g_agc.written[last]);
        TT_ASSERT_EQ(last / 4, mock_log[0].bytes[0] >> 5);
        TT_ASSERT_EQ(REG_CH1_RX_GAIN + 3, mock_log[0].bytes[1]);
    }
}

static void test_gain_invalid(void)
{
    char line[32];
    fresh();
    expect("gain", "ERR args");
    expect("gain 0", "ERR args");
    expect("gain 0 1 2", "ERR args");
    expect("gain a 1", "ERR args");
    expect("gain 0 1x", "ERR args");
    expect("gain 0 128", "ERR range");
    expect("gain 0 -1", "ERR range");
    expect("gain -1 5", "ERR range");
    expect("gain 99999999999 5", "ERR range");
    strcpy(line, "gain 16 5");
    line[5] = (char)('0' + NCH / 10);
    line[6] = (char)('0' + NCH % 10);   /* "gain 04/16 5": first out-of-range channel */
    expect(line, "ERR range");
    TT_ASSERT_EQ(0, count_kind(MOCK_EV_SPI));
}

static void test_gain_spi_error(void)
{
    fresh();
    g_agc.written[1] = 50;
    mock_spi_fail_next(-EIO);
    expect("gain 1 60", "ERR spi");
    TT_ASSERT_EQ(-1, g_agc.written[1]);   /* unknown after a failed write */
}

static void test_gain_then_agc_writes_only_differences(void)
{
    fresh();
    expect("gain 2 45", "OK");
    g_agc.base = 45;                      /* effective 45 everywhere (cal_offset 0) */
    mock_log_n = 0;
    TT_ASSERT_EQ(NCH - 1, agc_apply(&g_agc));   /* channel 2 already holds 45 */
}

static int last_sw_ctrl_write(void)
{
    int i, v = -1;
    for (i = 0; i < mock_log_n; i++) {
        const mock_event_t *e = &mock_log[i];
        if (e->kind == MOCK_EV_SPI && (((e->bytes[0] & 0x1F) << 8) | e->bytes[1]) == REG_SW_CONTROL)
            v = e->bytes[2];
    }
    return v;
}

static int sw_ctrl_writes(void)
{
    int i, n = 0;
    for (i = 0; i < mock_log_n; i++) {
        const mock_event_t *e = &mock_log[i];
        if (e->kind == MOCK_EV_SPI && (((e->bytes[0] & 0x1F) << 8) | e->bytes[1]) == REG_SW_CONTROL) n++;
    }
    return n;
}

static void test_tx_rx_auto(void)
{
    int v;
    fresh();
    expect("tx", "OK");
    TT_ASSERT_EQ(ADAR_COUNT, sw_ctrl_writes());   /* every device, one write each */
    v = last_sw_ctrl_write();
    TT_ASSERT((v & SW_CTRL_TR_SOURCE) == 0);
    TT_ASSERT((v & SW_CTRL_TX_EN) != 0 && (v & SW_CTRL_TR_SPI) != 0);
    TT_ASSERT(strstr(run("status"), " mode=tx ") != NULL);
    mock_log_n = 0;
    expect("rx", "OK");
    v = last_sw_ctrl_write();
    TT_ASSERT((v & SW_CTRL_TR_SOURCE) == 0);
    TT_ASSERT((v & SW_CTRL_RX_EN) != 0 && (v & SW_CTRL_TX_EN) == 0);
    TT_ASSERT(strstr(run("status"), " mode=rx ") != NULL);
    mock_log_n = 0;
    expect("auto", "OK");
    TT_ASSERT_EQ(ADAR_COUNT, sw_ctrl_writes());
    v = last_sw_ctrl_write();
    TT_ASSERT((v & SW_CTRL_TR_SOURCE) != 0);
    TT_ASSERT(strstr(run("status"), " mode=auto ") != NULL);
    mock_spi_fail_next(-EIO);
    expect("tx", "ERR spi");
    TT_ASSERT(strstr(run("status"), " mode=auto ") != NULL);   /* mode unchanged on failure */
}

/* M5: STATUS mode is the ADAR mode actually set, "none" before any adar_set_mode. */
static void test_status_mode_none_before_init(void)
{
    fresh();
    TT_ASSERT(strstr(run("status"), " mode=none ") != NULL);
    mock_spi_fail_next(-EIO);
    expect("auto", "ERR spi");
    TT_ASSERT(strstr(run("status"), " mode=none ") != NULL);   /* a failed set does not claim a mode */
    expect("auto", "OK");
    TT_ASSERT(strstr(run("status"), " mode=auto ") != NULL);
}

#if ADAR_COUNT > 1
static void test_status_mode_mixed_after_partial_failure(void)
{
    fresh();
    expect("tx", "OK");
    mock_spi_fail_after(1, -EIO);                      /* dev 0 takes "rx", dev 1 fails */
    expect("rx", "ERR spi");
    TT_ASSERT(strstr(run("status"), " mode=mixed ") != NULL);
    expect("rx", "OK");
    TT_ASSERT(strstr(run("status"), " mode=rx ") != NULL);
}
#endif

static void test_args_and_unknown(void)
{
    fresh();
    expect("tx 1", "ERR args");
    expect("rx x", "ERR args");
    expect("auto 1", "ERR args");
    expect("status 1", "ERR args");
    expect("stop now", "ERR args");
    TT_ASSERT_EQ(0, fault_is_latched());          /* "stop now" must not stop */
    expect("foo", "ERR unknown");
    expect("TX", "ERR unknown");                   /* case-sensitive */
    expect("beam1 0 0", "ERR unknown");
    expect("tx 1 2 3 4 5 6", "ERR args");
    TT_ASSERT_EQ(0, count_kind(MOCK_EV_SPI));
}

static void test_blank_lines(void)
{
    fresh();
    TT_ASSERT_EQ(0, cmd_exec("", g_out, sizeof g_out));
    TT_ASSERT_EQ(0, cmd_exec("   ", g_out, sizeof g_out));
    TT_ASSERT_EQ(0, g_out[0]);
}

static void test_stop(void)
{
    fresh();
    gpio_write(PIN_EN_PA, 1);
    gpio_write(PIN_EN_FPGA, 1);
    expect("stop", "OK stopped");
    TT_ASSERT_EQ(1, fault_is_latched());
    TT_ASSERT_EQ(FAULT_ESTOP_CMD, fault_latched_code());
    TT_ASSERT_EQ(0, gpio_read(PIN_EN_PA));
    TT_ASSERT_EQ(0, gpio_read(PIN_EN_FPGA));
    TT_ASSERT(strstr(run("status"), " fault=11 latched=1 ") != NULL);
}

static void test_latched_rejects_all_but_status(void)
{
    fresh();
    fault_raise(FAULT_OVERTEMP);
    mock_log_n = 0;
    expect("beam 0 0", "ERR latched");
    expect("gain 0 1", "ERR latched");
    expect("tx", "ERR latched");
    expect("rx", "ERR latched");
    expect("auto", "ERR latched");
    expect("stop", "ERR latched");
    expect("foo", "ERR latched");
    TT_ASSERT_EQ(0, count_kind(MOCK_EV_SPI));
    TT_ASSERT(strncmp(run("status"), "STATUS ", 7) == 0);
    TT_ASSERT(strstr(g_out, " fault=10 latched=1 ") != NULL);
    /* still latched after a simulated watchdog reset */
    fault_test_simulate_reset();
    expect("tx", "ERR latched");
}

static void test_nonlatched_fault_commands_still_work(void)
{
    fresh();
    fault_raise(FAULT_PLL_LOCK);
    expect("tx", "OK");
    TT_ASSERT(strstr(run("status"), " fault=1 latched=0 ") != NULL);
}

static void build_status(char *dst, const char *head, const int *gains)
{
    int g;
    strcpy(dst, head);
    strcat(dst, " gains=");
    for (g = 0; g < NCH; g++) {
        char t[8];
        int v = gains[g];
        size_t k = 0;
        if (g > 0) strcat(dst, ",");
        if (v < 0) { t[k++] = '-'; v = -v; }
        if (v >= 100) t[k++] = (char)('0' + v / 100);
        if (v >= 10) t[k++] = (char)('0' + (v / 10) % 10);
        t[k++] = (char)('0' + v % 10);
        t[k] = '\0';
        strcat(dst, t);
    }
}

static void test_status_exact(void)
{
    char want[300];
    int gains[16], g;
    uint8_t raw = 76;
    fresh();
    for (g = 0; g < 16; g++) gains[g] = -1;
    build_status(want, "STATUS lock=0 temp=0 temp_err=0 fault=0 latched=0 mode=none az=0 el=0 agc=0 base=30", gains);
    expect("status", want);
    TT_ASSERT(strlen(g_out) < 200);

    mock_gpio_set_input(PIN_PLL_LD, 1);
    mock_time_advance_us(5000u * 1000u);
    mock_i2c_set_rx(&raw, 1);
    thermal_tick();                                   /* 76 -> 37.1 C */
    expect("tx", "OK");
    expect("beam 10 -20", "OK");
    expect("gain 2 45", "OK");
    g_agc.enabled = 1;
    g_agc.base = 25;
    gains[2] = 45;
    build_status(want, "STATUS lock=1 temp=371 temp_err=0 fault=0 latched=0 mode=tx az=10 el=-20 agc=1 base=25", gains);
    expect("status", want);
}

static void test_status_temp_err(void)
{
    fresh();
    mock_time_advance_us(5000u * 1000u);
    mock_i2c_fail_next(-EIO);
    thermal_tick();
    TT_ASSERT(strstr(run("status"), " temp=0 temp_err=1 ") != NULL);
}

static void feed(const char *s) { while (*s) cmd_feed((unsigned char)*s++); }

/* Existing feed tests assert the TX stream is the reply only: switch echo off first. */
static void quiet(void)
{
    feed("echo off\r");
    mock_reset();
}

static void test_feed_line_assembly(void)
{
    fresh();
    quiet();
    feed("status\r\n");                               /* CRLF: one reply, blank line ignored */
    TT_ASSERT(strncmp(mock_uart_tx, "STATUS ", 7) == 0);
    TT_ASSERT(strstr(mock_uart_tx, "\r\n") != NULL);
    TT_ASSERT_EQ(0, strstr(mock_uart_tx, "\r\n")[2]);   /* nothing after the single reply */
    mock_reset();
    cmd_init(&g_agc);
    quiet();
    feed("foo\n");
    TT_ASSERT(strcmp(mock_uart_tx, "ERR unknown\r\n") == 0);
    mock_reset();
    feed("fo");
    feed("o\r");
    TT_ASSERT(strcmp(mock_uart_tx, "ERR unknown\r\n") == 0);
    mock_reset();
    feed("\r\n\r\n");                                  /* blank lines: no reply */
    TT_ASSERT_EQ(0, mock_uart_tx[0]);
}

static void test_feed_too_long(void)
{
    char line[80];
    int i;
    fresh();
    quiet();
    for (i = 0; i < 63; i++) line[i] = 'x';
    line[63] = '\0';
    feed(line);                                       /* exactly 63: accepted, unknown command */
    feed("\n");
    TT_ASSERT(strcmp(mock_uart_tx, "ERR unknown\r\n") == 0);
    mock_reset();
    cmd_init(&g_agc);
    quiet();
    for (i = 0; i < 64; i++) line[i] = 'x';
    line[64] = '\0';
    feed(line);                                       /* 64: too long */
    feed("\n");
    TT_ASSERT(strcmp(mock_uart_tx, "ERR too_long\r\n") == 0);
    mock_reset();
    cmd_init(&g_agc);
    quiet();
    feed("beam 0 0 ");                                /* long junk then a valid line after it */
    for (i = 0; i < 100; i++) cmd_feed('9');
    feed("\r\n");
    TT_ASSERT(strcmp(mock_uart_tx, "ERR too_long\r\n") == 0);
    mock_reset();
    feed("tx\n");
    TT_ASSERT(strcmp(mock_uart_tx, "OK\r\n") == 0);   /* parser recovered, tail not executed */
    /* direct call with an over-long line */
    TT_ASSERT(strcmp(run("0123456789012345678901234567890123456789012345678901234567890123"),
                     "ERR too_long") == 0);
}

/* ---- console echo and line editing ---- */

static int tx_eq(const char *s)
{
    if (strcmp(mock_uart_tx, s) != 0) {
        printf("  TX expected \"%s\", got \"%s\"\n", s, mock_uart_tx);
        return 0;
    }
    return 1;
}

void mock_uart_reset(void);                       /* defined in mocks/mock_uart.c */
static void tx_clear(void) { mock_uart_reset(); }

static void test_echo_default_on(void)
{
    fresh();
    feed("st");
    TT_ASSERT(tx_eq("st"));
    feed("atus\r");
    TT_ASSERT(strncmp(mock_uart_tx, "status\r\nSTATUS ", 15) == 0);
}

static void test_echo_crlf_and_lf(void)
{
    fresh();
    feed("foo\r\n");                                  /* LF after CR ignored: one line, no blank echo */
    TT_ASSERT(tx_eq("foo\r\nERR unknown\r\n"));
    tx_clear();
    feed("foo\n");                                    /* LF alone ends a line */
    TT_ASSERT(tx_eq("foo\r\nERR unknown\r\n"));
    tx_clear();
    feed("\r\n\r\n");                                 /* blank lines: echo CRLF, LF after CR ignored */
    TT_ASSERT(tx_eq("\r\n\r\n"));
    tx_clear();
    feed("\n\n");                                     /* LF after LF is a separate (blank) line */
    TT_ASSERT(tx_eq("\r\n\r\n"));
    tx_clear();
    feed("status\r");
    feed("\ntx\r");                                   /* LF right after CR is swallowed, next line works */
    TT_ASSERT(strstr(mock_uart_tx, "tx\r\nOK\r\n") != NULL);
    TT_ASSERT(strstr(mock_uart_tx, "\r\n\r\n") == NULL);
}

static void test_edit_backspace_del(void)
{
    fresh();
    feed("stx\x7f" "atus\r");
    TT_ASSERT(strstr(mock_uart_tx, "stx\b \batus\r\nSTATUS ") == mock_uart_tx);
    tx_clear();
    feed("stx\x08" "atus\r");
    TT_ASSERT(strstr(mock_uart_tx, "stx\b \batus\r\nSTATUS ") == mock_uart_tx);
    tx_clear();
    feed("\x08\x7f\x08");                             /* empty line: nothing */
    TT_ASSERT(tx_eq(""));
    feed("\x7f" "tx\x08\x08\x08\x08rx\r");            /* extra BS stop at length 0 */
    TT_ASSERT(tx_eq("tx\b \b\b \brx\r\nOK\r\n"));
    TT_ASSERT_EQ(ADAR_MODE_SPI_RX, adar_get_mode(0));
}

static void test_edit_backspace_echo_off(void)
{
    fresh();
    feed("echo off\r");
    tx_clear();
    feed("stx\x7f" "atus\r");
    TT_ASSERT(strncmp(mock_uart_tx, "STATUS ", 7) == 0);   /* edited, but no echo at all */
    TT_ASSERT(strchr(mock_uart_tx, '\b') == NULL);
}

static void test_edit_backspace_overflow(void)
{
    int i;
    fresh();
    feed("echo off\r");
    tx_clear();
    for (i = 0; i < 64; i++) cmd_feed('x');           /* 64th char: overflow */
    feed("\x7f\x7f\x08");                             /* BS during overflow: ignored */
    feed("\r");
    TT_ASSERT(tx_eq("ERR too_long\r\n"));
    tx_clear();
    feed("echo on\r");
    tx_clear();
    for (i = 0; i < 64; i++) cmd_feed('x');
    feed("\x7f");
    TT_ASSERT_EQ(63, (int)strlen(mock_uart_tx));       /* 63 echoed, 64th not, BS silent */
    feed("\r");
    TT_ASSERT(tx_eq("xxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxx\r\nERR too_long\r\n"));
    tx_clear();
    feed("tx\r");                                     /* recovered */
    TT_ASSERT(tx_eq("tx\r\nOK\r\n"));
}

static void test_edit_backspace_at_63(void)
{
    int i;
    fresh();
    for (i = 0; i < 63; i++) cmd_feed('x');
    tx_clear();
    feed("\x7f");                                     /* full line, BS works, not overflow */
    TT_ASSERT(tx_eq("\b \b"));
    feed("x\r");
    TT_ASSERT(strstr(mock_uart_tx, "ERR unknown\r\n") != NULL);
}

static void test_control_chars_dropped(void)
{
    fresh();
    feed("\x01\x07\x09\x1f\x80\xff\xc3");
    TT_ASSERT(tx_eq(""));
    feed("tx\x01\x09\x80\r");
    TT_ASSERT(tx_eq("tx\r\nOK\r\n"));                  /* never entered the line */
}

static void test_escape_sequences_exact(void)
{
    fresh();
    feed("t\x1b[A" "x\r");
    TT_ASSERT(tx_eq("tx\r\nOK\r\n"));
    tx_clear();
    feed("t\x1b[1;5Cx\r");
    TT_ASSERT(tx_eq("tx\r\nOK\r\n"));
    tx_clear();
    feed("t\x1bOPx\r");
    TT_ASSERT(tx_eq("tx\r\nOK\r\n"));
    tx_clear();
    feed("t\x1bzx\r");                                /* ESC + one byte */
    TT_ASSERT(tx_eq("tx\r\nOK\r\n"));
    tx_clear();
    feed("t\x1b\x7f" "x\r");                          /* ESC + DEL: DEL is the sequence byte, not an edit */
    TT_ASSERT(tx_eq("tx\r\nOK\r\n"));
}

static void test_escape_cr_aborts(void)
{
    fresh();
    feed("tx\x1b[1\r");                               /* CR inside CSI ends the line normally */
    TT_ASSERT(tx_eq("tx\r\nOK\r\n"));
    tx_clear();
    feed("tx\x1b\r");                                 /* CR right after ESC */
    TT_ASSERT(tx_eq("tx\r\nOK\r\n"));
    tx_clear();
    feed("tx\x1bO\n");                                /* LF inside SS3 */
    TT_ASSERT(tx_eq("tx\r\nOK\r\n"));
    tx_clear();
    feed("tx\x1b\x1b[Ax\r");                          /* ESC ESC: second ESC is the one byte; "[Ax" is text */
    TT_ASSERT(tx_eq("tx[Ax\r\nERR unknown\r\n"));
    tx_clear();
    feed("rx\r");                                     /* state machine back to normal */
    TT_ASSERT(tx_eq("rx\r\nOK\r\n"));
}

static void test_echo_overlong_not_echoed(void)
{
    int i;
    fresh();
    for (i = 0; i < 63; i++) cmd_feed('x');
    TT_ASSERT_EQ(63, (int)strlen(mock_uart_tx));
    cmd_feed('y');
    cmd_feed('z');
    TT_ASSERT_EQ(63, (int)strlen(mock_uart_tx));       /* beyond CMD_MAX_LINE: silent */
}

static void test_echo_command(void)
{
    fresh();
    feed("echo\r");
    TT_ASSERT(tx_eq("echo\r\nECHO on\r\n"));
    tx_clear();
    feed("echo off\r");
    TT_ASSERT(tx_eq("echo off\r\nOK\r\n"));            /* line end is echoed before the switch applies */
    tx_clear();
    feed("echo\r");
    TT_ASSERT(tx_eq("ECHO off\r\n"));
    tx_clear();
    feed("tx\r");
    TT_ASSERT(tx_eq("OK\r\n"));                        /* no echo, no CRLF echo */
    tx_clear();
    feed("echo on\n");
    TT_ASSERT(tx_eq("OK\r\n"));                        /* echo turns on after the line was read */
    tx_clear();
    feed("tx\r");
    TT_ASSERT(tx_eq("tx\r\nOK\r\n"));
    tx_clear();
    feed("echo maybe\r");
    TT_ASSERT(strstr(mock_uart_tx, "ERR args\r\n") != NULL);
    tx_clear();
    feed("echo on x\r");
    TT_ASSERT(strstr(mock_uart_tx, "ERR args\r\n") != NULL);
    tx_clear();
    feed("echo off x y z\r");
    TT_ASSERT(strstr(mock_uart_tx, "ERR args\r\n") != NULL);
    tx_clear();
    feed("echo ON\r");                                /* case-sensitive */
    TT_ASSERT(strstr(mock_uart_tx, "ERR args\r\n") != NULL);
}

static void test_echo_cmd_exec_direct(void)
{
    fresh();
    expect("echo", "ECHO on");
    expect("echo off", "OK");
    expect("echo", "ECHO off");
    expect("echo on", "OK");
    expect("echo 1", "ERR args");
    expect("echo on off", "ERR args");
}

static void test_echo_latched(void)
{
    fresh();
    feed("stop\r");
    tx_clear();
    feed("echo off\r");
    TT_ASSERT(strstr(mock_uart_tx, "OK\r\n") != NULL);
    tx_clear();
    feed("tx\r");
    TT_ASSERT(tx_eq("ERR latched\r\n"));
    tx_clear();
    feed("status\r");
    TT_ASSERT(strncmp(mock_uart_tx, "STATUS ", 7) == 0);
    tx_clear();
    feed("echo\r");
    TT_ASSERT(tx_eq("ECHO off\r\n"));
    tx_clear();
    feed("echo bad\r");
    TT_ASSERT(tx_eq("ERR args\r\n"));
}

static void test_init_resets_echo(void)
{
    fresh();
    feed("echo off\r");
    cmd_init(&g_agc);
    tx_clear();
    feed("x");
    TT_ASSERT(tx_eq("x"));
    feed("x\r");                                      /* s_last_cr = 1 */
    cmd_init(&g_agc);
    tx_clear();
    feed("\n");                                       /* init cleared it: this LF is a blank line, not swallowed */
    TT_ASSERT(tx_eq("\r\n"));                         /* echo CRLF, empty line produces no reply */
    feed("\x1b");                                     /* pending ESC */
    cmd_init(&g_agc);
    tx_clear();
    feed("z\r");                                      /* init cleared it: 'z' is echoed, not eaten as an escape final byte */
    TT_ASSERT(strncmp(mock_uart_tx, "z\r\n", 3) == 0);
    TT_ASSERT(tx_eq("z\r\nERR unknown\r\n"));
    feed("\x1b[");                                    /* half-received CSI */
    cmd_init(&g_agc);
    tx_clear();
    feed("\ntx\r");                                   /* leading LF is a line end, not swallowed */
    TT_ASSERT(tx_eq("\r\ntx\r\nOK\r\n"));
}


int main(void)
{
    TT_RUN(test_beam_valid);
    TT_RUN(test_beam_invalid);
    TT_RUN(test_beam_spi_error_keeps_state);
    TT_RUN(test_beam_partial_failure_reapplies_previous);
    TT_RUN(test_gain_valid);
    TT_RUN(test_gain_invalid);
    TT_RUN(test_gain_spi_error);
    TT_RUN(test_gain_then_agc_writes_only_differences);
    TT_RUN(test_tx_rx_auto);
    TT_RUN(test_status_mode_none_before_init);
#if ADAR_COUNT > 1
    TT_RUN(test_status_mode_mixed_after_partial_failure);
#endif
    TT_RUN(test_args_and_unknown);
    TT_RUN(test_blank_lines);
    TT_RUN(test_stop);
    TT_RUN(test_latched_rejects_all_but_status);
    TT_RUN(test_nonlatched_fault_commands_still_work);
    TT_RUN(test_status_exact);
    TT_RUN(test_status_temp_err);
    TT_RUN(test_feed_line_assembly);
    TT_RUN(test_feed_too_long);
    TT_RUN(test_echo_default_on);
    TT_RUN(test_echo_crlf_and_lf);
    TT_RUN(test_edit_backspace_del);
    TT_RUN(test_edit_backspace_echo_off);
    TT_RUN(test_edit_backspace_overflow);
    TT_RUN(test_edit_backspace_at_63);
    TT_RUN(test_control_chars_dropped);
    TT_RUN(test_escape_sequences_exact);
    TT_RUN(test_escape_cr_aborts);
    TT_RUN(test_echo_overlong_not_echoed);
    TT_RUN(test_echo_command);
    TT_RUN(test_echo_cmd_exec_direct);
    TT_RUN(test_echo_latched);
    TT_RUN(test_init_resets_echo);
    return TT_RESULT();
}
