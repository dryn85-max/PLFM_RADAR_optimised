#include "status_led.h"

sl_rgb_t sl_color(const sl_in_t *in, uint32_t t_ms)
{
    if (in->held) {
        if (in->zone == BB_ZONE_AP) return SL_BLUE;
        if (in->zone == BB_ZONE_RESET) return SL_RED;
        return SL_OFF; /* NONE / CANCEL zones: off, still overrides lower priorities */
    }
    if (in->flash_active) {
        uint32_t e = t_ms - in->flash_start_ms;
        if (e < SL_FLASH_TOTAL_MS) {
            return (e % SL_FLASH_PERIOD_MS) < SL_FLASH_ON_MS ? in->flash_color : SL_OFF;
        }
    }
    if (in->ap == SL_AP_ONLY) return SL_BLUE;
    if (in->ap == SL_AP_ON_DEMAND) {
        return ((t_ms - in->ap_ref_ms) % SL_BLINK_PERIOD_MS) < SL_BLINK_ON_MS ? SL_BLUE : SL_OFF;
    }
    return SL_OFF;
}
