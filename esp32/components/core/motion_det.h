/* Motion detector on the LD2410C moving target (plain C11). Spec: R1 of
 * docs/superpowers/specs/2026-10-03-esp32-motion.md.
 * A frame is "moving" when the moving bit of target_state is set, the moving distance is in
 * [dmin_cm, dmax_cm] (both inclusive) and the moving energy is >= emin. An event starts when
 * moving frames persist for tstart_ms and ends after tend_ms without a moving frame. */
#ifndef MOTION_DET_H
#define MOTION_DET_H

#include <stdint.h>
#include "ld2410_frame.h"

typedef struct {
    uint8_t en;          /* 0/1 */
    uint16_t dmin_cm;    /* zone, 0..900, dmin < dmax */
    uint16_t dmax_cm;
    uint8_t emin;        /* 0..100 */
    uint32_t tstart_ms;  /* 100..10000 */
    uint32_t tend_ms;    /* 500..60000 */
} motion_cfg_t;

/* en 1, zone 100..500 cm, energy >= 30, start 1000 ms, end 3000 ms. */
void motion_cfg_defaults(motion_cfg_t *cfg);

typedef enum {
    MOTION_EV_NONE = 0,
    MOTION_EV_START = 1,
    MOTION_EV_END = 2
} motion_ev_kind_t;

typedef struct {
    uint32_t event_no;       /* 1 for the first started event since init */
    uint64_t onset_us;       /* time of the first moving frame of the event */
    uint32_t duration_ms;    /* END: last moving - onset; START: 0 */
    uint8_t max_energy;
    uint16_t dist_cm;        /* moving distance at the max energy (first one on a tie) */
    uint16_t min_dist_cm;    /* START: values seen during PENDING */
    uint16_t max_dist_cm;
} motion_ev_t;

typedef struct {
    uint8_t state;           /* internal: idle / pending / active */
    uint32_t event_no;
    uint64_t onset_us;
    uint64_t last_us;        /* time of the last moving frame */
    uint8_t max_energy;
    uint16_t dist_cm, min_dist_cm, max_dist_cm;
} motion_det_t;

void motion_det_init(motion_det_t *det);

/* 1 while an event is ACTIVE (started and not yet ended). */
int motion_det_active(const motion_det_t *det);

/* One call per decoded frame; d == NULL means no data (link lost) and counts as a
 * non-moving frame. Call it at least once per second so an END is not delayed.
 * The new cfg applies from this frame. Returns MOTION_EV_START / MOTION_EV_END and fills *ev
 * (only then), else MOTION_EV_NONE. NULL det/cfg/ev -> MOTION_EV_NONE, nothing changed.
 * The END check uses the previous moving time before this frame is counted: a moving frame
 * arriving tend_ms or more after the last one ends the old event and is itself dropped. */
motion_ev_kind_t motion_det_step(motion_det_t *det, const motion_cfg_t *cfg,
                                 const ld_data_t *d, uint64_t now_us, motion_ev_t *ev);

#endif
