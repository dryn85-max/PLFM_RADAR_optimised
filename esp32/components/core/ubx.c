/* UBX framing: see ubx.h. */
#include "ubx.h"

#include <string.h>

void ubx_checksum(const uint8_t *buf, size_t n, uint8_t *ck_a, uint8_t *ck_b)
{
    uint8_t a = 0, b = 0;
    for (size_t i = 0; i < n; i++) {
        a = (uint8_t)(a + buf[i]);
        b = (uint8_t)(b + a);
    }
    *ck_a = a;
    *ck_b = b;
}

int ubx_encode_poll(uint8_t cls, uint8_t id, uint8_t *out, size_t cap)
{
    if (out == NULL || cap < UBX_POLL_LEN) {
        return -1;
    }
    out[0] = UBX_SYNC1;
    out[1] = UBX_SYNC2;
    out[2] = cls;
    out[3] = id;
    out[4] = 0;
    out[5] = 0;
    ubx_checksum(out + 2, 4, &out[6], &out[7]);
    return (int)UBX_POLL_LEN;
}

void ubx_init(ubx_t *p)
{
    memset(p, 0, sizeof *p);
    p->state = UBX_ST_IDLE;
}

void ubx_abort(ubx_t *p)
{
    p->state = UBX_ST_IDLE;
}

static void ck_add(ubx_t *p, uint8_t c)
{
    p->ck_a = (uint8_t)(p->ck_a + c);
    p->ck_b = (uint8_t)(p->ck_b + p->ck_a);
}

static void step(ubx_t *p, uint8_t c, ubx_cb_t cb, void *ctx);

/* False sync (bad checksum, or an implausible length): restart the search right
 * after the first sync byte, replaying the bytes received since (sync2, class, id,
 * length[, payload, CK_A, CK_B]). Each replay is strictly shorter than the failed
 * frame, so the recursion is bounded. */
static void replay(ubx_t *p, const uint8_t *tmp, size_t n, ubx_cb_t cb, void *ctx)
{
    p->state = UBX_ST_IDLE;
    for (size_t i = 0; i < n; i++) {
        step(p, tmp[i], cb, ctx);
    }
}

static void resync(ubx_t *p, uint8_t rx_b, ubx_cb_t cb, void *ctx)
{
    uint8_t tmp[1 + 2 + 2 + UBX_MAX_PAYLOAD + 2];
    size_t n = 0;
    tmp[n++] = UBX_SYNC2;
    tmp[n++] = p->cls;
    tmp[n++] = p->id;
    tmp[n++] = (uint8_t)(p->len & 0xFFu);
    tmp[n++] = (uint8_t)(p->len >> 8);
    memcpy(tmp + n, p->payload, p->len);
    n += p->len;
    tmp[n++] = p->rx_a;
    tmp[n++] = rx_b;
    replay(p, tmp, n, cb, ctx);
}

static void step(ubx_t *p, uint8_t c, ubx_cb_t cb, void *ctx)
{
    switch (p->state) {
    case UBX_ST_IDLE:
        if (c == UBX_SYNC1) {
            p->state = UBX_ST_SYNC2;
        }
        break;
    case UBX_ST_SYNC2:
        if (c == UBX_SYNC2) {
            p->state = UBX_ST_CLASS;
        } else if (c != UBX_SYNC1) {
            p->state = UBX_ST_IDLE; /* the byte is not a sync: examined from idle */
        }
        break;
    case UBX_ST_CLASS:
        p->cls = c;
        p->ck_a = 0;
        p->ck_b = 0;
        ck_add(p, c);
        p->state = UBX_ST_ID;
        break;
    case UBX_ST_ID:
        p->id = c;
        ck_add(p, c);
        p->state = UBX_ST_LEN1;
        break;
    case UBX_ST_LEN1:
        p->len = c;
        ck_add(p, c);
        p->state = UBX_ST_LEN2;
        break;
    case UBX_ST_LEN2:
        p->len = (uint16_t)(p->len | ((uint16_t)c << 8));
        ck_add(p, c);
        p->idx = 0;
        if (p->len > UBX_SKIP_MAX) {
            /* implausible length: a spurious sync in NMEA text, not a frame */
            const uint8_t tmp[5] = {UBX_SYNC2, p->cls, p->id, (uint8_t)(p->len & 0xFFu),
                                    (uint8_t)(p->len >> 8)};
            p->bad_len++;
            replay(p, tmp, sizeof tmp, cb, ctx);
        } else if (p->len > UBX_MAX_PAYLOAD) {
            p->skipped++;
            /* payload + 2 checksum bytes are discarded, unverified */
            p->idx = (uint32_t)p->len + 2u;
            p->state = UBX_ST_SKIP;
        } else {
            p->state = p->len == 0 ? UBX_ST_CKA : UBX_ST_PAYLOAD;
        }
        break;
    case UBX_ST_PAYLOAD:
        p->payload[p->idx++] = c;
        ck_add(p, c);
        if (p->idx >= p->len) {
            p->state = UBX_ST_CKA;
        }
        break;
    case UBX_ST_SKIP:
        if (--p->idx == 0) {
            p->state = UBX_ST_IDLE;
        }
        break;
    case UBX_ST_CKA:
        p->rx_a = c;
        p->state = UBX_ST_CKB;
        break;
    case UBX_ST_CKB:
        if (p->rx_a == p->ck_a && c == p->ck_b) {
            p->good++;
            p->state = UBX_ST_IDLE;
            if (cb != NULL) {
                cb(p->cls, p->id, p->payload, p->len, ctx);
            }
        } else {
            p->bad_ck++;
            resync(p, c, cb, ctx);
        }
        break;
    }
}

void ubx_feed(ubx_t *p, const uint8_t *data, size_t len, ubx_cb_t cb, void *ctx)
{
    for (size_t i = 0; i < len; i++) {
        step(p, data[i], cb, ctx);
    }
}

int ubx_decode_nav_timeutc(const uint8_t *b, size_t len, ubx_timeutc_t *out)
{
    if (b == NULL || out == NULL || len != UBX_TIMEUTC_LEN) {
        return -1;
    }
    out->itow = (uint32_t)b[0] | ((uint32_t)b[1] << 8) | ((uint32_t)b[2] << 16) | ((uint32_t)b[3] << 24);
    out->t_acc = (uint32_t)b[4] | ((uint32_t)b[5] << 8) | ((uint32_t)b[6] << 16) | ((uint32_t)b[7] << 24);
    uint32_t n = (uint32_t)b[8] | ((uint32_t)b[9] << 8) | ((uint32_t)b[10] << 16) | ((uint32_t)b[11] << 24);
    out->nano = (int32_t)n; /* two's complement, implementation-defined but universal */
    out->year = (uint16_t)(b[12] | (b[13] << 8));
    out->month = b[14];
    out->day = b[15];
    out->hour = b[16];
    out->min = b[17];
    out->sec = b[18];
    out->valid = b[19];
    return 0;
}
