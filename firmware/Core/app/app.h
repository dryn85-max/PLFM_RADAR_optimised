/* Application: boot sequence and superloop body (host-testable; main.c only
 * wires the target peripherals and the IWDG around it). */
#ifndef APP_H
#define APP_H
#include "agc.h"

/* Boot sequence:
 *  1. fault_init(); a latched fault from a previous run -> GPIO e-stop, print
 *     "BOOT latched fault=<n>", return (RF never powered, commands answer ERR latched).
 *  2. FPGA DIG0..4 low, SEQ_BASE_UP, PLL table + lock (fail: non-latched FAULT_PLL_LOCK,
 *     return, RF rails never enabled).
 *  3. ADAR1000 init (ascending) + safe bias (fail: FAULT_ADAR_COMM, return).
 *  4. SEQ_RF_UP, operational bias, TR-pin mode on every device, beam 0, AGC.
 *  5. FPGA reset pulse, mixers on, one discarded ADS7830 warm-up read (the internal
 *     reference only turns on with the first PD1=1 command, ads7830.pdf p. 17),
 *     then all periodic timers start at millis().
 * The IWDG is refreshed between the long stages. */
void app_init(void);

/* One non-blocking superloop pass: drain the command parser; unless a fault is
 * active run agc_tick, thermal_tick and the 100 ms PLL lock check; refresh the IWDG
 * (always, so a latched or faulted board keeps answering commands). */
void app_loop(void);

agc_t *app_agc(void);   /* AGC state shared with the command parser */

#endif
