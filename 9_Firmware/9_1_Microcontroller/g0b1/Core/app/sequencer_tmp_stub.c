/* TEMPORARY (Task 4): weak no-op definitions so the target links before
 * sequencer.c exists. Task 9 must DELETE this file and provide the real,
 * strong sequencer_emergency_stop() / sequencer_rf_off() in sequencer.c.
 * On the host the test spies override these (weak). */
#include "sequencer.h"

__attribute__((weak)) void sequencer_emergency_stop(void) { }
__attribute__((weak)) void sequencer_rf_off(void) { }
