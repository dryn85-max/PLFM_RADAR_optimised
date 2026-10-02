/* Outer-loop AGC for the ADAR1000 RX VGA gain: C port of upstream
 * ADAR1000_AGC (same attack / decay / holdoff / clamp semantics).
 *
 * Per frame (AGC_PERIOD_MS): if the FPGA saturation flag (DIG5) is set the
 * common base gain drops by step_down at once (floor min_gain); after
 * holdoff_frames clear frames it rises by step_up (ceiling max_gain).
 * Per-channel gain = clamp(base + cal_offset[g], min_gain, max_gain),
 * g = dev*4 + ch (0-based, 0 .. ADAR_COUNT*4-1).
 *
 * Unlike upstream (which rewrote all 16 channels every frame) only channels
 * whose effective gain differs from the last value written are sent.
 * No printf/DIAG in this module: it runs once per frame next to the radar. */
#ifndef AGC_H
#define AGC_H

#include <stdint.h>
#include "config.h"

typedef struct {
    uint8_t base, step_down, step_up, min_gain, max_gain, holdoff_frames, enabled;
    uint8_t holdoff_counter, last_saturated;
    uint32_t sat_events;
    int8_t  cal_offset[ADAR_COUNT * 4];
    int16_t written[ADAR_COUNT * 4];     /* last effective gain written, -1 = unknown */
    uint32_t last_tick_ms;               /* agc_tick() frame timer */
} agc_t;

/* base 30, step_down 4, step_up 1, min 0, max 127, holdoff 4, disabled,
 * cal_offset all 0, written all unknown, frame timer started now. */
void    agc_init(agc_t *a);
/* One frame. No-op returning 0 when disabled. Returns 1 if base changed. */
int     agc_update(agc_t *a, int saturated);
/* Clamped gain of channel g; g >= ADAR_COUNT*4 returns min_gain. */
uint8_t agc_effective(const agc_t *a, uint8_t g);
/* Write channels whose effective gain != written (adar_set_rx_gain, 0-based).
 * Returns the number of channels written, or the first negative errno (the
 * failed channel stays "unknown" and is retried next call). */
int     agc_apply(agc_t *a);
void    agc_invalidate(agc_t *a);        /* written[] = -1 (gain command, ADAR init) */
/* Every AGC_PERIOD_MS: one FPGA port sample, DIG6 2-frame debounce -> enabled,
 * then (if enabled) update with DIG5 and apply. */
void    agc_tick(agc_t *a);

#endif
