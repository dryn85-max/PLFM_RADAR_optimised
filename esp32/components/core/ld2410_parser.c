#include "ld2410_parser.h"
#include <string.h>

static const uint8_t HDR_DATA[4] = {0xF4, 0xF3, 0xF2, 0xF1};
static const uint8_t FTR_DATA[4] = {0xF8, 0xF7, 0xF6, 0xF5};
static const uint8_t HDR_CMD[4] = {0xFD, 0xFC, 0xFB, 0xFA};
static const uint8_t FTR_CMD[4] = {0x04, 0x03, 0x02, 0x01};

typedef enum { CL_INVALID, CL_PARTIAL, CL_COMPLETE } classify_t;

/* Classify b[0..n) (n >= 1): can it still become a frame starting at b[0]?
 * On CL_COMPLETE, *total is the frame length (<= n; extra bytes may follow)
 * and *kind its kind. Every byte present is checked, so a frame reported as
 * complete has a correct header, a legal length and a correct footer. */
static classify_t classify(const uint8_t *b, size_t n, size_t *total,
                           ld_frame_kind_t *kind)
{
    size_t h = n < 4 ? n : 4;
    int is_data = memcmp(b, HDR_DATA, h) == 0;
    int is_cmd = memcmp(b, HDR_CMD, h) == 0;
    if (!is_data && !is_cmd)
        return CL_INVALID;
    if (n < 6)
        return CL_PARTIAL;
    size_t plen = (size_t)b[4] | ((size_t)b[5] << 8);
    if (plen == 0 || plen > LD_MAX_PAYLOAD)
        return CL_INVALID;
    size_t fpos = 6 + plen;
    size_t tot = fpos + 4;
    if (n > fpos) {
        size_t m = n - fpos < 4 ? n - fpos : 4;
        if (memcmp(b + fpos, is_data ? FTR_DATA : FTR_CMD, m) != 0)
            return CL_INVALID;
    }
    if (n < tot)
        return CL_PARTIAL;
    *total = tot;
    *kind = is_data ? LD_FRAME_DATA : LD_FRAME_CMD;
    return CL_COMPLETE;
}

void ld_parser_init(ld_parser_t *p)
{
    memset(p, 0, sizeof *p);
}

void ld_parser_feed(ld_parser_t *p, const uint8_t *data, size_t len,
                    ld_frame_cb cb, void *ctx)
{
    for (size_t i = 0; i < len; i++) {
        /* Invariant: p->n < LD_MAX_FRAME here (the buffer holds only a
         * partial frame between calls), so this write is in bounds. */
        p->buf[p->n++] = data[i];
        while (p->n > 0) {
            size_t total = 0;
            ld_frame_kind_t kind = LD_FRAME_DATA;
            classify_t c = classify(p->buf, p->n, &total, &kind);
            if (c == CL_PARTIAL)
                break;
            if (c == CL_INVALID) {
                memmove(p->buf, p->buf + 1, --p->n);
                p->dropped_bytes++;
                continue;
            }
            ld_frame_t f;
            f.kind = kind;
            f.raw = p->buf;
            f.raw_len = total;
            f.payload = p->buf + 6;
            f.payload_len = total - LD_FRAME_OVERHEAD;
            p->frames++;
            if (cb)
                cb(&f, ctx);
            p->n -= total;
            memmove(p->buf, p->buf + total, p->n);
        }
    }
}
