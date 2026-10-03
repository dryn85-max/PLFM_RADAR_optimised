#include "origin_check.h"

#include <string.h>

static char lower(char c) { return (c >= 'A' && c <= 'Z') ? (char)(c - 'A' + 'a') : c; }

bool origin_allowed(const char *origin, size_t len, const char *expected)
{
    if (origin == NULL) return true;
    if (expected == NULL || len == 0 || len != strlen(expected)) return false;
    for (size_t i = 0; i < len; i++) {
        if (origin[i] == '\0') return false; /* embedded NUL: expected has none before len */
        if (lower(origin[i]) != lower(expected[i])) return false;
    }
    return true;
}
