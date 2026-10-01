/* Thermal monitoring: ADS7830 @0x48 channel 0 with a TMP37 (20 mV/degC, 0 mV at
 * 0 degC; tmp35_36_37.pdf) and the ADS7830 internal 2.5 V reference. */
#ifndef THERMAL_H
#define THERMAL_H
#include <stdint.h>

/* mv = raw*2500/255 (max 637500, fits int32); deci_c = mv*10/20 rounded to
 * nearest (= (mv+1)/2). *deci_c is untouched on error. Returns 0 or negative
 * errno (-EINVAL for a NULL pointer). */
int thermal_read_deci_c(int16_t *deci_c);

/* Starts the THERMAL_PERIOD_MS timer: the first read is one period after init. */
void thermal_init(void);
/* Call from the superloop. Every THERMAL_PERIOD_MS: read; a reading
 * >= OVERTEMP_DECI_C raises FAULT_OVERTEMP (latched). Read errors are only
 * recorded (thermal_last_err), not faulted (no PA on the prototype). */
void thermal_tick(void);

int16_t thermal_last(void);       /* last good reading, 0.1 degC (0 before the first) */
int     thermal_last_err(void);   /* 0, or the negative errno of the latest read */

#endif
