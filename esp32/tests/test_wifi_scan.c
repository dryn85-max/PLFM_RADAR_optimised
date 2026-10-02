/* Host tests for wifi_scan: SSID escaping, sort/dedupe/cap, bounded rendering. */
#include <errno.h>
#include <stdio.h>
#include <string.h>
#include "tinytest.h"
#include "wifi_scan.h"

static wsc_rec mk(const char *ssid, int rssi, int ch, int secure)
{
    wsc_rec r;
    memset(&r, 0, sizeof r);
    r.ssid_len = (uint8_t)strlen(ssid);
    memcpy(r.ssid, ssid, r.ssid_len);
    r.rssi = (int8_t)rssi;
    r.channel = (uint8_t)ch;
    r.secure = (uint8_t)secure;
    return r;
}

static int esc(const char *s, char *out, size_t cap, int *lossy)
{
    return wsc_html_escape(out, cap, (const uint8_t *)s, strlen(s), lossy);
}

static void test_escape_specials(void)
{
    char o[64];
    int lossy = 9;
    TT_ASSERT_EQ(24, esc("<>&\"'", o, sizeof o, &lossy));
    TT_ASSERT(strcmp(o, "&lt;&gt;&amp;&quot;&#39;") == 0);
    TT_ASSERT_EQ(0, lossy);
    TT_ASSERT_EQ(5, esc("abc12", o, sizeof o, NULL));
    TT_ASSERT(strcmp(o, "abc12") == 0);
    TT_ASSERT_EQ(0, esc("", o, sizeof o, &lossy));
    TT_ASSERT(o[0] == '\0');
}

static void test_escape_bytes(void)
{
    char o[64];
    int lossy = 0;
    /* control characters */
    const uint8_t ctl[] = {'a', 0x00, 0x01, 0x0A, 0x1F, 0x7F, 'b'};
    TT_ASSERT_EQ(7, wsc_html_escape(o, sizeof o, ctl, sizeof ctl, &lossy));
    TT_ASSERT(strcmp(o, "a?????b") == 0);
    TT_ASSERT_EQ(1, lossy);
    /* valid UTF-8 (2, 3, 4 byte sequences) passes through */
    const uint8_t ok[] = {0xC3, 0xA9, 0xE2, 0x82, 0xAC, 0xF0, 0x9F, 0x98, 0x80};
    lossy = 1;
    TT_ASSERT_EQ(9, wsc_html_escape(o, sizeof o, ok, sizeof ok, &lossy));
    TT_ASSERT(memcmp(o, ok, 9) == 0);
    TT_ASSERT_EQ(0, lossy);
    /* invalid: lone continuation, lone lead, overlong, surrogate, > U+10FFFF, 0xFF, truncated */
    const uint8_t bad1[] = {0x80};
    const uint8_t bad2[] = {0xC3};
    const uint8_t bad3[] = {0xC0, 0x80};
    const uint8_t bad4[] = {0xED, 0xA0, 0x80};
    const uint8_t bad5[] = {0xF4, 0x90, 0x80, 0x80};
    const uint8_t bad6[] = {0xFF};
    const uint8_t bad7[] = {0xE2, 0x82};
    const uint8_t bad8[] = {0xE0, 0x80, 0x80};
    const uint8_t *bads[] = {bad1, bad2, bad3, bad4, bad5, bad6, bad7, bad8};
    const size_t lens[] = {1, 1, 2, 3, 4, 1, 2, 3};
    for (size_t i = 0; i < 8; i++) {
        lossy = 0;
        int n = wsc_html_escape(o, sizeof o, bads[i], lens[i], &lossy);
        TT_ASSERT(n > 0);
        TT_ASSERT_EQ(1, lossy);
        for (int k = 0; k < n; k++) TT_ASSERT(o[k] == '?');
        TT_ASSERT_EQ(n, (int)strlen(o));
    }
    /* output never contains raw markup or high bytes for adversarial input */
    uint8_t all[256];
    for (int i = 0; i < 256; i++) all[i] = (uint8_t)i;
    char big[2048];
    int n = wsc_html_escape(big, sizeof big, all, sizeof all, &lossy);
    TT_ASSERT(n > 0);
    TT_ASSERT(strchr(big, '<') == NULL && strchr(big, '"') == NULL && strchr(big, '\'') == NULL);
}

static void test_escape_bounds(void)
{
    char o[16];
    /* exact fit: "&lt;" = 4 bytes + NUL needs 5 */
    TT_ASSERT_EQ(4, esc("<", o, 5, NULL));
    TT_ASSERT_EQ(-ENOSPC, esc("<", o, 4, NULL));
    TT_ASSERT(o[0] == '\0');
    TT_ASSERT_EQ(-ENOSPC, esc("a", o, 1, NULL));
    TT_ASSERT_EQ(-ENOSPC, esc("", o, 0, NULL));
    TT_ASSERT_EQ(0, esc("", o, 1, NULL));
    /* canary after the buffer is not touched */
    char buf[8];
    memset(buf, 'Z', sizeof buf);
    TT_ASSERT_EQ(-ENOSPC, esc("<<<", buf, 6, NULL));
    TT_ASSERT(buf[6] == 'Z' && buf[7] == 'Z');
}

static void test_prepare(void)
{
    wsc_rec a[6];
    a[0] = mk("weak", -80, 1, 1);
    a[1] = mk("", -30, 2, 0);
    a[2] = mk("strong", -40, 6, 1);
    a[3] = mk("weak", -60, 11, 1); /* duplicate, stronger: wins */
    a[4] = mk("open", -70, 3, 0);
    a[5] = mk("strong", -90, 4, 1); /* duplicate, weaker */
    size_t n = wsc_prepare(a, 6, WSC_LIST_MAX);
    TT_ASSERT_EQ(3, n);
    TT_ASSERT(memcmp(a[0].ssid, "strong", 6) == 0 && a[0].rssi == -40 && a[0].channel == 6);
    TT_ASSERT(memcmp(a[1].ssid, "weak", 4) == 0 && a[1].rssi == -60 && a[1].channel == 11);
    TT_ASSERT(memcmp(a[2].ssid, "open", 4) == 0 && a[2].secure == 0);

    TT_ASSERT_EQ(0, wsc_prepare(a, 0, WSC_LIST_MAX));
    wsc_rec e[2] = {mk("", -10, 1, 0), mk("", -20, 1, 0)};
    TT_ASSERT_EQ(0, wsc_prepare(e, 2, WSC_LIST_MAX));
    wsc_rec bad = mk("x", -10, 1, 0);
    bad.ssid_len = 33;
    TT_ASSERT_EQ(0, wsc_prepare(&bad, 1, WSC_LIST_MAX));
    wsc_rec one[1] = {mk("a", -10, 1, 0)};
    TT_ASSERT_EQ(0, wsc_prepare(one, 1, 0));

    /* same SSID bytes, different length are different SSIDs */
    wsc_rec d[2] = {mk("ab", -50, 1, 0), mk("abc", -51, 1, 0)};
    TT_ASSERT_EQ(2, wsc_prepare(d, 2, WSC_LIST_MAX));
    /* equal rssi: deterministic order independent of input order */
    wsc_rec t1[2] = {mk("b", -50, 1, 0), mk("a", -50, 1, 0)};
    wsc_rec t2[2] = {mk("a", -50, 1, 0), mk("b", -50, 1, 0)};
    wsc_prepare(t1, 2, WSC_LIST_MAX);
    wsc_prepare(t2, 2, WSC_LIST_MAX);
    TT_ASSERT(memcmp(&t1[0], &t2[0], sizeof t1[0]) == 0 && t1[0].ssid[0] == 'a');
}

static void test_prepare_cap(void)
{
    wsc_rec a[WSC_RAW_MAX];
    for (size_t i = 0; i < WSC_RAW_MAX; i++) {
        char s[8];
        snprintf(s, sizeof s, "n%02zu", i);
        a[i] = mk(s, -100 + (int)i, 1 + (int)(i % 13), (int)(i & 1));
    }
    size_t n = wsc_prepare(a, WSC_RAW_MAX, WSC_LIST_MAX);
    TT_ASSERT_EQ(WSC_LIST_MAX, n);
    for (size_t i = 1; i < n; i++) TT_ASSERT(a[i - 1].rssi >= a[i].rssi);
    TT_ASSERT_EQ(-61, a[0].rssi); /* strongest of 40 is -100 + 39 */
    /* dedupe happens before the cap: 40 copies of two names give 2 entries */
    for (size_t i = 0; i < WSC_RAW_MAX; i++) a[i] = mk(i & 1 ? "x" : "y", -50 - (int)i, 1, 0);
    TT_ASSERT_EQ(2, wsc_prepare(a, WSC_RAW_MAX, WSC_LIST_MAX));
}

static void test_render_entry(void)
{
    char o[WSC_ENTRY_MAX];
    wsc_rec r = mk("Home <b>&\"'", -55, 6, 1);
    int n = wsc_render_entry(o, sizeof o, &r);
    TT_ASSERT(n > 0 && (size_t)n == strlen(o));
    TT_ASSERT(strstr(o, "data-s=\"Home &lt;b&gt;&amp;&quot;&#39;\"") != NULL);
    TT_ASSERT(strstr(o, "-55 dBm") != NULL);
    TT_ASSERT(strstr(o, "ch 6") != NULL);
    TT_ASSERT(strstr(o, "secured") != NULL);
    TT_ASSERT(strstr(o, "<b>") == NULL);
    TT_ASSERT(strncmp(o, "<li>", 4) == 0 && strcmp(o + n - 6, "</li>\n") == 0);
    r = mk("cafe", -90, 13, 0);
    n = wsc_render_entry(o, sizeof o, &r);
    TT_ASSERT(n > 0 && strstr(o, "open") != NULL && strstr(o, "secured") == NULL);
    /* lossy SSID: not a link */
    r = mk("a", -40, 1, 1);
    r.ssid[0] = 0xFF;
    n = wsc_render_entry(o, sizeof o, &r);
    TT_ASSERT(n > 0 && strstr(o, "data-s") == NULL && strstr(o, "enter manually") != NULL);
}

static void test_render_worst_case(void)
{
    /* 32 bytes that all expand to the longest entity, worst-case numbers */
    wsc_rec r;
    memset(&r, 0, sizeof r);
    r.ssid_len = 32;
    memset(r.ssid, '\'', 32);
    r.rssi = -128;
    r.channel = 255;
    r.secure = 1;
    char o[WSC_ENTRY_MAX];
    int n = wsc_render_entry(o, sizeof o, &r);
    TT_ASSERT(n > 0 && (size_t)n < WSC_ENTRY_MAX);
    /* every smaller buffer reports -ENOSPC and leaves a valid empty string */
    for (size_t cap = 1; cap < (size_t)n + 1; cap++) {
        char small[WSC_ENTRY_MAX];
        memset(small, 'Q', sizeof small);
        TT_ASSERT_EQ(-ENOSPC, wsc_render_entry(small, cap, &r));
        TT_ASSERT(small[0] == '\0');
        TT_ASSERT(small[cap] == 'Q');
    }
    TT_ASSERT_EQ(n, wsc_render_entry(o, (size_t)n + 1, &r));
    TT_ASSERT_EQ(-ENOSPC, wsc_render_entry(o, 0, &r));
}

static void test_render_list(void)
{
    wsc_rec a[3] = {mk("a", -40, 1, 0), mk("b", -50, 2, 1), mk("c", -60, 3, 1)};
    char o[WSC_LIST_MAX * WSC_ENTRY_MAX];
    int n = wsc_render_list(o, sizeof o, a, 3);
    TT_ASSERT(n > 0 && (size_t)n == strlen(o));
    int items = 0;
    for (const char *p = o; (p = strstr(p, "<li>")) != NULL; p++) items++;
    TT_ASSERT_EQ(3, items);
    TT_ASSERT_EQ(0, wsc_render_list(o, sizeof o, a, 0));
    TT_ASSERT(o[0] == '\0');
    TT_ASSERT_EQ(-ENOSPC, wsc_render_list(o, (size_t)n, a, 3));
    TT_ASSERT(o[0] == '\0');
    TT_ASSERT_EQ(n, wsc_render_list(o, (size_t)n + 1, a, 3));
}

int main(void)
{
    TT_RUN(test_escape_specials);
    TT_RUN(test_escape_bytes);
    TT_RUN(test_escape_bounds);
    TT_RUN(test_prepare);
    TT_RUN(test_prepare_cap);
    TT_RUN(test_render_entry);
    TT_RUN(test_render_worst_case);
    TT_RUN(test_render_list);
    return TT_RESULT();
}
