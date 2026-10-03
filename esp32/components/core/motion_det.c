#include "motion_det.h"
#include <stddef.h>

enum { ST_IDLE = 0, ST_PENDING = 1, ST_ACTIVE = 2 };

void motion_cfg_defaults(motion_cfg_t *cfg)
{
    if (!cfg)
        return;
    cfg->en = 1;
    cfg->dmin_cm = 100;
    cfg->dmax_cm = 500;
    cfg->emin = 30;
    cfg->tstart_ms = 1000;
    cfg->tend_ms = 3000;
}

void motion_det_init(motion_det_t *det)
{
    if (!det)
        return;
    *det = (motion_det_t){0};
}

int motion_det_active(const motion_det_t *det)
{
    return det && det->state == ST_ACTIVE;
}

static uint64_t elapsed_us(uint64_t now, uint64_t then)
{
    return now > then ? now - then : 0; /* now < then must not happen: 0 elapsed */
}

static void fill(const motion_det_t *det, uint32_t duration_ms, motion_ev_t *ev)
{
    ev->event_no = det->event_no;
    ev->onset_us = det->onset_us;
    ev->duration_ms = duration_ms;
    ev->max_energy = det->max_energy;
    ev->dist_cm = det->dist_cm;
    ev->min_dist_cm = det->min_dist_cm;
    ev->max_dist_cm = det->max_dist_cm;
}

static motion_ev_kind_t end_event(motion_det_t *det, motion_ev_t *ev)
{
    uint64_t ms = elapsed_us(det->last_us, det->onset_us) / 1000u;
    fill(det, ms > UINT32_MAX ? UINT32_MAX : (uint32_t)ms, ev);
    det->state = ST_IDLE;
    return MOTION_EV_END;
}

motion_ev_kind_t motion_det_step(motion_det_t *det, const motion_cfg_t *cfg,
                                 const ld_data_t *d, uint64_t now_us, motion_ev_t *ev)
{
    if (!det || !cfg || !ev)
        return MOTION_EV_NONE;

    if (!cfg->en) {
        motion_ev_kind_t r = det->state == ST_ACTIVE ? end_event(det, ev) : MOTION_EV_NONE;
        det->state = ST_IDLE;
        return r;
    }

    int moving = d != NULL && (d->target_state & 1u) &&
                 d->moving_dist_cm >= cfg->dmin_cm && d->moving_dist_cm <= cfg->dmax_cm &&
                 d->moving_energy >= cfg->emin;

    if (det->state == ST_ACTIVE &&
        elapsed_us(now_us, det->last_us) >= (uint64_t)cfg->tend_ms * 1000u)
        return end_event(det, ev);

    if (!moving) {
        if (det->state == ST_PENDING)
            det->state = ST_IDLE;
        return MOTION_EV_NONE;
    }

    if (det->state == ST_IDLE) {
        det->state = ST_PENDING;
        det->onset_us = now_us;
        det->max_energy = d->moving_energy;
        det->dist_cm = d->moving_dist_cm;
        det->min_dist_cm = det->max_dist_cm = d->moving_dist_cm;
    } else {
        if (d->moving_energy > det->max_energy) {
            det->max_energy = d->moving_energy;
            det->dist_cm = d->moving_dist_cm;
        }
        if (d->moving_dist_cm < det->min_dist_cm)
            det->min_dist_cm = d->moving_dist_cm;
        if (d->moving_dist_cm > det->max_dist_cm)
            det->max_dist_cm = d->moving_dist_cm;
    }
    det->last_us = now_us;

    if (det->state == ST_PENDING &&
        elapsed_us(now_us, det->onset_us) >= (uint64_t)cfg->tstart_ms * 1000u) {
        det->state = ST_ACTIVE;
        det->event_no++;
        fill(det, 0, ev);
        return MOTION_EV_START;
    }
    return MOTION_EV_NONE;
}
