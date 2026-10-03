/* GPS link (GY-NEO6MV2): UART2 reader task, NMEA parser, ring producer, latest fix. */
#ifndef GPS_TASK_H
#define GPS_TASK_H

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"
#include "rec_payload.h"

typedef enum {
    GPS_LINK_ABSENT = 0, /* no byte ever received from the module */
    GPS_LINK_SILENT = 1, /* bytes seen, but no valid NMEA line for more than 3 s */
    GPS_LINK_OK = 2      /* a valid NMEA line within the last 3 s (a fix is not implied) */
} gps_link_t;

/* UBX NAV-TIMEUTC state (spec R3). UNKNOWN: the module is silent or absent. */
typedef enum {
    GPS_UTC_UNKNOWN = 0,
    GPS_UTC_NO_UBX = 1,    /* no NAV-TIMEUTC answer for > 5 s (time_sync fallback) */
    GPS_UTC_NOT_VALID = 2, /* validUTC = 0, or still waiting for the first answer */
    GPS_UTC_VALID = 3
} gps_utc_t;

typedef struct {
    uint32_t good, bad_ck, skipped, bad_len;
} gps_ubx_counters_t;

typedef struct {
    bool valid;            /* false until the first epoch was emitted */
    rec_gps_fix_t fix;     /* latest epoch (flags say what is valid) */
    uint64_t time_us;      /* esp_timer_get_time() when its RMC line completed */
    uint32_t good_lines;
    uint32_t bad_lines;
    uint32_t dropped_lines;
} gps_snapshot_t;

/* Start the GPS task. Needs ld2410_start() to have created the ring. Safe to
 * call without a module attached (status stays GPS_LINK_ABSENT). */
esp_err_t gps_start(void);

/* Copy the latest epoch. Returns false until the first one. */
bool gps_get_snapshot(gps_snapshot_t *out);

gps_link_t gps_status(void);

/* esp_timer time of the latest GPS time_sync record, 0 if none yet. */
uint64_t gps_last_time_sync_us(void);

/* Current UTC state, evaluated at call time (ages out on its own). */
gps_utc_t gps_utc_state(void);

#endif
