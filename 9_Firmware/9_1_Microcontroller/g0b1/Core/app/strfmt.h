/* Tiny bounded string builder with an integer formatter: lets cmd.c and app.c
 * produce text without printf (no float, no stdio dependency). The buffer is
 * always NUL-terminated; output that does not fit is cut and sb->trunc is set. */
#ifndef STRFMT_H
#define STRFMT_H
#include <stddef.h>
#include <stdint.h>

typedef struct { char *buf; size_t cap, len; int trunc; } sbuf_t;

static inline void sb_init(sbuf_t *s, char *buf, size_t cap)
{
    s->buf = buf; s->cap = cap; s->len = 0; s->trunc = 0;
    if (cap > 0) buf[0] = '\0';
}

static inline void sb_putc(sbuf_t *s, char c)
{
    if (s->cap == 0 || s->len + 1 >= s->cap) { s->trunc = 1; return; }
    s->buf[s->len++] = c;
    s->buf[s->len] = '\0';
}

static inline void sb_puts(sbuf_t *s, const char *str)
{
    while (*str != '\0') sb_putc(s, *str++);
}

static inline void sb_put_int(sbuf_t *s, int32_t v)
{
    char tmp[11];
    int n = 0;
    uint32_t u = (v < 0) ? (uint32_t)0 - (uint32_t)v : (uint32_t)v;
    if (v < 0) sb_putc(s, '-');
    do { tmp[n++] = (char)('0' + (int)(u % 10u)); u /= 10u; } while (u != 0u);
    while (n > 0) sb_putc(s, tmp[--n]);
}

static inline void sb_put_uint(sbuf_t *s, uint32_t u)
{
    char tmp[10];
    int n = 0;
    do { tmp[n++] = (char)('0' + (int)(u % 10u)); u /= 10u; } while (u != 0u);
    while (n > 0) sb_putc(s, tmp[--n]);
}

#endif
