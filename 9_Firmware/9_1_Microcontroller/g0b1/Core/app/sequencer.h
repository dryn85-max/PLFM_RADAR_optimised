/* Power/RF sequencer: rail ordering as tables of {gpio, level, delay_ms} steps.
 *
 * Chip selects (active low, idle high while the chip is powered) follow the
 * chip supply: SEQ_BASE_UP raises them AFTER the supply is on, SEQ_DOWN/SEQ_ESTOP
 * drive them (and PLL CE) LOW only AFTER the supply is off. A high MCU output
 * into an unpowered ADAR1000 / PLL would back-power it through its input
 * protection (inputs must stay <= supply + 0.3 V); a low level never violates
 * the abs. max. ratings and selecting an unpowered device does nothing.
 * (Gap: hal_gpio_init() still idles CS high before the rails are up; BACKLOG.)
 *
 * Power-up is split in two because the ADTR1107 needs its negative gate bias
 * (VGG_PA, set over SPI through the ADAR1000) before VDD_PA is applied:
 *   SEQ_BASE_UP (FPGA, LO, ADAR, VDD_SW, VSS_SW) -> ADAR init + safe bias ->
 *   SEQ_RF_UP (LNA 3V3, then PA 5 V).
 * Both refuse to run while a fault is latched (-EPERM, nothing written).
 *
 * Emergency stop is GPIO only (no SPI/I2C, no delay, no log), so it works
 * with a hung bus and from fault handlers. Order: DIG3 mixers_enable low ->
 * PA 5 V -> ADTR LNA 3V3 -> ADTR VSS_SW -> ADTR VDD_SW -> ADAR supply ->
 * ADAR CS0..3 low -> LO enable -> PLL CE, CS low -> FPGA DIG0, DIG1, DIG2, DIG4
 * low -> FPGA enable (the DIG lines go low while the FPGA is still powered so
 * no MCU output back-powers it). SEQ_DOWN is the same order with delays. */
#ifndef SEQUENCER_H
#define SEQUENCER_H

#include <stddef.h>
#include <stdint.h>
#include "hal_gpio.h"

typedef struct { gpio_t pin; uint8_t level; uint16_t delay_ms; } seq_step_t;

#define SEQ_BASE_UP_N 10
#define SEQ_RF_UP_N   2
#define SEQ_DOWN_N    18
#define SEQ_ESTOP_N   18
extern const seq_step_t SEQ_BASE_UP[SEQ_BASE_UP_N];
extern const seq_step_t SEQ_RF_UP[SEQ_RF_UP_N];
extern const seq_step_t SEQ_DOWN[SEQ_DOWN_N];
extern const seq_step_t SEQ_ESTOP[SEQ_ESTOP_N];

/* Write each step's level, then delay_ms(step.delay_ms) if nonzero.
 * NULL steps or n == 0 do nothing. Does NOT check the fault latch. */
void sequencer_run(const seq_step_t *steps, size_t n);

int  sequencer_power_up(void);        /* SEQ_BASE_UP; latched fault -> -EPERM, nothing written */
int  sequencer_rf_up(void);           /* SEQ_RF_UP;   latched fault -> -EPERM, nothing written */
void sequencer_rf_off(void);          /* DIG3, PA, LNA low; no delays; GPIO only */
void sequencer_power_down(void);      /* SEQ_DOWN (orderly, with delays) */
void sequencer_emergency_stop(void);  /* SEQ_ESTOP, no delays, GPIO only */

#endif
