#include "log_ring.h"

#include <string.h>

void log_ring_init_at(log_ring_t *r, void *mem, uint32_t cap, uint32_t start)
{
    r->buf = (uint8_t *)mem;
    r->cap = cap;
    r->head = start;
    r->used = 0;
    r->wpos = 0;
}

void log_ring_init(log_ring_t *r, void *mem, uint32_t cap)
{
    log_ring_init_at(r, mem, cap, 0);
}

void log_ring_write(log_ring_t *r, const char *data, size_t len)
{
    if (len == 0 || r->cap == 0) return;
    r->head += (uint32_t)len; /* wraps at 2^32 by design */
    if (len > r->cap) {
        data += len - r->cap;
        len = r->cap;
    }
    uint32_t n = (uint32_t)len;
    uint32_t first = r->cap - r->wpos;
    if (first > n) first = n;
    memcpy(r->buf + r->wpos, data, first);
    if (n > first) memcpy(r->buf, data + first, n - first);
    r->wpos = (r->wpos + n) % r->cap;
    r->used += n;
    if (r->used > r->cap) r->used = r->cap;
}

size_t log_ring_read(const log_ring_t *r, uint32_t from, char *out, size_t max,
                     uint32_t *next, bool *gap)
{
    uint32_t age = r->head - from; /* bytes between `from` and head */
    bool g = false;
    if (age > r->used) { /* too old, or ahead of head (wraps to a huge age) */
        g = true;
        age = r->used;
        from = r->head - r->used;
    }
    size_t n = age < max ? age : max;
    if (n > 0) {
        /* physical index of `from`: wpos - age, modulo cap (age <= cap) */
        uint32_t pos = (r->wpos + r->cap - age) % r->cap;
        size_t first = r->cap - pos;
        if (first > n) first = n;
        memcpy(out, r->buf + pos, first);
        if (n > first) memcpy(out + first, r->buf, n - first);
    }
    if (gap) *gap = g;
    if (next) *next = from + (uint32_t)n;
    return n;
}

size_t log_line_clean(const char *in, size_t len, char *out, size_t cap)
{
    size_t o = 0;
    size_t i = 0;
    while (i < len) {
        unsigned char c = (unsigned char)in[i];
        if (c != 0x1B) {
            if (o < cap) out[o++] = (char)c;
            i++;
            continue;
        }
        i++; /* drop ESC */
        if (i < len && in[i] == '[') {
            i++;
            while (i < len) {
                unsigned char p = (unsigned char)in[i];
                if (p >= 0x40 && p <= 0x7E) { i++; break; } /* final byte */
                if (p >= 0x20 && p <= 0x3F) { i++; continue; } /* param/interm. */
                break; /* malformed: resume at this byte */
            }
        }
    }
    return o;
}

bool log_line_is_secret(const char *line, size_t len)
{
    static const char key[] = "AP password";
    const size_t kl = sizeof key - 1;
    if (len < kl) return false;
    for (size_t i = 0; i + kl <= len; i++)
        if (memcmp(line + i, key, kl) == 0) return true;
    return false;
}
