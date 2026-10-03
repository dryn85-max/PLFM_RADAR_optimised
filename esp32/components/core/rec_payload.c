#include "rec_payload.h"
#include <errno.h>
#include "ringbuf.h"

static void put_le(uint8_t *p, uint64_t v, unsigned bytes)
{
    for (unsigned i = 0; i < bytes; i++)
        p[i] = (uint8_t)(v >> (8 * i));
}

static uint64_t get_le(const uint8_t *p, unsigned bytes)
{
    uint64_t v = 0;
    for (unsigned i = 0; i < bytes; i++)
        v |= (uint64_t)p[i] << (8 * i);
    return v;
}

/* Two's complement reinterpretation without implementation-defined casts. */
static int64_t sx(uint64_t u, unsigned bits)
{
    uint64_t sign = (uint64_t)1 << (bits - 1);
    if (u & sign)
        return -(int64_t)((~u) & (sign - 1u)) - 1;
    return (int64_t)u;
}

int rec_gps_fix_encode(uint8_t out[REC_GPS_FIX_LEN], const rec_gps_fix_t *v)
{
    if (!out || !v)
        return -EINVAL;
    put_le(out + 0, (uint64_t)v->utc_unix_ms, 8);
    put_le(out + 8, (uint32_t)v->lat_e7, 4);
    put_le(out + 12, (uint32_t)v->lon_e7, 4);
    put_le(out + 16, (uint32_t)v->alt_cm, 4);
    put_le(out + 20, v->speed_cmps > 0xFFFFu ? 0xFFFFu : v->speed_cmps, 2);
    put_le(out + 22, v->course_cdeg, 2);
    put_le(out + 24, v->hdop_x100, 2);
    out[26] = v->sats;
    out[27] = v->fix_quality;
    out[28] = v->flags;
    out[29] = out[30] = out[31] = 0;
    return 0;
}

int rec_gps_fix_decode(const uint8_t *buf, size_t len, rec_gps_fix_t *v)
{
    if (!buf || !v)
        return -EINVAL;
    if (len != REC_GPS_FIX_LEN || buf[29] || buf[30] || buf[31])
        return -EBADMSG;
    v->utc_unix_ms = sx(get_le(buf + 0, 8), 64);
    v->lat_e7 = (int32_t)sx(get_le(buf + 8, 4), 32);
    v->lon_e7 = (int32_t)sx(get_le(buf + 12, 4), 32);
    v->alt_cm = (int32_t)sx(get_le(buf + 16, 4), 32);
    v->speed_cmps = (uint32_t)get_le(buf + 20, 2);
    v->course_cdeg = (uint16_t)get_le(buf + 22, 2);
    v->hdop_x100 = (uint16_t)get_le(buf + 24, 2);
    v->sats = buf[26];
    v->fix_quality = buf[27];
    v->flags = buf[28];
    return 0;
}

int rec_imu_encode(uint8_t out[REC_IMU_LEN], const rec_imu_t *v)
{
    if (!out || !v)
        return -EINVAL;
    for (unsigned i = 0; i < 3; i++) {
        put_le(out + 2 * i, (uint16_t)v->acc_mg[i], 2);
        put_le(out + 6 + 2 * i, (uint16_t)v->gyr_ddps[i], 2);
    }
    put_le(out + 12, (uint16_t)v->pitch_cdeg, 2);
    put_le(out + 14, (uint16_t)v->roll_cdeg, 2);
    out[16] = v->n_samples;
    out[17] = v->status;
    return 0;
}

int rec_imu_decode(const uint8_t *buf, size_t len, rec_imu_t *v)
{
    if (!buf || !v)
        return -EINVAL;
    if (len != REC_IMU_LEN)
        return -EBADMSG;
    for (unsigned i = 0; i < 3; i++) {
        v->acc_mg[i] = (int16_t)sx(get_le(buf + 2 * i, 2), 16);
        v->gyr_ddps[i] = (int16_t)sx(get_le(buf + 6 + 2 * i, 2), 16);
    }
    v->pitch_cdeg = (int16_t)sx(get_le(buf + 12, 2), 16);
    v->roll_cdeg = (int16_t)sx(get_le(buf + 14, 2), 16);
    v->n_samples = buf[16];
    v->status = buf[17];
    return 0;
}

int rec_time_sync_encode(uint8_t out[REC_TIME_SYNC_LEN], const rec_time_sync_t *v)
{
    if (!out || !v)
        return -EINVAL;
    put_le(out, (uint64_t)v->utc_unix_us, 8);
    out[8] = v->source;
    return 0;
}

int rec_time_sync_decode(const uint8_t *buf, size_t len, rec_time_sync_t *v)
{
    if (!buf || !v)
        return -EINVAL;
    if (len != REC_TIME_SYNC_LEN)
        return -EBADMSG;
    v->utc_unix_us = sx(get_le(buf, 8), 64);
    v->source = buf[8];
    return 0;
}

int rec_motion_encode(uint8_t out[REC_MOTION_LEN], const rec_motion_t *v)
{
    if (!out || !v)
        return -EINVAL;
    put_le(out + 0, v->event_no, 4);
    out[4] = v->kind;
    out[5] = v->max_energy;
    put_le(out + 6, v->dist_cm, 2);
    put_le(out + 8, v->min_dist_cm, 2);
    put_le(out + 10, v->max_dist_cm, 2);
    put_le(out + 12, v->onset_esp_us, 8);
    put_le(out + 20, v->duration_ms, 4);
    out[24] = out[25] = out[26] = out[27] = 0;
    return 0;
}

int rec_motion_decode(const uint8_t *buf, size_t len, rec_motion_t *v)
{
    if (!buf || !v)
        return -EINVAL;
    if (len != REC_MOTION_LEN || buf[24] || buf[25] || buf[26] || buf[27])
        return -EBADMSG;
    if (buf[4] != MOTION_KIND_START && buf[4] != MOTION_KIND_END)
        return -EBADMSG;
    v->event_no = (uint32_t)get_le(buf + 0, 4);
    v->kind = buf[4];
    v->max_energy = buf[5];
    v->dist_cm = (uint16_t)get_le(buf + 6, 2);
    v->min_dist_cm = (uint16_t)get_le(buf + 8, 2);
    v->max_dist_cm = (uint16_t)get_le(buf + 10, 2);
    v->onset_esp_us = get_le(buf + 12, 8);
    v->duration_ms = (uint32_t)get_le(buf + 20, 4);
    return 0;
}

int rec_record_check(uint8_t type, size_t len)
{
    switch (type) {
    case REC_TYPE_LD2410_FRAME:
        return (len >= REC_LD2410_MIN_LEN && len <= RB_MAX_RAW) ? 0 : -EBADMSG;
    case REC_TYPE_GPS_FIX:
        return len == REC_GPS_FIX_LEN ? 0 : -EBADMSG;
    case REC_TYPE_IMU:
        return len == REC_IMU_LEN ? 0 : -EBADMSG;
    case REC_TYPE_TIME_SYNC:
        return len == REC_TIME_SYNC_LEN ? 0 : -EBADMSG;
    case REC_TYPE_MOTION:
        return len == REC_MOTION_LEN ? 0 : -EBADMSG;
    default:
        return -ENOTSUP;
    }
}
