#include "ld_settings.h"
#include <errno.h>
#include <stdio.h>
#include <string.h>
#include "wifi_form.h"

int ld_settings_from_params(const ld_params_t *p, ld_settings_t *out)
{
    if (!p || !out)
        return -EINVAL;
    out->max_move_gate = p->max_move_gate;
    out->max_still_gate = p->max_still_gate;
    out->duration_s = p->duration_s;
    memcpy(out->move_sens, p->move_sens, LD_GATE_COUNT);
    memcpy(out->still_sens, p->still_sens, LD_GATE_COUNT);
    return 0;
}

int ld_settings_check(const ld_settings_t *s)
{
    if (!s)
        return -EINVAL;
    if (s->max_move_gate < LD_GATE_MIN_MAX || s->max_move_gate > LD_GATE_MAX ||
        s->max_still_gate < LD_GATE_MIN_MAX || s->max_still_gate > LD_GATE_MAX)
        return -EINVAL;
    for (unsigned g = 0; g < LD_GATE_COUNT; g++) {
        if (s->move_sens[g] > LD_SENS_MAX)
            return -EINVAL;
        if (g >= 2 && s->still_sens[g] > LD_SENS_MAX)
            return -EINVAL;
    }
    return 0;
}

#define FIELD_COUNT 19u
#define VALUE_MAX 5u

/* Field table index: 0 mg, 1 sg, 2 dur, 3..11 m0..m8, 12..18 s2..s8. */
static int field_index(const char *k, size_t n)
{
    if (n == 2 && k[0] == 'm' && k[1] == 'g') return 0;
    if (n == 2 && k[0] == 's' && k[1] == 'g') return 1;
    if (n == 3 && memcmp(k, "dur", 3) == 0) return 2;
    if (n == 2 && k[0] == 'm' && k[1] >= '0' && k[1] <= '8') return 3 + (k[1] - '0');
    if (n == 2 && k[0] == 's' && k[1] >= '2' && k[1] <= '8') return 12 + (k[1] - '2');
    return -1;
}

int ld_settings_parse_form(const char *body, size_t len, const ld_settings_t *current,
                           ld_settings_t *out)
{
    if (!body || !current || !out)
        return LDS_E_ARG;
    if (len > LDS_BODY_MAX)
        return LDS_E_TOO_LONG;

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
                if (seen[idx]) return LDS_E_DUPLICATE;
                const char *val = eq + 1;
                size_t vlen = (size_t)((body + end) - val);
                char dec[VALUE_MAX + 1];
                int r = wf_url_decode(val, vlen, dec, VALUE_MAX);
                if (r == WF_E_TOO_LONG) return LDS_E_TOO_LONG;
                if (r < 0 || r == 0) return LDS_E_BAD_VALUE;
                unsigned v = 0;
                for (int i = 0; i < r; i++) {
                    if (dec[i] < '0' || dec[i] > '9') return LDS_E_BAD_VALUE;
                    v = v * 10 + (unsigned)(dec[i] - '0'); /* <= 99999 */
                }
                vals[idx] = v;
                seen[idx] = 1;
            }
        }
        pos = end + 1;
    }
    for (unsigned i = 0; i < FIELD_COUNT; i++)
        if (!seen[i]) return LDS_E_MISSING;

    if (vals[0] < LD_GATE_MIN_MAX || vals[0] > LD_GATE_MAX ||
        vals[1] < LD_GATE_MIN_MAX || vals[1] > LD_GATE_MAX || vals[2] > 65535u)
        return LDS_E_RANGE;
    for (unsigned i = 3; i < FIELD_COUNT; i++)
        if (vals[i] > LD_SENS_MAX) return LDS_E_RANGE;

    ld_settings_t s;
    s.max_move_gate = (uint8_t)vals[0];
    s.max_still_gate = (uint8_t)vals[1];
    s.duration_s = (uint16_t)vals[2];
    for (unsigned g = 0; g < LD_GATE_COUNT; g++)
        s.move_sens[g] = (uint8_t)vals[3 + g];
    s.still_sens[0] = current->still_sens[0];
    s.still_sens[1] = current->still_sens[1];
    for (unsigned g = 2; g < LD_GATE_COUNT; g++)
        s.still_sens[g] = (uint8_t)vals[12 + g - 2];
    *out = s;
    return LDS_OK;
}

int ld_settings_diff(const ld_settings_t *old, const ld_settings_t *new_, ld_diff_t *out)
{
    if (!old || !new_ || !out)
        return -EINVAL;
    memset(out, 0, sizeof *out);
    out->need_gates = old->max_move_gate != new_->max_move_gate ||
                      old->max_still_gate != new_->max_still_gate ||
                      old->duration_s != new_->duration_s;
    for (unsigned g = 0; g < LD_GATE_COUNT; g++) {
        int still_changed = g >= 2 && old->still_sens[g] != new_->still_sens[g];
        if (old->move_sens[g] == new_->move_sens[g] && !still_changed)
            continue;
        out->sens[out->n_sens].gate = (uint8_t)g;
        out->sens[out->n_sens].move = new_->move_sens[g];
        out->sens[out->n_sens].still = g >= 2 ? new_->still_sens[g] : old->still_sens[g];
        out->n_sens++;
    }
    return 0;
}

int ld_version_format(const ld_version_t *v, char *buf, size_t cap)
{
    if (!v || !buf || cap == 0)
        return -EINVAL;
    char tmp[LD_VERSION_STR_MAX];
    int n = snprintf(tmp, sizeof tmp, "V%u.%02x.%08x", (unsigned)(v->major >> 8),
                     (unsigned)(v->major & 0xFF), (unsigned)v->minor);
    if (n < 0 || (size_t)n >= sizeof tmp || (size_t)n >= cap)
        return -ENOSPC;
    memcpy(buf, tmp, (size_t)n + 1);
    return n;
}
