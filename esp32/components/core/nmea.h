/* NMEA 0183 parser for the NEO-6M GPS (plain C11, no ESP-IDF, no floats).
 *
 * Incremental: feed it the raw UART byte stream in chunks of any size.
 *   - Line assembler: '$' starts a line, CR or LF ends it. A line longer than
 *     NMEA_MAX_LINE characters (excluding CR LF) is dropped and counted once.
 *     A '$' inside a line abandons the partial line (counted as dropped) and
 *     starts a new one. Bytes outside a line are ignored silently.
 *   - Checksum "*HH" (upper or lower case hex) is required; a missing, wrong or
 *     truncated checksum, a non-printable byte or a malformed RMC/GGA counts as
 *     a bad line. Every other checksum-valid sentence counts as good and is ignored.
 *   - RMC and GGA of talker IDs GP and GN are decoded; other talkers are ignored.
 *   - Empty or malformed fields are "not valid": the matching GPS_FLAG_* stays
 *     clear and the value is 0.
 *
 * Validity rules (conservative, decided here):
 *   - RMC status must be 'A' (and the NMEA 2.3 mode field, when present, not 'N')
 *     for position, time and date to be flagged valid; a receiver without a fix
 *     can report a free-running RTC time or the GPS epoch date, which must not
 *     become a UTC time stamp.
 *   - GGA needs fix quality > 0 for position, altitude and time.
 *   - utc_unix_ms is non-zero only when RMC time and date are both flagged valid.
 *   - Leap second 60 is rejected (time not valid for that second).
 *
 * Units: lat/lon degrees * 1e7 (exact integer math), speed knots -> cm/s
 * (saturates at 65535), course centi-degrees, HDOP * 100 (saturates), altitude cm
 * (saturates to int32), UTC unix ms (two-digit year yy -> 20yy).
 *
 * Epochs: RMC and GGA with the same UTC time field belong to one epoch (an empty
 * time field is its own key, so two no-time sentences of different types merge).
 * A fix is emitted when both were seen, or when a sentence of a different time
 * (or a repeated type) begins a new epoch (the incomplete one is emitted first),
 * or by nmea_flush(). Events are delivered synchronously from nmea_feed(), so
 * the caller can stamp "now" when the RMC line completed. Order for one line:
 * stale epoch FIX, then the RMC event, then (if the epoch completed) its FIX. */
#ifndef NMEA_H
#define NMEA_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "rec_payload.h"

#define NMEA_MAX_LINE 82u /* characters between '$' and CR/LF, inclusive of "$" and "*HH" */
#define NMEA_MAX_FIELDS 20u

typedef enum {
    NMEA_EV_RMC = 0, /* an RMC line completed; fix holds only RMC-derived fields */
    NMEA_EV_FIX = 1  /* an epoch is complete (or flushed/superseded) */
} nmea_ev_kind_t;

typedef struct {
    nmea_ev_kind_t kind;
    rec_gps_fix_t fix;
    bool have_rmc; /* FIX only: the epoch contained an RMC */
    bool have_gga; /* FIX only: the epoch contained a GGA */
} nmea_event_t;

typedef void (*nmea_cb_t)(const nmea_event_t *ev, void *ctx);

typedef enum { NMEA_ST_IDLE = 0, NMEA_ST_LINE = 1, NMEA_ST_SKIP = 2 } nmea_state_t;

typedef struct {
    char line[NMEA_MAX_LINE + 1];
    size_t len;
    nmea_state_t state;

    /* Pending epoch. */
    bool ep_active;
    int32_t ep_key; /* UTC time of day in ms, or -1 when the time field is empty/invalid */
    bool ep_have_rmc;
    bool ep_have_gga;
    rec_gps_fix_t ep_rmc;
    rec_gps_fix_t ep_gga;

    uint32_t good_lines;    /* valid checksum (any sentence type) */
    uint32_t bad_lines;     /* checksum missing/wrong, malformed RMC/GGA, non-printable */
    uint32_t dropped_lines; /* overlong, or abandoned by a new '$' */
} nmea_t;

void nmea_init(nmea_t *p);
void nmea_feed(nmea_t *p, const uint8_t *data, size_t len, nmea_cb_t cb, void *ctx);
/* Emit the pending incomplete epoch, if any. */
void nmea_flush(nmea_t *p, nmea_cb_t cb, void *ctx);

/* Days since 1970-01-01 of a proportional Gregorian date, -1 if the date is not
 * a real calendar day (month 1-12, day within the month incl. leap years). The
 * year may be any value in 1970..2099. Exposed for tests. */
int32_t nmea_days_from_civil(int year, int month, int day);

#endif
