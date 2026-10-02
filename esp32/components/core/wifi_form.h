/* Wi-Fi setup helpers (plain C11): AP password generation and decoding of the
 * application/x-www-form-urlencoded body of the setup page. */
#ifndef WIFI_FORM_H
#define WIFI_FORM_H

#include <stddef.h>
#include <stdint.h>

#define WF_SSID_MAX 32u      /* decoded bytes */
#define WF_PASS_MAX 64u      /* decoded bytes */
#define WF_PASS_MIN 8u       /* WPA2; an empty password means an open network */
#define WF_BODY_MAX 512u     /* largest accepted request body */

#define WF_AP_PASS_LEN 12u
/* 32 characters, no 0/O/1/l/I; 32 = 2^5 so masking 5 random bits is unbiased. */
#define WF_AP_ALPHABET "ABCDEFGHJKLMNPQRSTUVWXYZ23456789"

enum {
    WF_OK = 0,
    WF_E_MISSING = -1,      /* ssid or password missing, or ssid empty */
    WF_E_BAD_ESCAPE = -2,   /* invalid %XX or an encoded NUL */
    WF_E_TOO_LONG = -3,
    WF_E_DUPLICATE = -4,    /* ssid or password given twice */
    WF_E_BAD_PASSWORD = -5  /* non-empty but shorter than WF_PASS_MIN */
};

/* Fill out[WF_AP_PASS_LEN + 1] with a NUL-terminated password; rnd returns 32 random bits. */
void wf_gen_ap_pass(uint32_t (*rnd)(void), char *out);

/* Parse "ssid=...&password=..." (body need not be NUL-terminated). Unknown keys are ignored.
 * ssid needs WF_SSID_MAX + 1 bytes, pass WF_PASS_MAX + 1 bytes; both NUL-terminated on success.
 * Returns WF_OK or a negative WF_E_*; outputs are unspecified on error. */
int wf_parse_credentials(const char *body, size_t len, char *ssid, char *pass);

/* Decode in[0..n) (%XX and '+') into out (capacity max + 1), NUL-terminated.
 * Returns the decoded length or WF_E_BAD_ESCAPE / WF_E_TOO_LONG (more than max bytes). */
int wf_url_decode(const char *in, size_t n, char *out, size_t max);

#endif
