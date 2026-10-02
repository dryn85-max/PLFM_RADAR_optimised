/* Incremental LD2410C stream framer (plain C11, no ESP-IDF, no allocation).
 *
 * Recognises two frame kinds:
 *   data      F4 F3 F2 F1 | len u16 LE | payload | F8 F7 F6 F5
 *   command   FD FC FB FA | len u16 LE | payload | 04 03 02 01   (commands and ACKs)
 * Feed it arbitrary chunks; each complete, footer-checked frame is reported
 * through a callback with its raw bytes unchanged.
 *
 * Resync: the parser keeps only bytes that are a valid prefix of a frame.
 * When a byte makes the buffer invalid (garbage, impossible length, wrong
 * footer) leading bytes are dropped one at a time and the rest is re-checked,
 * so a real header that starts inside rejected bytes is not lost.
 * A header-looking sequence inside the payload of a valid frame is part of
 * that frame (one frame is reported).
 *
 * Not thread-safe: one parser per stream, one feeder. */
#ifndef LD2410_PARSER_H
#define LD2410_PARSER_H

#include <stddef.h>
#include <stdint.h>

/* Largest payload accepted. Engineering data frames are about 35 bytes;
 * 64 leaves margin. VERIFY against the Hi-Link manual. */
#define LD_MAX_PAYLOAD 64u
#define LD_FRAME_OVERHEAD 10u /* header 4 + length 2 + footer 4 */
#define LD_MAX_FRAME (LD_MAX_PAYLOAD + LD_FRAME_OVERHEAD)

typedef enum {
    LD_FRAME_DATA = 1,
    LD_FRAME_CMD = 2
} ld_frame_kind_t;

typedef struct {
    ld_frame_kind_t kind;
    const uint8_t *raw;     /* whole frame, header..footer */
    size_t raw_len;
    const uint8_t *payload; /* points into raw */
    size_t payload_len;
} ld_frame_t;

/* The pointers in `frame` are valid only during the callback. */
typedef void (*ld_frame_cb)(const ld_frame_t *frame, void *ctx);

typedef struct {
    uint8_t buf[LD_MAX_FRAME];
    size_t n;
    uint32_t frames;        /* frames emitted */
    uint32_t dropped_bytes; /* bytes discarded while resyncing */
} ld_parser_t;

void ld_parser_init(ld_parser_t *p);
/* Feed `len` bytes; `cb` may be called zero or more times. */
void ld_parser_feed(ld_parser_t *p, const uint8_t *data, size_t len,
                    ld_frame_cb cb, void *ctx);

#endif
