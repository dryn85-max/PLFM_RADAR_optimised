#include "web_log.h"

#include <stdarg.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "esp_http_server.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

#include "http_srv.h"
#include "log_ring.h"

/* Hook line buffer. The hook runs on the stack of whichever task logs, including 3-4 KB ones, so
 * the buffer stays small: ESP_LOG lines are normally < 120 bytes, and the extra frame (256 B +
 * va_list copy) is small next to the vprintf call it wraps. Longer lines are cut and end "...\n". */
#define LINE_MAX_BYTES 256u
/* Body size per /log/data response; the client polls again at once when it gets a full one. */
#define DATA_MAX_BYTES 4096u

/* Ring storage: a static array in internal RAM (BSS). No allocation is needed, the hook works from
 * the very first log line, and internal RAM is what the critical section can touch safely. */
static uint8_t s_mem[WEB_LOG_BYTES];
static log_ring_t s_ring;
static portMUX_TYPE s_mux = portMUX_INITIALIZER_UNLOCKED;
static vprintf_like_t s_prev;
static bool s_installed;

/* /log/data copies the ring (under s_mux) into this buffer, then sends it. httpd runs handlers in
 * one task; the mutex keeps that assumption from mattering and keeps 4 KB off the httpd stack. */
static char s_data[DATA_MAX_BYTES];
static SemaphoreHandle_t s_data_lock;

extern const char web_log_html_start[] asm("_binary_web_log_html_start");

/* No ESP_LOGx and no allocation anywhere in this function (it would recurse into itself). */
static int web_log_vprintf(const char *fmt, va_list ap)
{
    va_list ap2;
    va_copy(ap2, ap); /* a va_list is consumed by one use: one copy for the console, ap for us */
    int ret = s_prev ? s_prev(fmt, ap2) : vprintf(fmt, ap2);
    va_end(ap2);

    char line[LINE_MAX_BYTES];
    int n = vsnprintf(line, sizeof line, fmt, ap);
    if (n < 0) return ret;
    size_t len = (size_t)n;
    if (len >= sizeof line) { /* truncated: keep the first 251 bytes, then "...\n" */
        memcpy(line + sizeof line - 5, "...\n", 4);
        len = sizeof line - 1;
    }
    len = log_line_clean(line, len, line, sizeof line);
    if (len == 0 || log_line_is_secret(line, len)) return ret;

    portENTER_CRITICAL(&s_mux);
    log_ring_write(&s_ring, line, len);
    portEXIT_CRITICAL(&s_mux);
    return ret;
}

esp_err_t web_log_init(void)
{
    if (s_installed) return ESP_OK;
    s_data_lock = xSemaphoreCreateMutex();
    if (s_data_lock == NULL) return ESP_ERR_NO_MEM;
    log_ring_init(&s_ring, s_mem, sizeof s_mem);
    s_prev = esp_log_set_vprintf(web_log_vprintf);
    s_installed = true;
    return ESP_OK;
}

/* Strict decimal u32: 1-10 digits, no sign/space/suffix, value <= UINT32_MAX. */
static bool parse_u32(const char *s, uint32_t *out)
{
    size_t n = strlen(s);
    if (n == 0 || n > 10) return false;
    uint64_t v = 0;
    for (size_t i = 0; i < n; i++) {
        if (s[i] < '0' || s[i] > '9') return false;
        v = v * 10u + (uint64_t)(s[i] - '0');
    }
    if (v > UINT32_MAX) return false;
    *out = (uint32_t)v;
    return true;
}

static esp_err_t log_page_get(httpd_req_t *req)
{
    httpd_resp_set_type(req, "text/html; charset=utf-8");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    return httpd_resp_send(req, web_log_html_start, HTTPD_RESP_USE_STRLEN);
}

static esp_err_t log_data_get(httpd_req_t *req)
{
    uint32_t from = 0;
    bool have_from = false;
    char query[48];
    char val[16];
    if (httpd_req_get_url_query_str(req, query, sizeof query) == ESP_OK &&
        httpd_query_key_value(query, "from", val, sizeof val) == ESP_OK) {
        have_from = parse_u32(val, &from); /* invalid or oversize: as if absent */
    }

    if (xSemaphoreTake(s_data_lock, pdMS_TO_TICKS(1000)) != pdTRUE) {
        return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "busy");
    }
    uint32_t next = 0;
    bool gap = false;
    size_t n;
    portENTER_CRITICAL(&s_mux);
    if (!have_from) from = s_ring.head - s_ring.used; /* whole buffer: the oldest held byte */
    n = log_ring_read(&s_ring, from, s_data, sizeof s_data, &next, &gap);
    portEXIT_CRITICAL(&s_mux);

    /* httpd_resp_set_hdr keeps the pointers until the send below, so these locals are enough. */
    char h_next[16];
    snprintf(h_next, sizeof h_next, "%lu", (unsigned long)next);
    httpd_resp_set_type(req, "text/plain; charset=utf-8");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    httpd_resp_set_hdr(req, "X-Log-Next", h_next);
    httpd_resp_set_hdr(req, "X-Log-Gap", (have_from && gap) ? "1" : "0");
    esp_err_t err = httpd_resp_send(req, s_data, (ssize_t)n);
    xSemaphoreGive(s_data_lock);
    return err;
}

esp_err_t web_log_register(void)
{
    static const httpd_uri_t get_page = {.uri = "/log", .method = HTTP_GET, .handler = log_page_get};
    static const httpd_uri_t get_data = {.uri = "/log/data", .method = HTTP_GET, .handler = log_data_get};
    esp_err_t err = http_srv_register(&get_page);
    if (err == ESP_OK) err = http_srv_register(&get_data);
    return err;
}
