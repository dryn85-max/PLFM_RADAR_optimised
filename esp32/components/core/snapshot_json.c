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

/* integer / 10^dec with sign, dec in 1..7; v is the value scaled by 10^dec */
static void put_fixed(w_t *w, int64_t v, unsigned dec)
{
    uint64_t a = v < 0 ? (uint64_t)0 - (uint64_t)v : (uint64_t)v;
    uint64_t p = 1;
    for (unsigned i = 0; i < dec; i++)
        p *= 10;
    put(w, "%s%llu.%0*llu", v < 0 ? "-" : "", (unsigned long long)(a / p), (int)dec,
        (unsigned long long)(a % p));
}

static void put_fixed_or_null(w_t *w, const char *key, int ok, int64_t v, unsigned dec)
{
    put(w, ",\"%s\":", key);
    if (ok)
        put_fixed(w, v, dec);
    else
        put(w, "null");
}

static void put_uint_or_null(w_t *w, const char *key, int ok, unsigned v)
{
    put(w, ",\"%s\":", key);
    if (ok)
        put(w, "%u", v);
    else
        put(w, "null");
}

/* days since 1970-01-01 -> civil date (Howard Hinnant's algorithm) */
static void civil_from_days(int64_t z, int *y, unsigned *m, unsigned *d)
{
    z += 719468;
    int64_t era = (z >= 0 ? z : z - 146096) / 146097;
    unsigned doe = (unsigned)(z - era * 146097);
    unsigned yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
    int64_t yy = (int64_t)yoe + era * 400;
    unsigned doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
    unsigned mp = (5 * doy + 2) / 153;
    *d = doy - (153 * mp + 2) / 5 + 1;
    *m = mp < 10 ? mp + 3 : mp - 9;
    *y = (int)(yy + (*m <= 2));
}

#define UTC_MS_MAX 253402300799999LL /* 9999-12-31T23:59:59.999Z */

static void put_gps(w_t *w, const snapshot_t *s)
{
    const char *st = s->gps_status == SNAP_GPS_OK ? "ok" : s->gps_status == SNAP_GPS_SILENT ? "silent" : "absent";
    int live = s->gps_status == SNAP_GPS_OK && s->have_gps;
    const rec_gps_fix_t *g = &s->gps;
    int pos = live && (g->flags & GPS_FLAG_POS_VALID);
    int alt = live && (g->flags & GPS_FLAG_ALT_VALID);
    int tm = live && (g->flags & GPS_FLAG_TIME_VALID) && (g->flags & GPS_FLAG_DATE_VALID) &&
             g->utc_unix_ms >= 0 && g->utc_unix_ms <= UTC_MS_MAX;
    /* pos_valid already means GGA quality > 0 or RMC status A */
    put(w, ",\"gps\":{\"status\":\"%s\",\"fix\":%s", st, pos ? "true" : "false");
    put_uint_or_null(w, "fix_quality", live, g->fix_quality);
    put_uint_or_null(w, "sats", live, g->sats);
    put_fixed_or_null(w, "hdop", live && g->hdop_x100 != 0, g->hdop_x100, 2);
    put_fixed_or_null(w, "lat", pos, g->lat_e7, 7);
    put_fixed_or_null(w, "lon", pos, g->lon_e7, 7);
    put_fixed_or_null(w, "alt_m", alt, g->alt_cm, 2);
    /* cm/s -> km/h: x 0.036, rounded to 0.01 km/h */
    put_fixed_or_null(w, "speed_kmh", pos, ((int64_t)g->speed_cmps * 36 + 5) / 10, 2);
    put_fixed_or_null(w, "course_deg", pos, g->course_cdeg, 2);
    put(w, ",\"utc\":");
    if (tm) {
        int64_t ms = g->utc_unix_ms, days = ms / 86400000, rem = ms % 86400000;
        int y; unsigned mo, d;
        civil_from_days(days, &y, &mo, &d);
        put(w, "\"%04d-%02u-%02uT%02u:%02u:%02u.%03uZ\"", y, mo, d, (unsigned)(rem / 3600000),
            (unsigned)(rem / 60000 % 60), (unsigned)(rem / 1000 % 60), (unsigned)(rem % 1000));
    } else {
        put(w, "null");
    }
    put(w, ",\"time_valid\":%s,\"pos_valid\":%s}", tm ? "true" : "false", pos ? "true" : "false");
}

static void put_imu(w_t *w, const snapshot_t *s)
{
    const char *st = s->imu_status == SNAP_IMU_OK ? "ok" : s->imu_status == SNAP_IMU_ERROR ? "error" : "absent";
    int ok = s->imu_status == SNAP_IMU_OK && s->have_imu && (s->imu.status & IMU_STATUS_VALID);
    put(w, ",\"imu\":{\"status\":\"%s\"", st);
    put_fixed_or_null(w, "pitch_deg", ok, s->imu.pitch_cdeg, 2);
    put_fixed_or_null(w, "roll_deg", ok, s->imu.roll_cdeg, 2);
    put(w, ",\"valid\":%s}", ok ? "true" : "false");
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
    if ((unsigned)s->gps_status > SNAP_GPS_OK || (unsigned)s->imu_status > SNAP_IMU_OK)
        return -EINVAL;
    const char *tsrc;
    switch (s->time_source) {
    case TSRC_NONE: tsrc = "none"; break;
    case TSRC_GPS: tsrc = "gps"; break;
    case TSRC_SNTP: tsrc = "sntp"; break;
    default: return -EINVAL;
    }
    w_t w = {out, cap, 0, 0};
    put(&w, "{\"seq\":%lu,\"frame_no\":%lu,\"esp_time_us\":%llu,\"link\":\"%s\",\"data\":",
        (unsigned long)s->seq, (unsigned long)s->frame_no, (unsigned long long)s->esp_time_us, link);
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
    put_gps(&w, s);
    put_imu(&w, s);
    put(&w, ",\"time_source\":\"%s\"}", tsrc);
    if (w.err) {
        out[0] = 0;
        return w.err;
    }
    return (int)w.pos;
}
