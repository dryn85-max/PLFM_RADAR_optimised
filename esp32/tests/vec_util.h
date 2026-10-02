/* Test helpers: read vector files, tiny JSON value lookup (flat keys, the
 * vectors are machine-generated), hex decode. Include once per test file. */
#ifndef VEC_UTIL_H
#define VEC_UTIL_H
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static __attribute__((unused)) size_t vec_read(const char *name, uint8_t *buf, size_t cap)
{
    char path[256];
    snprintf(path, sizeof path, "%s/%s", VECTOR_DIR, name);
    FILE *f = fopen(path, "rb");
    if (!f) { printf("  cannot open %s\n", path); exit(2); }
    size_t n = fread(buf, 1, cap - 1, f);
    fclose(f);
    buf[n] = 0; /* text files get a terminator */
    return n;
}

/* Find `"key"` at or after *pos; returns pointer to the value, advances *pos. */
static __attribute__((unused)) const char *json_find(const char *text, const char *key, const char **pos)
{
    char pat[64];
    snprintf(pat, sizeof pat, "\"%s\"", key);
    const char *p = *pos ? *pos : text;
    for (;;) {
        p = strstr(p, pat);
        if (!p) return NULL;
        p += strlen(pat);
        const char *q = p;
        while (*q == ' ') q++;
        if (*q == ':') { p = q; break; } /* a key, not a string value */
    }
    while (*p == ' ' || *p == ':') p++;
    *pos = p;
    return p;
}
static __attribute__((unused)) unsigned long long json_u64(const char *text, const char *key)
{
    const char *pos = NULL;
    const char *v = json_find(text, key, &pos);
    return v ? strtoull(v, NULL, 10) : ~0ULL;
}
static __attribute__((unused)) unsigned long long json_u64_at(const char *text, const char *key, const char **pos)
{
    const char *v = json_find(text, key, pos);
    return v ? strtoull(v, NULL, 10) : ~0ULL;
}
static __attribute__((unused)) void json_arr_u8(const char *text, const char *key, uint8_t *out, size_t n)
{
    const char *pos = NULL;
    const char *v = json_find(text, key, &pos);
    for (size_t i = 0; v && i < n; i++) {
        while (*v == '[' || *v == ',' || *v == ' ' || *v == '\n') v++;
        out[i] = (uint8_t)strtoul(v, (char **)&v, 10);
    }
}
/* Decode the hex string that is the value of `key` (searched from *pos). */
static __attribute__((unused)) size_t json_hex(const char *text, const char *key, const char **pos,
                       uint8_t *out, size_t cap)
{
    const char *v = json_find(text, key, pos);
    size_t n = 0;
    if (!v || *v != '"') return 0;
    for (v++; v[0] && v[0] != '"' && v[1] && n < cap; v += 2) {
        char h[3] = {v[0], v[1], 0};
        out[n++] = (uint8_t)strtoul(h, NULL, 16);
    }
    return n;
}
#endif
