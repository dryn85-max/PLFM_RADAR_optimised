#include "boot_btn.h"

void bb_init(bb_t *b, bool pressed_at_boot)
{
    b->held_ms = 0;
    b->held = false;
    b->ignore = pressed_at_boot;
}

bb_zone_t bb_zone(uint32_t held_ms)
{
    if (held_ms >= BB_CANCEL_MS) return BB_ZONE_CANCEL;
    if (held_ms >= BB_RESET_MS) return BB_ZONE_RESET;
    if (held_ms >= BB_AP_MS) return BB_ZONE_AP;
    return BB_ZONE_NONE;
}

bb_act_t bb_step(bb_t *b, bool pressed, uint32_t dt_ms)
{
    if (b->ignore) {
        if (!pressed) b->ignore = false;
        return BB_ACT_NONE;
    }
    if (pressed) {
        if (!b->held) {
            b->held = true;
            b->held_ms = 0;
        } else {
            b->held_ms = (dt_ms > UINT32_MAX - b->held_ms) ? UINT32_MAX : b->held_ms + dt_ms;
        }
        return BB_ACT_NONE;
    }
    if (!b->held) return BB_ACT_NONE;
    bb_zone_t z = bb_zone(b->held_ms);
    b->held = false;
    b->held_ms = 0;
    if (z == BB_ZONE_AP) return BB_ACT_AP;
    if (z == BB_ZONE_RESET) return BB_ACT_RESET;
    return BB_ACT_NONE;
}

bb_zone_t bb_held_zone(const bb_t *b)
{
    return b->held ? bb_zone(b->held_ms) : BB_ZONE_NONE;
}

bool bb_is_held(const bb_t *b)
{
    return b->held;
}
