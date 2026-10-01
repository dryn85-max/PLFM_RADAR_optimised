#include "cmd.h"
#include <string.h>
#include "adar1000.h"
#include "beam.h"
#include "config.h"
#include "fault.h"
#include "fpga_if.h"
#include "hal_gpio.h"
#include "hal_uart.h"
#include "pll_lo.h"
#include "strfmt.h"
#include "thermal.h"

#define NCH       (ADAR_COUNT * 4)
#define MAX_TOK   4
#define BIG       1000000000   /* saturation value for oversized numbers (fails every range check) */

static agc_t *s_agc;
static int    s_az, s_el;

static char   s_line[CMD_MAX_LINE + 1];
static size_t s_len;
static int    s_overflow;

void cmd_init(agc_t *agc)
{
    s_agc = agc;
    s_az = 0;
    s_el = 0;
    s_len = 0;
    s_overflow = 0;
}

/* Strict integer: [+-]digits, nothing else. Oversized values saturate at BIG. */
static int parse_int(const char *s, int *out)
{
    int neg = 0;
    long v = 0;
    if (*s == '-' || *s == '+') {
        neg = (*s == '-');
        s++;
    }
    if (*s == '\0') {
        return -1;
    }
    for (; *s != '\0'; s++) {
        if (*s < '0' || *s > '9') {
            return -1;
        }
        if (v < BIG) {
            v = v * 10 + (*s - '0');
            if (v > BIG) {
                v = BIG;
            }
        }
    }
    *out = (int)(neg ? -v : v);
    return 0;
}

static size_t reply(char *out, size_t outlen, const char *msg)
{
    sbuf_t sb;
    sb_init(&sb, out, outlen);
    sb_puts(&sb, msg);
    return sb.len;
}

static int set_mode_all(adar_mode_t m)
{
    uint8_t d;
    for (d = 0; d < ADAR_COUNT; d++) {
        int rc = adar_set_mode(d, m);
        if (rc != 0) {
            return rc;
        }
    }
    return 0;
}

/* Mode of the ADAR devices as actually applied: none / auto / tx / rx, or
 * "mixed" when a failed all-device switch left them different. */
static const char *mode_name(void)
{
    static const char *const NAMES[] = { "auto", "tx", "rx", "none" };
    adar_mode_t m = adar_get_mode(0);
    uint8_t d;
    for (d = 1; d < ADAR_COUNT; d++) {
        if (adar_get_mode(d) != m) {
            return "mixed";
        }
    }
    return NAMES[m];
}

static size_t do_status(char *out, size_t outlen)
{
    sbuf_t sb;
    int g;
    sb_init(&sb, out, outlen);
    sb_puts(&sb, "STATUS lock=");        sb_put_int(&sb, pll_is_locked() ? 1 : 0);
    sb_puts(&sb, " temp=");              sb_put_int(&sb, thermal_last());
    sb_puts(&sb, " temp_err=");          sb_put_int(&sb, thermal_last_err() != 0 ? 1 : 0);
    sb_puts(&sb, " fault=");             sb_put_int(&sb, (int)fault_active());
    sb_puts(&sb, " latched=");           sb_put_int(&sb, fault_is_latched() ? 1 : 0);
    sb_puts(&sb, " mode=");              sb_puts(&sb, mode_name());
    sb_puts(&sb, " az=");                sb_put_int(&sb, s_az);
    sb_puts(&sb, " el=");                sb_put_int(&sb, s_el);
    sb_puts(&sb, " agc=");               sb_put_int(&sb, s_agc->enabled ? 1 : 0);
    sb_puts(&sb, " base=");              sb_put_uint(&sb, s_agc->base);
    sb_puts(&sb, " gains=");             /* last value written per channel, -1 = not written yet */
    for (g = 0; g < NCH; g++) {
        if (g > 0) {
            sb_putc(&sb, ',');
        }
        sb_put_int(&sb, s_agc->written[g]);
    }
    return sb.len;
}

static size_t do_beam(char **t, int n, char *out, size_t outlen)
{
    int az, el, rc;
    if (n != 3 || parse_int(t[1], &az) != 0 || parse_int(t[2], &el) != 0) {
        return reply(out, outlen, "ERR args");
    }
    if (az < -180 || az > 180 || el < -60 || el > 60) {
        return reply(out, outlen, "ERR range");
    }
    rc = beam_apply(el);
    if (rc != 0) {
        /* beam_apply() stops at the first failing write, so some elements may
         * already carry the new phases. State (az/el) and the FPGA strobes
         * (DIG1/DIG2) are left untouched; best-effort re-apply of the previous
         * elevation to bring the array back (result ignored: the bus is
         * probably faulty). */
        (void)beam_apply(s_el);
        return reply(out, outlen, "ERR spi");
    }
    if (el != s_el) {
        fpga_if_toggle_elevation();
    }
    if (az != s_az) {
        fpga_if_toggle_azimuth();
    }
    s_el = el;
    s_az = az;
    return reply(out, outlen, "OK");
}

static size_t do_gain(char **t, int n, char *out, size_t outlen)
{
    int ch, val;
    if (n != 3 || parse_int(t[1], &ch) != 0 || parse_int(t[2], &val) != 0) {
        return reply(out, outlen, "ERR args");
    }
    if (ch < 0 || ch >= NCH || val < 0 || val > 127) {
        return reply(out, outlen, "ERR range");
    }
    if (adar_set_rx_gain((uint8_t)(ch / 4), (uint8_t)(ch % 4), (uint8_t)val) != 0) {
        s_agc->written[ch] = -1;   /* hardware state unknown after a failed write */
        return reply(out, outlen, "ERR spi");
    }
    s_agc->written[ch] = (int16_t)val;   /* the AGC rewrites only when its effective value differs */
    return reply(out, outlen, "OK");
}

static size_t do_mode(adar_mode_t m, char *out, size_t outlen)
{
    if (set_mode_all(m) != 0) {
        return reply(out, outlen, "ERR spi");   /* STATUS shows what each device really has */
    }
    return reply(out, outlen, "OK");
}

size_t cmd_exec(const char *line, char *out, size_t outlen)
{
    char buf[CMD_MAX_LINE + 1];
    char *t[MAX_TOK];
    int n = 0, extra = 0;
    char *p;
    const char *cmd;

    if (outlen > 0) {
        out[0] = '\0';
    }
    if (strlen(line) > CMD_MAX_LINE) {
        return reply(out, outlen, "ERR too_long");
    }
    strcpy(buf, line);
    for (p = buf; *p != '\0';) {
        while (*p == ' ') {
            *p++ = '\0';
        }
        if (*p == '\0') {
            break;
        }
        if (n < MAX_TOK) {
            t[n++] = p;
        } else {
            extra = 1;
        }
        while (*p != '\0' && *p != ' ') {
            p++;
        }
    }
    if (n == 0) {
        return 0;
    }
    cmd = t[0];
    if (extra) {
        n = MAX_TOK + 1;   /* any command with more than 3 args is a bad-args case */
    }

    if (strcmp(cmd, "status") == 0) {
        return n == 1 ? do_status(out, outlen) : reply(out, outlen, "ERR args");
    }
    if (fault_is_latched()) {
        return reply(out, outlen, "ERR latched");
    }
    if (strcmp(cmd, "beam") == 0) {
        return do_beam(t, n, out, outlen);
    }
    if (strcmp(cmd, "gain") == 0) {
        return do_gain(t, n, out, outlen);
    }
    if (strcmp(cmd, "tx") == 0 || strcmp(cmd, "rx") == 0 || strcmp(cmd, "auto") == 0 ||
        strcmp(cmd, "stop") == 0) {
        if (n != 1) {
            return reply(out, outlen, "ERR args");
        }
        if (strcmp(cmd, "tx") == 0) {
            return do_mode(ADAR_MODE_SPI_TX, out, outlen);
        }
        if (strcmp(cmd, "rx") == 0) {
            return do_mode(ADAR_MODE_SPI_RX, out, outlen);
        }
        if (strcmp(cmd, "auto") == 0) {
            return do_mode(ADAR_MODE_TR_PIN, out, outlen);
        }
        fault_raise(FAULT_ESTOP_CMD);
        return reply(out, outlen, "OK stopped");
    }
    return reply(out, outlen, "ERR unknown");
}

static void send_reply(const char *msg, size_t n)
{
    if (n > 0) {
        (void)uart_write(msg, n);
        (void)uart_write("\r\n", 2);
    }
}

void cmd_feed(int c)
{
    char out[200];
    size_t n;

    if (c == '\r' || c == '\n') {
        if (s_overflow) {
            s_overflow = 0;
            s_len = 0;
            n = reply(out, sizeof out, "ERR too_long");
            send_reply(out, n);
            return;
        }
        s_line[s_len] = '\0';
        s_len = 0;
        n = cmd_exec(s_line, out, sizeof out);
        send_reply(out, n);
        return;
    }
    if (s_overflow) {
        return;   /* discard the rest of an over-long line */
    }
    if (s_len >= CMD_MAX_LINE) {
        s_overflow = 1;
        s_len = 0;
        return;
    }
    s_line[s_len++] = (char)c;
}
