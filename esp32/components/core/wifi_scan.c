#include "wifi_scan.h"

#include <errno.h>
#include <stdio.h>
#include <string.h>

/* ---- sort / dedupe / cap ---- */

static int ssid_cmp(const wsc_rec *a, const wsc_rec *b)
{
    size_t m = a->ssid_len < b->ssid_len ? a->ssid_len : b->ssid_len;
    int c = memcmp(a->ssid, b->ssid, m);
    if (c != 0) return c;
    return (int)a->ssid_len - (int)b->ssid_len;
}

/* true when a goes before b: stronger first, then SSID bytes, then channel */
static int before(const wsc_rec *a, const wsc_rec *b)
{
    if (a->rssi != b->rssi) return a->rssi > b->rssi;
    int c = ssid_cmp(a, b);
    if (c != 0) return c < 0;
    return a->channel < b->channel;
}

size_t wsc_prepare(wsc_rec *recs, size_t n, size_t cap)
{
    /* drop empty / malformed */
    size_t w = 0;
    for (size_t i = 0; i < n; i++) {
        if (recs[i].ssid_len == 0 || recs[i].ssid_len > sizeof recs[i].ssid) continue;
        if (w != i) recs[w] = recs[i];
        w++;
    }
    /* insertion sort (n <= a few dozen) */
    for (size_t i = 1; i < w; i++) {
        wsc_rec k = recs[i];
        size_t j = i;
        while (j > 0 && before(&k, &recs[j - 1])) {
            recs[j] = recs[j - 1];
            j--;
        }
        recs[j] = k;
    }
    /* dedupe: the first of each SSID is the strongest; stop at cap */
    size_t out = 0;
    for (size_t i = 0; i < w && out < cap; i++) {
        int dup = 0;
        for (size_t j = 0; j < out; j++) {
            if (ssid_cmp(&recs[i], &recs[j]) == 0) {
                dup = 1;
                break;
            }
        }
        if (dup) continue;
        if (out != i) recs[out] = recs[i];
        out++;
    }
    return out;
}

/* ---- bounded output ---- */

typedef struct {
    char *p;
    size_t cap;
    size_t len;
    int full;
} sink;

static void put(sink *s, const char *str, size_t n)
{
    if (s->full) return;
    if (n >= s->cap - s->len) { /* need n + 1 for the NUL */
        s->full = 1;
        return;
    }
    memcpy(s->p + s->len, str, n);
    s->len += n;
}

static void puts_(sink *s, const char *str) { put(s, str, strlen(str)); }

static int finish(sink *s)
{
    if (s->full) {
        if (s->cap > 0) s->p[0] = '\0';
        return -ENOSPC;
    }
    s->p[s->len] = '\0';
    return (int)s->len;
}

/* ---- escaping ---- */

/* Length (1..4) of a valid UTF-8 sequence at s[0..left), or 0 if invalid. */
static size_t utf8_len(const uint8_t *s, size_t left)
{
    uint8_t c = s[0];
    if (c < 0x80) return 1;
    size_t n;
    uint32_t cp;
    uint32_t min;
    if (c >= 0xC2 && c <= 0xDF) {
        n = 2; cp = c & 0x1Fu; min = 0x80;
    } else if (c >= 0xE0 && c <= 0xEF) {
        n = 3; cp = c & 0x0Fu; min = 0x800;
    } else if (c >= 0xF0 && c <= 0xF4) {
        n = 4; cp = c & 0x07u; min = 0x10000;
    } else {
        return 0;
    }
    if (left < n) return 0;
    for (size_t i = 1; i < n; i++) {
        if ((s[i] & 0xC0u) != 0x80u) return 0;
        cp = (cp << 6) | (s[i] & 0x3Fu);
    }
    if (cp < min || cp > 0x10FFFFu || (cp >= 0xD800u && cp <= 0xDFFFu)) return 0;
    return n;
}

static void esc_into(sink *s, const uint8_t *ssid, size_t len, int *lossy)
{
    int loss = 0;
    size_t i = 0;
    while (i < len) {
        uint8_t c = ssid[i];
        switch (c) {
        case '&': puts_(s, "&amp;"); i++; continue;
        case '<': puts_(s, "&lt;"); i++; continue;
        case '>': puts_(s, "&gt;"); i++; continue;
        case '"': puts_(s, "&quot;"); i++; continue;
        case '\'': puts_(s, "&#39;"); i++; continue;
        default: break;
        }
        if (c < 0x20 || c == 0x7F) {
            put(s, "?", 1);
            loss = 1;
            i++;
            continue;
        }
        size_t n = utf8_len(ssid + i, len - i);
        if (n == 0) {
            put(s, "?", 1);
            loss = 1;
            i++;
        } else {
            put(s, (const char *)(ssid + i), n);
            i += n;
        }
    }
    if (lossy) *lossy = loss;
}

int wsc_html_escape(char *out, size_t cap, const uint8_t *ssid, size_t len, int *lossy)
{
    sink s = {out, cap, 0, cap == 0};
    esc_into(&s, ssid, len, lossy);
    return finish(&s);
}

/* ---- rendering ---- */

static void entry_into(sink *s, const wsc_rec *r)
{
    /* escape once into a small scratch to learn lossiness before choosing the markup */
    char esc[32 * 6 + 1];
    int lossy = 0;
    size_t len = r->ssid_len > sizeof r->ssid ? sizeof r->ssid : r->ssid_len;
    int n = wsc_html_escape(esc, sizeof esc, r->ssid, len, &lossy);
    if (n < 0) {
        s->full = 1; /* cannot happen: 32 bytes expand to at most 32 * 6 */
        return;
    }
    char num[96];
    int q = r->rssi >= -55 ? 4 : r->rssi >= -65 ? 3 : r->rssi >= -75 ? 2 : 1;
    puts_(s, "<li>");
    puts_(s, lossy ? "<div class=\"r\">" : "<a class=\"r\" href=\"#\" data-s=\"");
    if (!lossy) {
        puts_(s, esc);
        puts_(s, "\">");
    }
    puts_(s, "<span class=\"nm\">");
    puts_(s, esc);
    snprintf(num, sizeof num, "</span><span class=\"mt\"><i class=\"q q%d\"></i>%d dBm &middot; ch %u &middot; ", q,
             (int)r->rssi, (unsigned)r->channel);
    puts_(s, num);
    puts_(s, r->secure ? "&#128274; secured" : "open");
    if (lossy) puts_(s, " &middot; enter manually");
    puts_(s, lossy ? "</span></div></li>\n" : "</span></a></li>\n");
}

int wsc_render_entry(char *out, size_t cap, const wsc_rec *r)
{
    sink s = {out, cap, 0, cap == 0};
    entry_into(&s, r);
    return finish(&s);
}

int wsc_render_list(char *out, size_t cap, const wsc_rec *recs, size_t n)
{
    sink s = {out, cap, 0, cap == 0};
    for (size_t i = 0; i < n; i++) entry_into(&s, &recs[i]);
    return finish(&s);
}
