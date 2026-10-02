#include "http_srv.h"

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "esp_log.h"
#include "esp_netif.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "lwip/sockets.h"

#include "ld2410_task.h"
#include "ld_settings.h"
#include "wifi_form.h"
#include "wifi_scan.h"
#include "wifi_mgr.h"

static const char *TAG = "http";
static httpd_handle_t s_server;
static void (*s_close_hook)(int fd);

httpd_handle_t http_srv_handle(void) { return s_server; }

void http_srv_set_close_hook(void (*hook)(int fd)) { s_close_hook = hook; }

/* Replaces the default session close: must close the socket itself. */
static void on_session_close(httpd_handle_t hd, int sockfd)
{
    (void)hd;
    if (s_close_hook) s_close_hook(sockfd);
    close(sockfd);
}

/* Same look as web_page.html (colour tokens, card, system font); inline, no external resources.
 * Sent once per response in the page head. */
#define WIFI_CSS                                                                              \
    "<style>"                                                                                 \
    ":root{--bg:#f4f5f7;--card:#fff;--fg:#1c1f24;--mut:#667;--bar:#2f6fed;--bd:#d8dbe0;"      \
    "--ok:#1f8f4e;--bad:#c0392b}"                                                             \
    "@media(prefers-color-scheme:dark){:root{--bg:#14161a;--card:#1e2126;--fg:#e8eaed;"       \
    "--mut:#98a0ab;--bar:#5b8def;--bd:#33373e;--ok:#4cc27e;--bad:#e8685a}}"                   \
    "*{box-sizing:border-box}"                                                                \
    "body{margin:0;padding:12px;background:var(--bg);color:var(--fg);"                        \
    "font:16px/1.4 system-ui,-apple-system,Segoe UI,Roboto,sans-serif}"                       \
    "main{max-width:640px;margin:0 auto}"                                                     \
    "h1{font-size:1.1rem;margin:0 0 10px}h2{font-size:.95rem;margin:0 0 8px}"                 \
    ".card{background:var(--card);border:1px solid var(--bd);border-radius:10px;"             \
    "padding:12px;margin-bottom:10px}"                                                        \
    ".k{font-size:.8rem;color:var(--mut)}.k a{color:var(--bar)}"                              \
    "ul{list-style:none;margin:0;padding:0}"                                                  \
    "li+li{border-top:1px solid var(--bd)}"                                                   \
    ".r{display:flex;flex-wrap:wrap;align-items:center;justify-content:space-between;"        \
    "gap:2px 12px;min-height:44px;padding:8px 4px;color:var(--fg);text-decoration:none}"      \
    "a.r:active{background:var(--bg)}"                                                        \
    ".nm{font-weight:600;overflow-wrap:anywhere}"                                             \
    ".mt{font-size:.8rem;color:var(--mut)}"                                                   \
    ".q{display:inline-block;width:16px;height:12px;margin-right:6px;"                        \
    "clip-path:polygon(0 100%,100% 100%,100% 0);background:var(--bd)}"                        \
    ".q1{background:linear-gradient(90deg,var(--bar) 25%,var(--bd) 25%)}"                     \
    ".q2{background:linear-gradient(90deg,var(--bar) 50%,var(--bd) 50%)}"                     \
    ".q3{background:linear-gradient(90deg,var(--bar) 75%,var(--bd) 75%)}"                     \
    ".q4{background:var(--bar)}"                                                              \
    "label{display:block;margin-bottom:12px;font-size:.9rem;color:var(--mut)}"                \
    "input{display:block;width:100%;margin-top:4px;padding:10px;font:inherit;"                \
    "color:var(--fg);background:var(--bg);border:1px solid var(--bd);border-radius:8px}"      \
    "input:focus{outline:2px solid var(--bar);outline-offset:-1px}"                           \
    "button{width:100%;padding:12px;font:inherit;font-weight:600;color:#fff;"                 \
    "background:var(--bar);border:0;border-radius:8px}"                                       \
    ".ok{color:var(--ok)}.bad{color:var(--bad)}"                                              \
    "</style>"

#define PAGE_OPEN_CSS(title, extra)                                                           \
    "<!doctype html><html><head><meta charset=\"utf-8\">"                                     \
    "<meta name=\"viewport\" content=\"width=device-width,initial-scale=1\">"                 \
    "<title>" title "</title>" WIFI_CSS extra "</head><body><main>"

#define WIFI_PAGE_OPEN(title) PAGE_OPEN_CSS(title, "")

static const char WIFI_HEAD[] =
    WIFI_PAGE_OPEN("AERIS-10 Lite Wi-Fi")
    "<h1>Wi-Fi setup</h1>"
    "<div class=\"card\"><h2>Nearby networks</h2>"
    "<div class=\"k\">Tap one to fill the SSID &middot; <a href=\"/wifi\">rescan</a></div>";

static const char WIFI_NO_SCAN[] =
    "<p class=\"k\">Scan unavailable, enter SSID manually.</p>";
static const char WIFI_NONE[] = "<p class=\"k\">No networks found. Enter SSID manually.</p>";

static const char WIFI_FORM[] =
    "</div><form class=\"card\" method=\"post\" action=\"/wifi\">"
    "<label>SSID<input id=\"ssid\" name=\"ssid\" maxlength=\"32\" required></label>"
    "<label>Password (empty = open network, otherwise 8-64 characters)"
    "<input name=\"password\" type=\"password\" maxlength=\"64\"></label>"
    "<button type=\"submit\">Save and reboot</button>"
    "</form><p class=\"k\"><a href=\"/ld2410\">LD2410C radar settings</a></p></main>"
    "<script>document.querySelectorAll('a[data-s]').forEach(function(a){"
    "a.onclick=function(){document.getElementById('ssid').value=a.dataset.s;return false}})"
    "</script></body></html>";

/* Small styled result page, sent in chunks (no stack buffer); cls and msg are literals without
 * markup. */
static esp_err_t wifi_result(httpd_req_t *req, const char *status, const char *cls, const char *msg)
{
    httpd_resp_set_status(req, status);
    httpd_resp_set_type(req, "text/html");
    if (httpd_resp_send_chunk(req, WIFI_PAGE_OPEN("AERIS-10 Lite Wi-Fi") "<div class=\"card\"><h1 class=\"",
                              HTTPD_RESP_USE_STRLEN) != ESP_OK ||
        httpd_resp_send_chunk(req, cls, HTTPD_RESP_USE_STRLEN) != ESP_OK ||
        httpd_resp_send_chunk(req, "\">", HTTPD_RESP_USE_STRLEN) != ESP_OK ||
        httpd_resp_send_chunk(req, msg, HTTPD_RESP_USE_STRLEN) != ESP_OK ||
        httpd_resp_send_chunk(req,
                              "</h1><div class=\"k\"><a href=\"/wifi\">Back to Wi-Fi setup</a></div>"
                              "</div></main></body></html>",
                              HTTPD_RESP_USE_STRLEN) != ESP_OK) {
        return ESP_FAIL;
    }
    return httpd_resp_send_chunk(req, NULL, 0);
}

/* One rendered entry at a time; only the single httpd task runs wifi_get, so a static buffer is
 * safe and keeps the 6 KB httpd stack free. */
static char s_entry[WSC_ENTRY_MAX];

#define SEND(str)                                                                   \
    do {                                                                            \
        if (httpd_resp_send_chunk(req, (str), HTTPD_RESP_USE_STRLEN) != ESP_OK) {   \
            free(recs);                                                             \
            return ESP_FAIL;                                                        \
        }                                                                           \
    } while (0)

bool http_srv_req_on_ap(httpd_req_t *req)
{
    esp_netif_t *ap = wifi_mgr_ap_netif();
    if (ap == NULL || !esp_netif_is_netif_up(ap)) return false;
    esp_netif_ip_info_t info;
    if (esp_netif_get_ip_info(ap, &info) != ESP_OK || info.ip.addr == 0) return false;

    int fd = httpd_req_to_sockfd(req);
    if (fd < 0) return false;
    struct sockaddr_storage ss;
    socklen_t sl = sizeof ss;
    memset(&ss, 0, sizeof ss);
    if (getsockname(fd, (struct sockaddr *)&ss, &sl) != 0) return false;

    uint32_t local = 0;
    if (ss.ss_family == AF_INET) {
        local = ((const struct sockaddr_in *)&ss)->sin_addr.s_addr;
    } else if (ss.ss_family == AF_INET6) {
        /* only IPv4-mapped (::ffff:a.b.c.d) addresses are understood */
        const uint8_t *a = ((const struct sockaddr_in6 *)&ss)->sin6_addr.s6_addr;
        static const uint8_t prefix[12] = {0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0xff, 0xff};
        if (memcmp(a, prefix, sizeof prefix) != 0) return false;
        memcpy(&local, a + 12, 4);
    } else {
        return false;
    }
    return local != 0 && local == info.ip.addr;
}

static esp_err_t wifi_get(httpd_req_t *req)
{
    if (!http_srv_req_on_ap(req)) return httpd_resp_send_err(req, HTTPD_404_NOT_FOUND, NULL);

    /* Scan before sending anything: blocks this task for about 2 s and briefly disrupts the AP. */
    wsc_rec *recs = malloc(sizeof *recs * WSC_RAW_MAX);
    size_t n = 0;
    esp_err_t serr = ESP_ERR_NO_MEM;
    if (recs != NULL) {
        serr = wifi_mgr_scan(recs, WSC_RAW_MAX, &n);
        if (serr != ESP_OK) {
            ESP_LOGW(TAG, "scan failed: %s", esp_err_to_name(serr));
            n = 0;
        }
        n = wsc_prepare(recs, n, WSC_LIST_MAX);
    }

    httpd_resp_set_type(req, "text/html");
    SEND(WIFI_HEAD);
    if (serr != ESP_OK) {
        SEND(WIFI_NO_SCAN);
    } else if (n == 0) {
        SEND(WIFI_NONE);
    } else {
        SEND("<ul>");
        for (size_t i = 0; i < n; i++) {
            if (wsc_render_entry(s_entry, sizeof s_entry, &recs[i]) > 0) SEND(s_entry);
        }
        SEND("</ul>");
    }
    SEND(WIFI_FORM);
    free(recs);
    return httpd_resp_send_chunk(req, NULL, 0);
}

static void restart_cb(void *arg)
{
    (void)arg;
    esp_restart();
}

#define WF_RECV_RETRIES 3 /* recv timeouts tolerated while reading the /wifi body */

static esp_err_t wifi_post(httpd_req_t *req)
{
    if (!http_srv_req_on_ap(req)) return httpd_resp_send_err(req, HTTPD_404_NOT_FOUND, NULL);
    if (req->content_len == 0 || req->content_len > WF_BODY_MAX) {
        return wifi_result(req, HTTPD_400, "bad", "Bad request length");
    }
    char body[WF_BODY_MAX];
    size_t got = 0;
    int timeouts = 0;
    while (got < req->content_len) {
        int r = httpd_req_recv(req, body + got, req->content_len - got);
        if (r == HTTPD_SOCK_ERR_TIMEOUT) {
            if (++timeouts > WF_RECV_RETRIES) {
                memset(body, 0, sizeof body);
                return httpd_resp_send_err(req, HTTPD_408_REQ_TIMEOUT, NULL);
            }
            continue;
        }
        if (r <= 0) return ESP_FAIL;
        got += (size_t)r;
    }
    char ssid[WF_SSID_MAX + 1], pass[WF_PASS_MAX + 1];
    int rc = wf_parse_credentials(body, got, ssid, pass);
    memset(body, 0, sizeof body);
    if (rc != WF_OK) {
        memset(pass, 0, sizeof pass);
        return wifi_result(req, HTTPD_400, "bad", "Invalid SSID or password");
    }
    esp_err_t err = wifi_mgr_save_credentials(ssid, pass);
    memset(pass, 0, sizeof pass);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "saving credentials failed: %s", esp_err_to_name(err));
        return wifi_result(req, HTTPD_500, "bad", "Saving failed");
    }
    wifi_result(req, HTTPD_200, "ok", "Saved, rebooting");

    const esp_timer_create_args_t targs = {.callback = restart_cb, .name = "wifi_restart"};
    esp_timer_handle_t t;
    if (esp_timer_create(&targs, &t) == ESP_OK) esp_timer_start_once(t, 1000000);
    return ESP_OK;
}

/* ---- /ld2410: LD2410C settings (AP only) ---- */

/* Extra rules for the sensitivity table and the small action buttons. */
#define LD_CSS                                                                                \
    "<style>"                                                                                 \
    "table{width:100%;border-collapse:collapse;font-size:.85rem}"                             \
    "th,td{text-align:left;padding:4px 2px;border-top:1px solid var(--bd)}"                   \
    "th{color:var(--mut);font-weight:600;border-top:0}"                                       \
    "td input{width:4.5em;margin:0;padding:6px}"                                              \
    ".mt{color:var(--mut)}"                                                                   \
    "button.sm{width:auto;margin:0 6px 6px 0;padding:10px 14px;color:var(--fg);"              \
    "background:var(--bg);border:1px solid var(--bd)}"                                        \
    "button.dg{color:var(--bad)}"                                                             \
    "form.in{display:inline}"                                                                 \
    "</style>"

#define LD_PAGE_OPEN PAGE_OPEN_CSS("AERIS-10 Lite LD2410C", LD_CSS)

#define LD_REQ_TIMEOUT_MS 5000
#define LD_RECV_RETRIES 3 /* recv timeouts tolerated while reading the /ld2410 body */

#define LD_LINKS_INNER "<a href=\"/ld2410\">refresh</a> &middot; <a href=\"/wifi\">Wi-Fi setup</a>"
#define LD_LINKS "<div class=\"k\">" LD_LINKS_INNER "</div>"

/* Table 7 (p.15) of Protocolo_comunicacion_serial_LD2410C.pdf; -1 = not settable. */
#define LD_DEF_MAX_GATE 8
#define LD_DEF_DURATION 5
static const int8_t LD_DEF_MOVE[LD_GATE_COUNT] = {50, 50, 40, 30, 20, 15, 15, 15, 15};
static const int8_t LD_DEF_STILL[LD_GATE_COUNT] = {-1, -1, 40, 40, 30, 30, 20, 20, 20};

/* Bounded formatted chunk (small stack buffer; every format below takes numbers and internal
 * literals only, never request data). Truncation counts as failure. */
static esp_err_t send_fmt(httpd_req_t *req, const char *fmt, ...) __attribute__((format(printf, 2, 3)));
static esp_err_t send_fmt(httpd_req_t *req, const char *fmt, ...)
{
    char buf[320];
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(buf, sizeof buf, fmt, ap);
    va_end(ap);
    if (n < 0 || (size_t)n >= sizeof buf) return ESP_FAIL;
    return httpd_resp_send_chunk(req, buf, (ssize_t)n);
}

#define LSEND(str)                                                                  \
    do {                                                                            \
        if (httpd_resp_send_chunk(req, (str), HTTPD_RESP_USE_STRLEN) != ESP_OK) {   \
            return ESP_FAIL;                                                        \
        }                                                                           \
    } while (0)

#define LSENDF(...)                                                                 \
    do {                                                                            \
        if (send_fmt(req, __VA_ARGS__) != ESP_OK) return ESP_FAIL;                  \
    } while (0)

/* Message for a request that did not complete OK. Returns a literal or fills buf. */
static const char *ld_err_text(esp_err_t err, const ld_req_t *r, char *buf, size_t cap)
{
    if (err == ESP_ERR_TIMEOUT) return "The LD2410C did not answer in time";
    if (err == ESP_ERR_INVALID_STATE) return "The LD2410C is busy or not ready, try again";
    if (err != ESP_OK) return "Internal error";
    switch (r->result) {
    case LD_RES_CMD_FAILED:
        snprintf(buf, cap, "Command failed: %s", r->failed_cmd);
        return buf;
    case LD_RES_BAD_ARG: return "Values out of range, nothing was sent";
    case LD_RES_VERIFY: return "The module did not keep the values (read-back differs)";
    default: return "Unknown error";
    }
}

static const char *ld_parse_text(int rc)
{
    switch (rc) {
    case LDS_E_MISSING: return "A form field is missing";
    case LDS_E_DUPLICATE: return "A form field was sent twice";
    case LDS_E_BAD_VALUE: return "A value is not a plain number";
    case LDS_E_TOO_LONG: return "A value is too long";
    case LDS_E_RANGE: return "A value is out of range";
    default: return "Invalid form";
    }
}

/* Result page head: status line, <h1 class=cls>msg</h1>. cls and msg contain no user data. */
static esp_err_t ld_result_open(httpd_req_t *req, const char *status, const char *cls, const char *msg)
{
    httpd_resp_set_status(req, status);
    httpd_resp_set_type(req, "text/html");
    if (httpd_resp_send_chunk(req, LD_PAGE_OPEN "<div class=\"card\"><h1 class=\"",
                              HTTPD_RESP_USE_STRLEN) != ESP_OK ||
        httpd_resp_send_chunk(req, cls, HTTPD_RESP_USE_STRLEN) != ESP_OK ||
        httpd_resp_send_chunk(req, "\">", HTTPD_RESP_USE_STRLEN) != ESP_OK ||
        httpd_resp_send_chunk(req, msg, HTTPD_RESP_USE_STRLEN) != ESP_OK ||
        httpd_resp_send_chunk(req, "</h1>", HTTPD_RESP_USE_STRLEN) != ESP_OK) {
        return ESP_FAIL;
    }
    return ESP_OK;
}

static esp_err_t ld_result_close(httpd_req_t *req)
{
    if (httpd_resp_send_chunk(req, LD_LINKS "</div></main></body></html>", HTTPD_RESP_USE_STRLEN) !=
        ESP_OK) {
        return ESP_FAIL;
    }
    return httpd_resp_send_chunk(req, NULL, 0);
}

static esp_err_t ld_result(httpd_req_t *req, const char *status, const char *cls, const char *msg)
{
    if (ld_result_open(req, status, cls, msg) != ESP_OK) return ESP_FAIL;
    return ld_result_close(req);
}

/* Distance range of gate g: 0.75 m per gate, in whole centimetres (no floating point). */
static void ld_range(unsigned g, unsigned *a_m, unsigned *a_cm, unsigned *b_m, unsigned *b_cm)
{
    unsigned a = g * 75u, b = (g + 1u) * 75u;
    *a_m = a / 100u;
    *a_cm = a % 100u;
    *b_m = b / 100u;
    *b_cm = b % 100u;
}

static esp_err_t ld2410_get(httpd_req_t *req)
{
    if (!http_srv_req_on_ap(req)) return httpd_resp_send_err(req, HTTPD_404_NOT_FOUND, NULL);

    ld_req_t rq;
    memset(&rq, 0, sizeof rq);
    rq.kind = LD_REQ_READ;
    esp_err_t err = ld2410_request(&rq, LD_REQ_TIMEOUT_MS);
    if (err != ESP_OK || rq.result != LD_RES_OK || !rq.have_before) {
        char msg[LD_REQ_NAME_MAX + 24];
        const char *txt = (err == ESP_OK && rq.result == LD_RES_OK)
                              ? "No parameters received"
                              : ld_err_text(err, &rq, msg, sizeof msg);
        return ld_result(req, HTTPD_200, "bad", txt);
    }
    const ld_settings_t *s = &rq.before;

    httpd_resp_set_type(req, "text/html");
    LSEND(LD_PAGE_OPEN "<h1>LD2410C radar settings</h1>");
    if (rq.have_version) {
        char ver[LD_VERSION_STR_MAX];
        if (ld_version_format(&rq.version, ver, sizeof ver) < 0) snprintf(ver, sizeof ver, "?");
        LSENDF("<div class=\"card\"><div class=\"k\">Firmware %s &middot; " LD_LINKS_INNER "</div></div>", ver);
    } else {
        LSEND("<div class=\"card\"><div class=\"k\">Firmware unknown &middot; " LD_LINKS_INNER "</div></div>");
    }
    LSEND("<form class=\"card\" method=\"post\" action=\"/ld2410\">");
    LSENDF("<label>Max moving gate (2-8)<input type=\"number\" name=\"mg\" min=\"2\" max=\"8\" "
           "value=\"%u\" required></label>",
           (unsigned)s->max_move_gate);
    LSENDF("<label>Max still gate (2-8)<input type=\"number\" name=\"sg\" min=\"2\" max=\"8\" "
           "value=\"%u\" required></label>",
           (unsigned)s->max_still_gate);
    LSENDF("<label>No-one duration, seconds (0-65535)<input type=\"number\" name=\"dur\" min=\"0\" "
           "max=\"65535\" value=\"%u\" required></label>",
           (unsigned)s->duration_s);
    LSEND("<table><tr><th>Gate</th><th>Range</th><th>Moving</th><th>Still</th>"
          "<th>Default m/s</th></tr>");
    for (unsigned g = 0; g < LD_GATE_COUNT; g++) {
        unsigned am, ac, bm, bc;
        ld_range(g, &am, &ac, &bm, &bc);
        LSENDF("<tr><td>%u</td><td>%u.%02u-%u.%02u m</td>"
               "<td><input type=\"number\" name=\"m%u\" min=\"0\" max=\"100\" value=\"%u\" required></td>",
               g, am, ac, bm, bc, g, (unsigned)s->move_sens[g]);
        if (g < 2) {
            LSENDF("<td class=\"mt\">%u (fixed)</td>", (unsigned)s->still_sens[g]);
        } else {
            LSENDF("<td><input type=\"number\" name=\"s%u\" min=\"0\" max=\"100\" value=\"%u\" required></td>",
                   g, (unsigned)s->still_sens[g]);
        }
        if (LD_DEF_STILL[g] < 0) {
            LSENDF("<td class=\"mt\">%d / -</td></tr>", (int)LD_DEF_MOVE[g]);
        } else {
            LSENDF("<td class=\"mt\">%d / %d</td></tr>", (int)LD_DEF_MOVE[g], (int)LD_DEF_STILL[g]);
        }
    }
    LSENDF("</table><p class=\"k\">Sensitivity 0-100 (100 = gate ignored). Factory defaults: max gates "
           "%d/%d, no-one duration %d s. The still sensitivity of gates 0 and 1 cannot be changed.</p>",
           LD_DEF_MAX_GATE, LD_DEF_MAX_GATE, LD_DEF_DURATION);
    LSEND("<button type=\"submit\">Save</button>"
          "<p class=\"k\">Changes are stored in the LD2410C itself. Data frames pause briefly while "
          "saving.</p></form>");
    LSEND("<div class=\"card\"><h2>Module actions</h2>"
          "<form class=\"in\" method=\"post\" action=\"/ld2410\">"
          "<input type=\"hidden\" name=\"action\" value=\"bt_off\">"
          "<button class=\"sm\" type=\"submit\">Bluetooth off</button></form>"
          "<form class=\"in\" method=\"post\" action=\"/ld2410\">"
          "<input type=\"hidden\" name=\"action\" value=\"restart\">"
          "<button class=\"sm\" type=\"submit\" "
          "onclick=\"return confirm('Restart the LD2410C?')\">Restart module</button></form>"
          "<form class=\"in\" method=\"post\" action=\"/ld2410\">"
          "<input type=\"hidden\" name=\"action\" value=\"factory\">"
          "<button class=\"sm dg\" type=\"submit\" "
          "onclick=\"return confirm('Reset the LD2410C to factory settings?')\">Factory reset</button>"
          "</form></div></main></body></html>");
    return httpd_resp_send_chunk(req, NULL, 0);
}

/* Look for an `action=<name>` field. Returns -1 if absent, the request kind otherwise, or -2 for
 * an unknown action value. */
static int ld_find_action(const char *body, size_t len)
{
    size_t i = 0;
    while (i < len) {
        size_t j = i;
        while (j < len && body[j] != '&') j++;
        if (j - i > 7 && memcmp(body + i, "action=", 7) == 0) {
            const char *v = body + i + 7;
            size_t vl = j - i - 7;
            if (vl == 6 && memcmp(v, "bt_off", 6) == 0) return LD_REQ_BT_OFF;
            if (vl == 7 && memcmp(v, "restart", 7) == 0) return LD_REQ_RESTART;
            if (vl == 7 && memcmp(v, "factory", 7) == 0) return LD_REQ_FACTORY;
            return -2;
        }
        i = j + 1;
    }
    return -1;
}

static esp_err_t ld2410_post(httpd_req_t *req)
{
    if (!http_srv_req_on_ap(req)) return httpd_resp_send_err(req, HTTPD_404_NOT_FOUND, NULL);
    if (req->content_len == 0 || req->content_len > LDS_BODY_MAX) {
        return ld_result(req, HTTPD_400, "bad", "Bad request length");
    }
    char body[LDS_BODY_MAX];
    size_t got = 0;
    int timeouts = 0;
    while (got < req->content_len) {
        int r = httpd_req_recv(req, body + got, req->content_len - got);
        if (r == HTTPD_SOCK_ERR_TIMEOUT) {
            if (++timeouts > LD_RECV_RETRIES) {
                return httpd_resp_send_err(req, HTTPD_408_REQ_TIMEOUT, NULL);
            }
            continue;
        }
        if (r <= 0) return ESP_FAIL;
        got += (size_t)r;
    }

    ld_req_t rq;
    char msg[LD_REQ_NAME_MAX + 24];
    memset(&rq, 0, sizeof rq);
    int act = ld_find_action(body, got);
    if (act == -2) return ld_result(req, HTTPD_400, "bad", "Unknown action");
    if (act >= 0) {
        rq.kind = (ld_req_kind_t)act;
        esp_err_t err = ld2410_request(&rq, LD_REQ_TIMEOUT_MS);
        if (err != ESP_OK || rq.result != LD_RES_OK) {
            return ld_result(req, HTTPD_500, "bad", ld_err_text(err, &rq, msg, sizeof msg));
        }
        return ld_result(req, HTTPD_200, "ok",
                         act == LD_REQ_BT_OFF   ? "Bluetooth switched off, module restarting"
                         : act == LD_REQ_FACTORY ? "Factory reset done, module restarting"
                                                 : "Module restarting");
    }

    /* Save: current values first (for the read-only still sensitivities and the diff). */
    rq.kind = LD_REQ_READ;
    esp_err_t err = ld2410_request(&rq, LD_REQ_TIMEOUT_MS);
    if (err != ESP_OK || rq.result != LD_RES_OK || !rq.have_before) {
        const char *txt = (err == ESP_OK && rq.result == LD_RES_OK)
                              ? "No parameters received"
                              : ld_err_text(err, &rq, msg, sizeof msg);
        return ld_result(req, HTTPD_500, "bad", txt);
    }
    ld_settings_t target;
    int rc = ld_settings_parse_form(body, got, &rq.before, &target);
    if (rc != LDS_OK) return ld_result(req, HTTPD_400, "bad", ld_parse_text(rc));

    memset(&rq, 0, sizeof rq);
    rq.kind = LD_REQ_WRITE;
    rq.target = target;
    err = ld2410_request(&rq, LD_REQ_TIMEOUT_MS);
    if (err != ESP_OK || rq.result != LD_RES_OK) {
        return ld_result(req, HTTPD_500, "bad", ld_err_text(err, &rq, msg, sizeof msg));
    }
    if (ld_result_open(req, HTTPD_200, "ok", "Saved") != ESP_OK) return ESP_FAIL;
    const ld_settings_t *a = rq.have_after ? &rq.after : &target;
    LSENDF("<div class=\"k\">Now in the module: max moving gate %u, max still gate %u, no-one "
           "duration %u s</div><p class=\"mt\">",
           (unsigned)a->max_move_gate, (unsigned)a->max_still_gate, (unsigned)a->duration_s);
    for (unsigned g = 0; g < LD_GATE_COUNT; g++) {
        LSENDF("Gate %u: moving %u, still %u<br>", g, (unsigned)a->move_sens[g],
               (unsigned)a->still_sens[g]);
    }
    LSEND("</p>");
    return ld_result_close(req);
}

esp_err_t http_srv_register(const httpd_uri_t *uri)
{
    if (s_server == NULL) return ESP_ERR_INVALID_STATE;
    return httpd_register_uri_handler(s_server, uri);
}

esp_err_t http_srv_start(void)
{
    if (s_server != NULL) return ESP_ERR_INVALID_STATE;
    httpd_config_t cfg = HTTPD_DEFAULT_CONFIG();
    /* /wifi x2, /ld2410 x2, live page and /ws: 6 of 8 */
    cfg.max_uri_handlers = 8;
    /* httpd needs max_open_sockets <= CONFIG_LWIP_MAX_SOCKETS - 3 (16 in sdkconfig.defaults) */
    cfg.max_open_sockets = 7;
    cfg.close_fn = on_session_close;
    cfg.stack_size = 8192; /* /ld2410 POST: 512-byte body + request structs + vsnprintf */
    cfg.lru_purge_enable = true;
    esp_err_t err = httpd_start(&s_server, &cfg);
    if (err != ESP_OK) return err;

    static const httpd_uri_t get_wifi = {.uri = "/wifi", .method = HTTP_GET, .handler = wifi_get};
    static const httpd_uri_t post_wifi = {.uri = "/wifi", .method = HTTP_POST, .handler = wifi_post};
    static const httpd_uri_t get_ld = {.uri = "/ld2410", .method = HTTP_GET, .handler = ld2410_get};
    static const httpd_uri_t post_ld = {.uri = "/ld2410", .method = HTTP_POST, .handler = ld2410_post};
    err = http_srv_register(&get_wifi);
    if (err == ESP_OK) err = http_srv_register(&post_wifi);
    if (err == ESP_OK) err = http_srv_register(&get_ld);
    if (err == ESP_OK) err = http_srv_register(&post_ld);
    return err;
}
