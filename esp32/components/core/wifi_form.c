#include "wifi_form.h"

#include <string.h>

void wf_gen_ap_pass(uint32_t (*rnd)(void), char *out)
{
    for (size_t i = 0; i < WF_AP_PASS_LEN; i++) {
        out[i] = WF_AP_ALPHABET[(rnd() >> 7) & 31u];
    }
    out[WF_AP_PASS_LEN] = '\0';
}

static int hexval(char c)
{
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

/* Decode in[0..n) into out (capacity max + 1). Returns length or a negative WF_E_*. */
static int decode_value(const char *in, size_t n, char *out, size_t max)
{
    size_t o = 0;
    for (size_t i = 0; i < n; i++) {
        char c = in[i];
        if (c == '%') {
            if (i + 2 >= n) return WF_E_BAD_ESCAPE;
            int h = hexval(in[i + 1]), l = hexval(in[i + 2]);
            if (h < 0 || l < 0) return WF_E_BAD_ESCAPE;
            c = (char)((h << 4) | l);
            if (c == '\0') return WF_E_BAD_ESCAPE;
            i += 2;
        } else if (c == '+') {
            c = ' ';
        }
        if (o >= max) return WF_E_TOO_LONG;
        out[o++] = c;
    }
    out[o] = '\0';
    return (int)o;
}

int wf_parse_credentials(const char *body, size_t len, char *ssid, char *pass)
{
    if (body == NULL || ssid == NULL || pass == NULL) return WF_E_MISSING;
    if (len > WF_BODY_MAX) return WF_E_TOO_LONG;

    int have_ssid = 0, have_pass = 0;
    size_t pos = 0;
    while (pos < len) {
        size_t end = pos;
        while (end < len && body[end] != '&') end++;
        const char *eq = memchr(body + pos, '=', end - pos);
        if (eq != NULL) {
            size_t klen = (size_t)(eq - (body + pos));
            const char *val = eq + 1;
            size_t vlen = (size_t)((body + end) - val);
            int *have = NULL;
            char *dst = NULL;
            size_t max = 0;
            if (klen == 4 && memcmp(body + pos, "ssid", 4) == 0) {
                have = &have_ssid; dst = ssid; max = WF_SSID_MAX;
            } else if (klen == 8 && memcmp(body + pos, "password", 8) == 0) {
                have = &have_pass; dst = pass; max = WF_PASS_MAX;
            }
            if (have != NULL) {
                if (*have) return WF_E_DUPLICATE;
                int r = decode_value(val, vlen, dst, max);
                if (r < 0) return r;
                *have = 1;
            }
        }
        pos = end + 1;
    }
    if (!have_ssid || !have_pass || ssid[0] == '\0') return WF_E_MISSING;
    size_t plen = strlen(pass);
    if (plen != 0 && plen < WF_PASS_MIN) return WF_E_BAD_PASSWORD;
    return WF_OK;
}
