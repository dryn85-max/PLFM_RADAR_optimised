/* LD2410C settings model (plain C11): range check, form parsing, diff, version text.
 * Limits follow the "HLK-LD2410C serial communication protocol" V1.00 (see ld2410_cmd.h). */
#ifndef LD_SETTINGS_H
#define LD_SETTINGS_H

#include <stddef.h>
#include <stdint.h>
#include "ld2410_cmd.h"

#define LDS_BODY_MAX 512u /* largest accepted form body */

typedef struct {
    uint8_t max_move_gate;  /* 2..8 */
    uint8_t max_still_gate; /* 2..8 */
    uint16_t duration_s;    /* 0..65535 */
    uint8_t move_sens[LD_GATE_COUNT];  /* 0..100, gates 0..8 */
    uint8_t still_sens[LD_GATE_COUNT]; /* 0..100; gates 0 and 1 are not settable
                                        * (Table 7, p.15) and ignored by check/diff */
} ld_settings_t;

/* Form fields (application/x-www-form-urlencoded), all 19 required, decimal digits only,
 * at most 5 characters after URL decoding:
 *   mg  max motion gate      sg  max still gate      dur  no-one duration (s)
 *   m0..m8  motion sensitivity of gate 0..8
 *   s2..s8  still sensitivity of gate 2..8   (s0, s1 are not form fields)
 * Unknown fields are ignored. */
enum {
    LDS_OK = 0,
    LDS_E_ARG = -1,       /* null argument */
    LDS_E_MISSING = -2,   /* a required field is absent */
    LDS_E_DUPLICATE = -3, /* a known field given twice */
    LDS_E_BAD_VALUE = -4, /* empty, non-digit (sign, space, ...), or bad %XX escape */
    LDS_E_TOO_LONG = -5,  /* value over 5 characters, or body over LDS_BODY_MAX */
    LDS_E_RANGE = -6      /* out of range for the field */
};

typedef struct {
    int need_gates; /* 1: send 0x0060 with the new gates and duration */
    unsigned n_sens; /* number of 0x0064 commands */
    struct {
        uint8_t gate, move, still;
    } sens[LD_GATE_COUNT];
} ld_diff_t;

#define LD_VERSION_STR_MAX 20u /* "V255.ff.ffffffff" + NUL fits */

/* Copy a decoded read-parameters ACK. Returns 0 or -EINVAL (null). */
int ld_settings_from_params(const ld_params_t *p, ld_settings_t *out);

/* 0 if every settable value is in range, else -EINVAL (null or out of range). */
int ld_settings_check(const ld_settings_t *s);

/* Parse a form body (need not be NUL-terminated). still_sens[0..1] are taken from
 * `current`. Returns LDS_OK or a negative LDS_E_*; *out is untouched on error. */
int ld_settings_parse_form(const char *body, size_t len, const ld_settings_t *current,
                           ld_settings_t *out);

/* What to send to go from `old` to `new_`. 0x0060 is needed when gates or duration differ
 * (it always carries all three new values). A 0x0064 per gate whose move sens differs or,
 * for gates 2..8, whose still sens differs; the still value sent for gates 0 and 1 is the
 * old one (not settable). Returns 0, or -EINVAL (null). Does not range-check. */
int ld_settings_diff(const ld_settings_t *old, const ld_settings_t *new_, ld_diff_t *out);

/* "V<major>>8>.<major&0xFF as 2 hex>.<minor as 8 hex>", e.g. major 0x0107 minor 0x22091516
 * -> "V1.07.22091516". Returns the length (excluding NUL) or -ENOSPC / -EINVAL.
 * VERIFY on the bench: the document (2.2.8, p.13) shows minor bytes 16 15 09 22 as
 * "22091615", which is not the little-endian u32 0x22091516 printed here. */
int ld_version_format(const ld_version_t *v, char *buf, size_t cap);

#endif
