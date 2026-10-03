/* Record types and payload codecs of the recording protocol v3 (plain C11).
 * All integers little-endian, packed byte by byte (no struct overlay).
 * Layouts are normative in docs/superpowers/plans/2026-10-02-esp32-gps-imu.md
 * ("Fixed layouts") and mirrored by host/ld2410_rec.py and the shared vectors
 * in esp32/tests/vectors.
 *
 *   type 0 ld2410_frame: raw LD2410C frame bytes (not decoded here)
 *   type 1 gps_fix  (32 B): utc_unix_ms i64 (0 if time/date invalid), lat_e7 i32,
 *     lon_e7 i32, alt_cm i32, speed_cmps u16, course_cdeg u16, hdop_x100 u16,
 *     sats u8, fix_quality u8 (GGA value), flags u8, reserved u8[3] = 0
 *   type 2 imu      (18 B): acc_mg i16[3], gyr_ddps i16[3] (0.1 deg/s), pitch_cdeg i16,
 *     roll_cdeg i16, n_samples u8, status u8
 *   type 3 time_sync (9 B): utc_unix_us i64, source u8
 *
 * Decoders reject a wrong length and nonzero reserved bytes with -EBADMSG;
 * undefined flag/status/source values are passed through unchanged. */
#ifndef REC_PAYLOAD_H
#define REC_PAYLOAD_H

#include <stddef.h>
#include <stdint.h>

#define REC_TYPE_LD2410_FRAME 0u
#define REC_TYPE_GPS_FIX 1u
#define REC_TYPE_IMU 2u
#define REC_TYPE_TIME_SYNC 3u

#define REC_GPS_FIX_LEN 32u
#define REC_IMU_LEN 18u
#define REC_TIME_SYNC_LEN 9u
#define REC_LD2410_MIN_LEN 10u /* header 4 + length 2 + footer 4 */

#define GPS_FLAG_TIME_VALID 0x01u
#define GPS_FLAG_DATE_VALID 0x02u
#define GPS_FLAG_POS_VALID 0x04u
#define GPS_FLAG_ALT_VALID 0x08u
/* Set only while the last UBX NAV-TIMEUTC had validUTC = 1 (spec R3/R4); never from
 * NMEA alone. rec_gps_fix_encode/decode pass flags through unchanged. */
#define GPS_FLAG_UTC_VERIFIED 0x10u

#define IMU_STATUS_VALID 0x01u

#define TIME_SRC_GPS 1u
#define TIME_SRC_SNTP 2u

typedef struct {
    int64_t utc_unix_ms;
    int32_t lat_e7;
    int32_t lon_e7;
    int32_t alt_cm;
    uint32_t speed_cmps; /* encode saturates to 65535; decode never exceeds it */
    uint16_t course_cdeg;
    uint16_t hdop_x100;
    uint8_t sats;
    uint8_t fix_quality;
    uint8_t flags;
} rec_gps_fix_t;

typedef struct {
    int16_t acc_mg[3];
    int16_t gyr_ddps[3];
    int16_t pitch_cdeg;
    int16_t roll_cdeg;
    uint8_t n_samples;
    uint8_t status;
} rec_imu_t;

typedef struct {
    int64_t utc_unix_us;
    uint8_t source;
} rec_time_sync_t;

/* Encoders: 0, or -EINVAL (null). */
int rec_gps_fix_encode(uint8_t out[REC_GPS_FIX_LEN], const rec_gps_fix_t *v);
int rec_imu_encode(uint8_t out[REC_IMU_LEN], const rec_imu_t *v);
int rec_time_sync_encode(uint8_t out[REC_TIME_SYNC_LEN], const rec_time_sync_t *v);

/* Decoders: 0, -EINVAL (null), -EBADMSG (len != exact size, reserved != 0). */
int rec_gps_fix_decode(const uint8_t *buf, size_t len, rec_gps_fix_t *v);
int rec_imu_decode(const uint8_t *buf, size_t len, rec_imu_t *v);
int rec_time_sync_decode(const uint8_t *buf, size_t len, rec_time_sync_t *v);

/* Classify a (type, payload length) pair: 0 known type with a valid length,
 * -ENOTSUP unknown type (the caller keeps or skips the record, never fails),
 * -EBADMSG known type with a wrong length (type 0: 10..RB_MAX_RAW bytes). */
int rec_record_check(uint8_t type, size_t len);

#endif
