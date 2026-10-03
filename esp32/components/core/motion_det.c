#include "motion_det.h"
#include <errno.h>
#include <stddef.h>
#include <string.h>
#include "wifi_form.h"

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

#define DMAX_CM 900u

static int in_range(const motion_cfg_t *c)
{
    return c->en <= 1u && c->dmin_cm <= DMAX_CM && c->dmax_cm <= DMAX_CM && c->emin <= 100u &&
           c->tstart_ms >= 100u && c->tstart_ms <= 10000u && c->tend_ms >= 500u &&
           c->tend_ms <= 60000u;
}

int motion_cfg_check(const motion_cfg_t *cfg)
{
    if (!cfg || !in_range(cfg) || cfg->dmin_cm >= cfg->dmax_cm)
        return -EINVAL;
    return 0;
}

#define FIELD_COUNT 6u
#define VALUE_MAX 5u

/* Field table index: 0 en, 1 dmin, 2 dmax, 3 emin, 4 tstart, 5 tend. */
static int field_index(const char *k, size_t n)
{
    static const char *const names[FIELD_COUNT] = {"en", "dmin", "dmax", "emin", "tstart", "tend"};
    for (unsigned i = 0; i < FIELD_COUNT; i++)
        if (n == strlen(names[i]) && memcmp(k, names[i], n) == 0)
            return (int)i;
    return -1;
}

int motion_cfg_parse_form(const char *body, size_t len, motion_cfg_t *out)
{
    if (!body || !out)
        return MCF_E_ARG;
    if (len > MCF_BODY_MAX)
        return MCF_E_TOO_LONG;

    unsigned vals[FIELD_COUNT];
    uint8_t seen[FIELD_COUNT] = {0};
    size_t pos = 0;
    while (pos < len) {
        size_t end = pos;
        while (end < len && body[end] != '&') end++;
        const char *eq = memchr(body + pos, '=', end - pos);
        if (eq != NULL) {
            int idx = field_index(body + pos, (size_t)(eq - (body + pos)));
            if (idx >= 0) {
                if (seen[idx]) return MCF_E_DUPLICATE;
                const char *val = eq + 1;
                size_t vlen = (size_t)((body + end) - val);
                char dec[VALUE_MAX + 1];
                int r = wf_url_decode(val, vlen, dec, VALUE_MAX);
                if (r == WF_E_TOO_LONG) return MCF_E_TOO_LONG;
                if (r <= 0) return MCF_E_BAD_VALUE;
                unsigned v = 0;
                for (int i = 0; i < r; i++) {
                    if (dec[i] < '0' || dec[i] > '9') return MCF_E_BAD_VALUE;
                    v = v * 10 + (unsigned)(dec[i] - '0'); /* <= 99999 */
                }
                vals[idx] = v;
                seen[idx] = 1;
            }
        }
        pos = end + 1;
    }
    for (unsigned i = 0; i < FIELD_COUNT; i++)
        if (!seen[i]) return MCF_E_MISSING;

    if (vals[0] > 1u || vals[1] > DMAX_CM || vals[2] > DMAX_CM || vals[3] > 100u ||
        vals[4] < 100u || vals[4] > 10000u || vals[5] < 500u || vals[5] > 60000u)
        return MCF_E_RANGE;
    if (vals[1] >= vals[2])
        return MCF_E_ORDER;

    out->en = (uint8_t)vals[0];
    out->dmin_cm = (uint16_t)vals[1];
    out->dmax_cm = (uint16_t)vals[2];
    out->emin = (uint8_t)vals[3];
    out->tstart_ms = vals[4];
    out->tend_ms = vals[5];
    return MCF_OK;
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
