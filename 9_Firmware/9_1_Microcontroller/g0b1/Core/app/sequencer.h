/* Power/RF sequencer interface. Prototypes only until Task 9 adds sequencer.c;
 * fault.c depends on these two entry points. */
#ifndef SEQUENCER_H
#define SEQUENCER_H

/* Full emergency stop, GPIO only (no SPI/I2C): DIG3 mixers_enable low -> PA 5 V
 * -> ADTR LNA 3V3 -> ADTR VSS_SW -> ADTR VDD_SW -> ADAR supply -> LO enable ->
 * FPGA enable. Must be safe to call from a fault handler, at any time. */
void sequencer_emergency_stop(void);

/* RF rails off (PA, LNA), base rails stay. GPIO only. */
void sequencer_rf_off(void);

#endif
