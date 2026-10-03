/* CSRF guard for the AP-only state-changing endpoints (pure, host-tested).
 *
 * origin_allowed() decides whether a request's Origin header may pass:
 *   - origin == NULL: header absent (curl and other non-browser clients send none) -> allowed.
 *   - otherwise `origin` (len bytes, not necessarily NUL-terminated) must equal `expected`
 *     as a whole string, ASCII case-insensitively (scheme and host are case-insensitive in
 *     URLs; browsers serialise them in lowercase anyway). No trailing slash, port or other
 *     variants are accepted. "null", an empty string, a length mismatch or an embedded NUL
 *     are all rejected. */
#ifndef ORIGIN_CHECK_H
#define ORIGIN_CHECK_H

#include <stdbool.h>
#include <stddef.h>

bool origin_allowed(const char *origin, size_t len, const char *expected);

#endif
