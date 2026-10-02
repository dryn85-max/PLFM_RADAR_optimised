#include "snapshot_json.h"
#include <errno.h>
#include <stdarg.h>
#include <stdio.h>

typedef struct {
    char *out;
    size_t cap;
    size_t pos;
    int err;
} w_t;

static void put(w_t *w, const char *fmt, ...) __attribute__((format(printf, 2, 3)));
static void put(w_t *w, const char *fmt, ...)
{
    if (w->err)
        return;
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(w->out + w->pos, w->cap - w->pos, fmt, ap);
    va_end(ap);
    if (n < 0 || (size_t)n >= w->cap - w->pos)
        w->err = -ENOSPC;
    else
        w->pos += (size_t)n;
}

static void put_arr(w_t *w, const char *key, const uint8_t *v)
{
    put(w, "\"%s\":[", key);
    for (unsigned i = 0; i < LD_GATES; i++)
        put(w, "%s%u", i ? "," : "", (unsigned)v[i]);
    put(w, "]");
}

int snapshot_json(char *out, size_t cap, const snapshot_t *s)
{
    if (!out || !s)
        return -EINVAL;
    if (cap == 0)
        return -ENOSPC;
    out[0] = 0;
    const char *link;
    switch (s->link) {
    case SNAP_LINK_NO_DATA: link = "no_data"; break;
    case SNAP_LINK_OK: link = "ok"; break;
    case SNAP_LINK_LOST: link = "lost"; break;
    default: return -EINVAL;
    }
    w_t w = {out, cap, 0, 0};
    put(&w, "{\"seq\":%lu,\"esp_time_us\":%llu,\"link\":\"%s\",\"data\":",
        (unsigned long)s->seq, (unsigned long long)s->esp_time_us, link);
    if (!s->have_frame) {
        put(&w, "null");
    } else {
        const ld_data_t *d = &s->data;
        put(&w, "{\"engineering\":%u,\"target_state\":%u,\"moving_dist_cm\":%u,"
                "\"moving_energy\":%u,\"still_dist_cm\":%u,\"still_energy\":%u,"
                "\"detect_dist_cm\":%u",
            d->engineering ? 1u : 0u, (unsigned)d->target_state,
            (unsigned)d->moving_dist_cm, (unsigned)d->moving_energy,
            (unsigned)d->still_dist_cm, (unsigned)d->still_energy,
            (unsigned)d->detect_dist_cm);
        if (d->engineering) {
            put(&w, ",\"max_moving_gate\":%u,\"max_still_gate\":%u,",
                (unsigned)d->max_moving_gate, (unsigned)d->max_still_gate);
            put_arr(&w, "moving_gate_energy", d->moving_gate_energy);
            put(&w, ",");
            put_arr(&w, "still_gate_energy", d->still_gate_energy);
        }
        put(&w, "}");
    }
    put(&w, "}");
    if (w.err) {
        out[0] = 0;
        return w.err;
    }
    return (int)w.pos;
}
