/* Integer beam tables for the ADAR1000 array (no float/double).
 * Elements are spaced lambda/2 along the steered axis (elevation), so the
 * per-element phase step is 180 deg * sin(el). Element n = dev*4 + ch. */
#ifndef BEAM_H
#define BEAM_H
#include <stdint.h>
#include "config.h"

/* Fills idx[0 .. ADAR_COUNT*4-1] with the 7-bit ADAR1000 phase indices
 * (LSB 360/128 deg) for steering angle el_deg (integer degrees, -90..90).
 * Returns 0, or -EINVAL outside the range (idx untouched).
 *   phase_q6(n) = (n * sin_q15(el)) >> 3      (1/64 LSB units, arithmetic shift)
 *   idx         = ((phase_q6 + 32) >> 6) & 127  (round to nearest, mod 128) */
int beam_phase_indices(int el_deg, uint8_t idx[ADAR_COUNT * 4]);

/* Computes the indices and writes RX and TX phases of every device/channel
 * (adar_set_rx_phase / adar_set_tx_phase). Returns 0 or the first error; stops
 * at the first failing write. -EINVAL (no SPI traffic) outside -90..90. */
int beam_apply(int el_deg);

#endif
