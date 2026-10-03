/* Live-view snapshot -> JSON text (plain C11). */
#ifndef SNAPSHOT_JSON_H
#define SNAPSHOT_JSON_H

#include <stddef.h>
#include <stdint.h>
#include "ld2410_frame.h"
#include "rec_payload.h"
#include "time_source.h"

typedef enum {
    SNAP_LINK_NO_DATA = 0, /* nothing received yet */
    SNAP_LINK_OK = 1,
    SNAP_LINK_LOST = 2     /* frames stopped arriving */
} snap_link_t;

typedef enum {
    SNAP_GPS_ABSENT = 0, /* no byte ever received from the module */
    SNAP_GPS_SILENT = 1, /* bytes seen, but no valid NMEA for > 3 s */
    SNAP_GPS_OK = 2
} snap_gps_t;

/* UBX NAV-TIMEUTC state (spec R4/R5). Zero (UNKNOWN) is what a zero-initialised
 * snapshot_t carries: the GPS task has not reported a state -> "utc_state": null. */
typedef enum {
    SNAP_UTC_UNKNOWN = 0, /* not reported -> null */
    SNAP_UTC_NO_UBX = 1,  /* no NAV-TIMEUTC answer from the module -> "no_ubx" */
    SNAP_UTC_NOT_VALID = 2, /* answers arrive, validUTC = 0 -> "not_valid" */
    SNAP_UTC_VALID = 3    /* validUTC = 1 -> "valid" */
} snap_utc_t;

typedef enum {
    SNAP_IMU_ABSENT = 0, /* no sensor answered since boot */
    SNAP_IMU_ERROR = 1,  /* answered before, now failing or stale */
    SNAP_IMU_OK = 2
} snap_imu_t;

/* Upper bound of any snapshot_json() output including the NUL; a test checks
 * the worst case against it. Callers size their buffers with it. */
#define SNAPSHOT_JSON_MAX 1024

typedef struct {
    uint32_t seq;      /* shared ring sequence (recording correlation) */
    uint32_t frame_no; /* LD2410C data frames decoded since boot (wraps) */
    uint64_t esp_time_us;
    snap_link_t link;
    uint8_t have_frame; /* 0: `data` not valid, JSON carries "data":null */
    ld_data_t data;
    snap_gps_t gps_status;
    uint8_t have_gps;      /* 0: `gps` not valid (no epoch yet) */
    rec_gps_fix_t gps;
    snap_utc_t gps_utc_state; /* 0 (zero-init) = unknown */
    snap_imu_t imu_status;
    uint8_t have_imu;      /* 0: `imu` not valid (no record yet) */
    rec_imu_t imu;
    time_source_t time_source;
} snapshot_t;

/* Top-level keys: seq, frame_no, esp_time_us, link, data, then after "data": "gps", "imu", "time_source".
 *   gps: {status:"absent"|"silent"|"ok", fix:bool, fix_quality, sats, hdop,
 *         lat, lon, alt_m, speed_kmh, course_deg, utc, time_valid, pos_valid,
 *         utc_state:"valid"|"not_valid"|"no_ubx"|null}  (last key; null = unknown, also
 *         for any out-of-range enum value; independent of status/have_gps)
 *   imu: {status:"absent"|"error"|"ok", pitch_deg, roll_deg, valid:bool}
 *   time_source: "gps"|"sntp"|"none"
 * Values that are not valid are null (everything but status/flags when the
 * status is not "ok"). lat/lon (7 decimals) and the other decimals are
 * formatted from integers, never with float printf. utc is ISO 8601
 * "YYYY-MM-DDTHH:MM:SS.mmmZ" and only present when time and date are valid.
 * fix = position valid and fix_quality > 0.
 *
 * Writes a NUL-terminated JSON object into out[cap]. Returns its length, or
 * -EINVAL, or -ENOSPC when it does not fit (never truncates; out is then an
 * empty string). Gate arrays appear only for engineering frames. */
int snapshot_json(char *out, size_t cap, const snapshot_t *s);

#endif
