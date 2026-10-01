#include "agc.h"
#include <string.h>
#include "adar1000.h"
#include "fpga_if.h"
#include "hal_time.h"

#define AGC_CHANNELS (ADAR_COUNT * 4)

void agc_init(agc_t *a)
{
    int g;
    memset(a, 0, sizeof *a);
    a->base = kDefaultRxVgaGain;
    a->step_down = 4;
    a->step_up = 1;
    a->min_gain = 0;
    a->max_gain = 127;
    a->holdoff_frames = 4;
    for (g = 0; g < AGC_CHANNELS; g++) {
        a->written[g] = -1;
    }
    a->last_tick_ms = millis();
}

int agc_update(agc_t *a, int saturated)
{
    uint8_t before = a->base;

    if (!a->enabled) {
        return 0;
    }
    a->last_saturated = saturated != 0;
    if (saturated) {
        a->sat_events++;
        a->holdoff_counter = 0;
        if (a->base >= a->step_down + a->min_gain) {   /* int promotion, as upstream */
            a->base = (uint8_t)(a->base - a->step_down);
        } else {
            a->base = a->min_gain;
        }
    } else {
        a->holdoff_counter++;
        if (a->holdoff_counter >= a->holdoff_frames) {
            a->holdoff_counter = 0;
            if (a->base + a->step_up <= a->max_gain) {
                a->base = (uint8_t)(a->base + a->step_up);
            } else {
                a->base = a->max_gain;
            }
        }
    }
    return a->base != before;
}

uint8_t agc_effective(const agc_t *a, uint8_t g)
{
    int16_t raw;
    if (g >= AGC_CHANNELS) {
        return a->min_gain;   /* out-of-range channels get the minimum, as upstream */
    }
    raw = (int16_t)((int16_t)a->base + a->cal_offset[g]);
    if (raw < (int16_t)a->min_gain) {
        return a->min_gain;
    }
    if (raw > (int16_t)a->max_gain) {
        return a->max_gain;
    }
    return (uint8_t)raw;
}

int agc_apply(agc_t *a)
{
    int g, writes = 0, rc;
    for (g = 0; g < AGC_CHANNELS; g++) {
        uint8_t eff = agc_effective(a, (uint8_t)g);
        if (a->written[g] == (int16_t)eff) {
            continue;
        }
        rc = adar_set_rx_gain((uint8_t)(g / 4), (uint8_t)(g % 4), eff);
        if (rc != 0) {
            return rc;
        }
        a->written[g] = eff;
        writes++;
    }
    return writes;
}

void agc_invalidate(agc_t *a)
{
    int g;
    for (g = 0; g < AGC_CHANNELS; g++) {
        a->written[g] = -1;
    }
}

void agc_tick(agc_t *a)
{
    fpga_status_t s;
    if ((uint32_t)(millis() - a->last_tick_ms) < AGC_PERIOD_MS) {
        return;
    }
    a->last_tick_ms = millis();
    s = fpga_if_sample();
    a->enabled = (uint8_t)fpga_if_agc_enable_debounced(s.agc_enable);
    if (a->enabled) {
        (void)agc_update(a, s.saturation);
        (void)agc_apply(a);   /* on error the channel stays unknown and is retried next frame */
    }
}
