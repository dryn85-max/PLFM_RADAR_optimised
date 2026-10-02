/* FPGA DIG0..7 contract (roles per radar_system_top.v):
 *   DIG0 new_chirp, DIG1 new_elevation, DIG2 new_azimuth  (MCU -> FPGA, toggle)
 *   DIG3 mixers_enable, DIG4 fpga_reset_n                 (MCU -> FPGA, level)
 *   DIG5 agc saturation, DIG6 agc enable (host_agc_enable), DIG7 reserved
 *                                                         (FPGA -> MCU)
 * No logging in this module (the toggles are per-chirp paths). */
#ifndef FPGA_IF_H
#define FPGA_IF_H

#include <stdint.h>

void fpga_if_init(void);                 /* DIG0..4 low; AGC-enable debounce back to boot default (off) */
void fpga_if_reset_pulse(void);          /* DIG4 low, 10 ms, high (upstream) */
void fpga_if_set_mixers(int on);         /* DIG3 */
void fpga_if_toggle_chirp(void);         /* DIG0 */
void fpga_if_toggle_elevation(void);     /* DIG1 */
void fpga_if_toggle_azimuth(void);       /* DIG2 */

typedef struct { uint8_t saturation, agc_enable; } fpga_status_t;
fpga_status_t fpga_if_sample(void);      /* ONE gpio_read_fpga_port() -> bits 5, 6 */

/* Upstream 2-frame rule (main.cpp): the enable state changes only when two
 * consecutive samples agree (prev starts at 0 = boot default, AGC off).
 * Any nonzero dig6_now counts as 1. Returns the debounced enable state. */
int fpga_if_agc_enable_debounced(uint8_t dig6_now);

#endif
