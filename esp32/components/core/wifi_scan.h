/* Wi-Fi scan list helpers (plain C11): sort/dedupe/cap of scan records and HTML rendering of
 * one list entry into a bounded buffer. No ESP-IDF types; the caller converts wifi_ap_record_t. */
#ifndef WIFI_SCAN_H
#define WIFI_SCAN_H

#include <stddef.h>
#include <stdint.h>

#define WSC_LIST_MAX 20u   /* entries shown on the setup page */
#define WSC_RAW_MAX 40u    /* scan results read from the driver before dedupe */
#define WSC_ENTRY_MAX 640u /* buffer that always fits one rendered entry (checked by a test) */

typedef struct {
    uint8_t ssid[32]; /* raw bytes, not NUL-terminated */
    uint8_t ssid_len; /* 0..32 */
    int8_t rssi;      /* dBm */
    uint8_t channel;
    uint8_t secure;   /* 0 = open, otherwise secured */
} wsc_rec;

/* In place: drop empty SSIDs, sort by RSSI (strongest first; ties by SSID bytes, then channel),
 * keep only the strongest record of each SSID, keep at most cap records.
 * Returns the new count (<= cap). A record with ssid_len > 32 is treated as empty. */
size_t wsc_prepare(wsc_rec *recs, size_t n, size_t cap);

/* HTML-escape ssid[0..len) into out (NUL-terminated). & < > " ' become entities; control
 * characters (< 0x20, 0x7F), invalid UTF-8 and overlong/surrogate sequences become '?'.
 * Returns the bytes written without the NUL, or -ENOSPC if out (cap bytes) is too small
 * (out is then an empty string when cap > 0). *lossy (may be NULL) is set to 1 when any byte
 * was replaced by '?', else 0. */
int wsc_html_escape(char *out, size_t cap, const uint8_t *ssid, size_t len, int *lossy);

/* Render one list item "<li>...</li>\n" into out (NUL-terminated). The SSID is a clickable
 * link (data-s attribute, filled into the SSID field by the page script) unless escaping was
 * lossy, in which case it is shown with "enter manually".
 * Returns the length without the NUL, or -ENOSPC (out is then an empty string when cap > 0). */
int wsc_render_entry(char *out, size_t cap, const wsc_rec *r);

/* Render n entries back to back; same return convention as wsc_render_entry. */
int wsc_render_list(char *out, size_t cap, const wsc_rec *recs, size_t n);

#endif
