/* BOOT button hold-zone tracker (plain C11, no hardware access).
 * Fed with the sampled button level and the time since the previous sample; the action
 * of the zone is reported once, on the release edge. Zones by hold time:
 *   NONE [0,2000) ms, AP [2000,5000), RESET [5000,10000), CANCEL >= 10000 (no action).
 * Hold time is measured between the first and the last pressed sample: the press-edge
 * step starts it at 0 and the release step adds nothing. Not thread-safe. */
#ifndef BOOT_BTN_H
#define BOOT_BTN_H

#include <stdbool.h>
#include <stdint.h>

#define BB_AP_MS     2000u
#define BB_RESET_MS  5000u
#define BB_CANCEL_MS 10000u

typedef enum { BB_ZONE_NONE = 0, BB_ZONE_AP = 1, BB_ZONE_RESET = 2, BB_ZONE_CANCEL = 3 } bb_zone_t;
typedef enum { BB_ACT_NONE = 0, BB_ACT_AP = 1, BB_ACT_RESET = 2 } bb_act_t;

typedef struct {
    uint32_t held_ms; /* saturates at UINT32_MAX */
    bool held;        /* a press is being tracked */
    bool ignore;      /* pressed at boot: ignore until the first release */
} bb_t;

/* pressed_at_boot: the button was already down when the application started
 * (GPIO0 low at reset is download mode); nothing fires until it is released once. */
void bootbtn_init(bb_t *b, bool pressed_at_boot);
/* Zone of a hold time. */
bb_zone_t bootbtn_zone(uint32_t held_ms);
/* One sample. Returns the action on the release edge (BB_ACT_NONE otherwise, and for the
 * NONE/CANCEL zones and the release of a held-at-boot press). */
bb_act_t bootbtn_step(bb_t *b, bool pressed, uint32_t dt_ms);
/* Current zone while a real press is held; BB_ZONE_NONE when not held or ignored. */
bb_zone_t bootbtn_held_zone(const bb_t *b);
/* True while a real (not ignored) press is held: the LED shows the zone colour. */
bool bootbtn_is_held(const bb_t *b);

#endif
