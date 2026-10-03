/* u-blox UBX binary protocol for the NEO-6M (plain C11, no ESP-IDF).
 * Reference: u-blox 6 Receiver Description incl. Protocol Specification
 * (GPS.G6-SW-10018), hardware/datasheets/.
 *
 * Packet (section 23, p. 84): 0xB5 0x62, class u8, id u8, length u16 LE (payload
 * only), payload, CK_A CK_B. Checksum (section 26, p. 85-86): 8-bit Fletcher over
 * class, id, length and payload. Polling (section 27.2, p. 86): a message is polled
 * by sending its class/id with an empty payload.
 *
 * Parser: feed the raw byte stream in chunks of any size. A frame whose length is in
 * (UBX_MAX_PAYLOAD, UBX_SKIP_MAX] is skipped by its length (payload and checksum
 * bytes are discarded without verification) and counted in `skipped`. A length above
 * UBX_SKIP_MAX is treated as a false sync (e.g. a stray B5 62 in NMEA text): counted
 * in `bad_len` (not in `skipped`), no bytes are skipped and the search restarts after
 * the first sync byte. On a bad checksum the frame is counted in `bad_ck` and the
 * search restarts the same way: the bytes already received after the first sync byte
 * are re-examined, so a good frame hidden inside a corrupted one is still found. */
#ifndef UBX_H
#define UBX_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define UBX_SYNC1 0xB5u
#define UBX_SYNC2 0x62u
#define UBX_MAX_PAYLOAD 64u
#define UBX_SKIP_MAX 512u /* longest frame still skipped by its length */
#define UBX_CLASS_NAV 0x01u
#define UBX_ID_NAV_TIMEUTC 0x21u
#define UBX_TIMEUTC_LEN 20u
#define UBX_POLL_LEN 8u

void ubx_checksum(const uint8_t *buf, size_t n, uint8_t *ck_a, uint8_t *ck_b);

/* Encode a poll request (empty payload). Returns the length (UBX_POLL_LEN), or -1
 * when cap is too small. */
int ubx_encode_poll(uint8_t cls, uint8_t id, uint8_t *out, size_t cap);

typedef void (*ubx_cb_t)(uint8_t cls, uint8_t id, const uint8_t *payload, size_t len, void *ctx);

typedef enum {
    UBX_ST_IDLE = 0, /* searching for 0xB5 */
    UBX_ST_SYNC2,
    UBX_ST_CLASS,
    UBX_ST_ID,
    UBX_ST_LEN1,
    UBX_ST_LEN2,
    UBX_ST_PAYLOAD,
    UBX_ST_SKIP, /* oversized frame: discarding payload + checksum by length */
    UBX_ST_CKA,
    UBX_ST_CKB
} ubx_state_t;

typedef struct {
    ubx_state_t state;
    uint8_t cls;
    uint8_t id;
    uint16_t len;
    uint32_t idx; /* payload bytes received, or bytes left to skip in UBX_ST_SKIP */
    uint8_t ck_a; /* running */
    uint8_t ck_b;
    uint8_t rx_a; /* received CK_A */
    uint8_t payload[UBX_MAX_PAYLOAD];

    uint32_t good;
    uint32_t bad_ck;
    uint32_t skipped; /* frames with length in (UBX_MAX_PAYLOAD, UBX_SKIP_MAX] */
    uint32_t bad_len; /* false syncs: length > UBX_SKIP_MAX */
} ubx_t;

void ubx_init(ubx_t *p);
void ubx_feed(ubx_t *p, const uint8_t *data, size_t len, ubx_cb_t cb, void *ctx);
/* Drop any frame in progress (no counter changes). */
void ubx_abort(ubx_t *p);
static inline bool ubx_idle(const ubx_t *p) { return p->state == UBX_ST_IDLE; }
/* True right after a lone 0xB5 (waiting for 0x62). */
static inline bool ubx_wait_sync2(const ubx_t *p) { return p->state == UBX_ST_SYNC2; }

/* NAV-TIMEUTC (class 0x01, id 0x21), section 35.13, p. 179-180: payload exactly 20 bytes. */
typedef struct {
    uint32_t itow;  /* ms, GPS time of week */
    uint32_t t_acc; /* ns */
    int32_t nano;   /* ns of second, -1e9..1e9 */
    uint16_t year;
    uint8_t month;
    uint8_t day;
    uint8_t hour;
    uint8_t min;
    uint8_t sec;
    uint8_t valid; /* bit0 validTOW, bit1 validWKN, bit2 validUTC (p. 180) */
} ubx_timeutc_t;

/* 0 on success, -1 when len != 20 (or arguments are NULL). */
int ubx_decode_nav_timeutc(const uint8_t *payload, size_t len, ubx_timeutc_t *out);
static inline bool ubx_timeutc_valid_tow(const ubx_timeutc_t *t) { return (t->valid & 0x01u) != 0; }
static inline bool ubx_timeutc_valid_wkn(const ubx_timeutc_t *t) { return (t->valid & 0x02u) != 0; }
static inline bool ubx_timeutc_valid_utc(const ubx_timeutc_t *t) { return (t->valid & 0x04u) != 0; }

#endif
