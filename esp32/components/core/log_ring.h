/* Byte ring for the web console log, plus line helpers (plain C11).
 *
 * The ring keeps the most recent `cap` bytes written (cap > 0, any size) in a
 * caller-provided block. Every byte has a 32-bit stream offset: the first byte
 * ever written has offset 0 (or the start given to log_ring_init_at), `head`
 * is the offset one past the newest byte. Offsets wrap at 2^32; all offset
 * math is uint32 subtraction (valid because at most cap < 2^31 bytes are held).
 * `used` (bytes held, <= cap) and `wpos` (physical write index) are internal
 * state kept next to `head` so reads stay correct across the 2^32 wrap even
 * when cap is not a power of two.
 *
 * A reader polls with log_ring_read(from=its last `next`). If the bytes it
 * wants were overwritten, or `from` is not a valid offset (ahead of head), the
 * read restarts at the oldest held byte and sets *gap.
 *
 * NOT THREAD-SAFE: the caller must hold a lock around every call that takes
 * the same log_ring_t.
 *
 * log_line_clean and log_line_is_secret work on one whole formatted line; the
 * caller passes complete lines, so no state is carried between calls. */
#ifndef LOG_RING_H
#define LOG_RING_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

typedef struct {
    uint8_t *buf;
    uint32_t cap;
    uint32_t head; /* total bytes ever written (+ start offset), wraps at 2^32 */
    uint32_t used; /* bytes currently held, <= cap */
    uint32_t wpos; /* physical index of the next write, < cap */
} log_ring_t;

void log_ring_init(log_ring_t *r, void *mem, uint32_t cap);
/* As log_ring_init but the stream offset starts at `start` (tests of the
 * 2^32 wrap). */
void log_ring_init_at(log_ring_t *r, void *mem, uint32_t cap, uint32_t start);

/* Append; if len > cap only the last cap bytes are kept (head still advances
 * by the full len). */
void log_ring_write(log_ring_t *r, const char *data, size_t len);

/* Copy up to `max` bytes starting at offset `from` into out. If `from` is
 * older than the oldest held byte, or ahead of head, start at the oldest held
 * byte and set *gap = true (else *gap = false). Returns the byte count;
 * *next = offset of the first byte not returned (feed it back as `from`).
 * `gap` and `next` may be NULL. */
size_t log_ring_read(const log_ring_t *r, uint32_t from, char *out, size_t max,
                     uint32_t *next, bool *gap);

/* Copy `in` to `out` without ANSI CSI sequences (ESC '[' params 0x20..0x3F,
 * final byte 0x40..0x7E). A sequence cut off by the end of `in` is dropped;
 * a malformed one (a non-parameter, non-final byte inside) is dropped up to,
 * not including, that byte; an ESC not followed by '[' is dropped alone.
 * Output is truncated to `cap` bytes; returns the output length. out must
 * not overlap in unless out == in. */
size_t log_line_clean(const char *in, size_t len, char *out, size_t cap);

/* True if the line contains "AP password" (case-sensitive). */
bool log_line_is_secret(const char *line, size_t len);

#endif
