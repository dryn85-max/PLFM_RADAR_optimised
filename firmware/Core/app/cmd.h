/* Line-oriented text command interface (USART2 / ST-LINK VCP, 115200 8N1).
 *
 *   beam <az> <el>   az -180..180 (mechanical, stored/reported), el -60..60 (phases)
 *   gain <ch> <val>  RX VGA of channel ch (0 .. ADAR_COUNT*4-1) = val (0..127)
 *   tx | rx          force the ADAR1000s to SPI TX / RX mode (bench)
 *   auto             back to TR-pin mode (FPGA owns TX/RX switching)
 *   status           one STATUS line
 *   stop             emergency stop, latched
 *
 * Replies: OK | OK stopped | ERR range | ERR args | ERR spi | ERR unknown |
 *          ERR too_long | ERR latched (everything except `status` while a fault
 *          is latched). Lines end with CR and/or LF, max 63 characters,
 *          case-sensitive, tokens split on spaces, integers are strict
 *          (optional sign, digits only; "12x" is ERR args).
 * No printf: the status line uses the integer formatter in strfmt.h. */
#ifndef CMD_H
#define CMD_H
#include <stddef.h>
#include "agc.h"

#define CMD_MAX_LINE 63

/* Binds the AGC state used by `gain` and `status`; mode auto, az = el = 0. */
void cmd_init(agc_t *agc);
/* Execute one complete line (no terminator). Writes the reply (no newline)
 * into out and returns its length; 0 and an empty reply for a blank line. */
size_t cmd_exec(const char *line, char *out, size_t outlen);
/* Feed one received character; on CR/LF a non-empty line is executed and the
 * reply is written to the UART followed by CRLF. */
void cmd_feed(int c);

#endif
