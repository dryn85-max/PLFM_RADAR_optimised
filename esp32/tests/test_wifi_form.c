/* Host tests for wifi_form: AP password generation and form decoding. */
#include <stdio.h>
#include <string.h>
#include "tinytest.h"
#include "wifi_form.h"

static uint32_t seq_val;
static uint32_t rnd_seq(void) { return seq_val++ * 0x9E3779B1u; }
static uint32_t rnd_zero(void) { return 0; }
static uint32_t rnd_ones(void) { return 0xFFFFFFFFu; }

static void test_ap_pass(void)
{
    char p[WF_AP_PASS_LEN + 1];
    memset(p, 'x', sizeof p);
    wf_gen_ap_pass(rnd_seq, p);
    TT_ASSERT_EQ(WF_AP_PASS_LEN, strlen(p));
    for (int round = 0; round < 500; round++) {
        wf_gen_ap_pass(rnd_seq, p);
        for (size_t i = 0; i < WF_AP_PASS_LEN; i++) {
            TT_ASSERT(strchr(WF_AP_ALPHABET, p[i]) != NULL);
            TT_ASSERT(strchr("0O1lI", p[i]) == NULL);
        }
    }
    /* extremes of the random source stay inside the alphabet */
    wf_gen_ap_pass(rnd_zero, p);
    TT_ASSERT_EQ(WF_AP_PASS_LEN, strlen(p));
    TT_ASSERT(strchr(WF_AP_ALPHABET, p[0]) != NULL);
    wf_gen_ap_pass(rnd_ones, p);
    for (size_t i = 0; i < WF_AP_PASS_LEN; i++) TT_ASSERT(strchr(WF_AP_ALPHABET, p[i]) != NULL);
    /* every alphabet character is reachable */
    int seen[256] = {0};
    for (int round = 0; round < 500; round++) {
        wf_gen_ap_pass(rnd_seq, p);
        for (size_t i = 0; i < WF_AP_PASS_LEN; i++) seen[(unsigned char)p[i]] = 1;
    }
    for (const char *a = WF_AP_ALPHABET; *a; a++) TT_ASSERT(seen[(unsigned char)*a]);
    TT_ASSERT_EQ(32, strlen(WF_AP_ALPHABET));
}

static int parse(const char *body, char *ssid, char *pass)
{
    return wf_parse_credentials(body, strlen(body), ssid, pass);
}

static void test_decode_basic(void)
{
    char s[WF_SSID_MAX + 1], p[WF_PASS_MAX + 1];
    TT_ASSERT_EQ(0, parse("ssid=Home&password=secret123", s, p));
    TT_ASSERT(strcmp(s, "Home") == 0);
    TT_ASSERT(strcmp(p, "secret123") == 0);
    TT_ASSERT_EQ(0, parse("password=pass+word%21&ssid=My+Net%20%26", s, p));
    TT_ASSERT(strcmp(s, "My Net &") == 0);
    TT_ASSERT(strcmp(p, "pass word!") == 0);
    TT_ASSERT_EQ(0, parse("ssid=a%2fb%2Fc&password=", s, p)); /* empty = open network */
    TT_ASSERT(strcmp(s, "a/b/c") == 0);
    TT_ASSERT(strcmp(p, "") == 0);
    TT_ASSERT_EQ(0, parse("x=1&ssid=n&y&password=12345678&z=%zz", s, p)); /* unknown keys ignored */
    TT_ASSERT(strcmp(s, "n") == 0);
    TT_ASSERT_EQ(0, parse("ssid=%C3%A4&password=12345678", s, p)); /* UTF-8 bytes pass through */
    TT_ASSERT_EQ(2, strlen(s));
}

static void test_decode_errors(void)
{
    char s[WF_SSID_MAX + 1], p[WF_PASS_MAX + 1];
    TT_ASSERT_EQ(WF_E_MISSING, parse("", s, p));
    TT_ASSERT_EQ(WF_E_MISSING, parse("ssid=a", s, p));
    TT_ASSERT_EQ(WF_E_MISSING, parse("password=12345678", s, p));
    TT_ASSERT_EQ(WF_E_MISSING, parse("ssid=&password=12345678", s, p)); /* empty ssid */
    TT_ASSERT_EQ(WF_E_MISSING, parse("ssidx=a&password=12345678", s, p));
    TT_ASSERT_EQ(WF_E_BAD_ESCAPE, parse("ssid=a%&password=12345678", s, p));
    TT_ASSERT_EQ(WF_E_BAD_ESCAPE, parse("ssid=a%4&password=12345678", s, p));
    TT_ASSERT_EQ(WF_E_BAD_ESCAPE, parse("ssid=a%4g&password=12345678", s, p));
    TT_ASSERT_EQ(WF_E_BAD_ESCAPE, parse("ssid=a%00b&password=12345678", s, p)); /* NUL */
    TT_ASSERT_EQ(WF_E_BAD_ESCAPE, parse("ssid=a&password=1234%5", s, p));
    TT_ASSERT_EQ(WF_E_DUPLICATE, parse("ssid=a&ssid=b&password=12345678", s, p));
    TT_ASSERT_EQ(WF_E_DUPLICATE, parse("ssid=a&password=12345678&password=x", s, p));
    TT_ASSERT_EQ(WF_E_BAD_PASSWORD, parse("ssid=a&password=short", s, p)); /* 1..7 chars */
    TT_ASSERT_EQ(WF_E_BAD_PASSWORD, parse("ssid=a&password=1234567", s, p));
    TT_ASSERT_EQ(WF_E_MISSING, wf_parse_credentials(NULL, 0, s, p));
}

static void test_limits(void)
{
    char s[WF_SSID_MAX + 1], p[WF_PASS_MAX + 1];
    char body[512];
    char a32[33], a33[34], b64[65], b65[66];
    memset(a32, 'a', 32); a32[32] = 0;
    memset(a33, 'a', 33); a33[33] = 0;
    memset(b64, 'b', 64); b64[64] = 0;
    memset(b65, 'b', 65); b65[65] = 0;
    snprintf(body, sizeof body, "ssid=%s&password=%s", a32, b64);
    TT_ASSERT_EQ(0, parse(body, s, p));
    TT_ASSERT_EQ(32, strlen(s));
    TT_ASSERT_EQ(64, strlen(p));
    snprintf(body, sizeof body, "ssid=%s&password=%s", a33, b64);
    TT_ASSERT_EQ(WF_E_TOO_LONG, parse(body, s, p));
    snprintf(body, sizeof body, "ssid=%s&password=%s", a32, b65);
    TT_ASSERT_EQ(WF_E_TOO_LONG, parse(body, s, p));
    /* the limit applies to decoded bytes, not to the encoded text */
    snprintf(body, sizeof body, "ssid=%%61%%61%%61%%61&password=%%62%%62%%62%%62%%62%%62%%62%%62");
    TT_ASSERT_EQ(0, parse(body, s, p));
    TT_ASSERT(strcmp(s, "aaaa") == 0);
    /* 33 escaped bytes are still too long */
    char esc[200] = "ssid=";
    for (int i = 0; i < 33; i++) strcat(esc, "%41");
    strcat(esc, "&password=12345678");
    TT_ASSERT_EQ(WF_E_TOO_LONG, parse(esc, s, p));
    /* oversized body */
    static char big[WF_BODY_MAX + 100];
    memset(big, 'x', sizeof big);
    TT_ASSERT_EQ(WF_E_TOO_LONG, wf_parse_credentials(big, sizeof big, s, p));
    /* length-delimited, not NUL-terminated */
    TT_ASSERT_EQ(0, wf_parse_credentials("ssid=ab&password=12345678JUNK", 25, s, p));
    TT_ASSERT(strcmp(p, "12345678") == 0);
    TT_ASSERT_EQ(WF_E_BAD_ESCAPE, wf_parse_credentials("ssid=a&password=12345678%41", 26, s, p));
}

int main(void)
{
    TT_RUN(test_ap_pass);
    TT_RUN(test_decode_basic);
    TT_RUN(test_decode_errors);
    TT_RUN(test_limits);
    return TT_RESULT();
}
