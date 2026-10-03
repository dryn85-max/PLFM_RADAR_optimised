#include "ota_http.h"

#include <stdatomic.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "esp_app_desc.h"
#include "esp_log.h"
#include "esp_ota_ops.h"
#include "esp_partition.h"
#include "esp_system.h"
#include "esp_timer.h"

#include "http_srv.h"

#define OTA_CHUNK 4096        /* heap buffer for the upload body */
#define OTA_RECV_RETRIES 5    /* consecutive recv timeouts tolerated (httpd recv timeout: 5 s) */
#define OTA_RESTART_US 1000000

static const char *TAG = "ota_http";

extern const char ota_page_html_start[] asm("_binary_ota_page_html_start");

/* One update at a time. Never cleared after a successful upload or rollback: the device is about
 * to reboot and must not accept another request in between. httpd runs handlers in one task, so
 * a second request cannot really overlap; the flag keeps that assumption from mattering. */
static atomic_flag s_busy = ATOMIC_FLAG_INIT;

static void restart_cb(void *arg)
{
    (void)arg;
    esp_restart();
}

/* Call after the response has been sent. */
static void schedule_restart(void)
{
    const esp_timer_create_args_t targs = {.callback = restart_cb, .name = "ota_restart"};
    esp_timer_handle_t t;
    if (esp_timer_create(&targs, &t) == ESP_OK && esp_timer_start_once(t, OTA_RESTART_US) == ESP_OK) {
        return;
    }
    ESP_LOGE(TAG, "restart timer failed, restarting now");
    esp_restart();
}

/* Plain-text reply (curl prints it). "Connection: close" so an unread request body can never be
 * parsed as the next request. */
static esp_err_t reply(httpd_req_t *req, const char *status, const char *msg)
{
    httpd_resp_set_status(req, status);
    httpd_resp_set_type(req, "text/plain");
    httpd_resp_set_hdr(req, "Connection", "close");
    return httpd_resp_sendstr(req, msg);
}

/* Reply, then return ESP_FAIL so httpd closes the socket (use when body bytes are left unread). */
static esp_err_t reply_close(httpd_req_t *req, const char *status, const char *msg)
{
    reply(req, status, msg);
    return ESP_FAIL;
}

/* ---- slot description ---- */

typedef struct {
    char slot[20];        /* partition label */
    char ver[33];         /* app version, "" if no readable image */
    bool has_image;       /* esp_ota_get_partition_description succeeded */
    esp_ota_img_states_t state;
    bool state_known;     /* otadata holds a state for this slot */
} slot_info_t;

static const char *state_name(const slot_info_t *s)
{
    if (!s->state_known) return "undefined";
    switch (s->state) {
    case ESP_OTA_IMG_NEW: return "new";
    case ESP_OTA_IMG_PENDING_VERIFY: return "pending verify";
    case ESP_OTA_IMG_VALID: return "valid";
    case ESP_OTA_IMG_INVALID: return "invalid";
    case ESP_OTA_IMG_ABORTED: return "aborted";
    default: return "undefined";
    }
}

static void slot_read(const esp_partition_t *p, slot_info_t *out)
{
    memset(out, 0, sizeof *out);
    out->state = ESP_OTA_IMG_UNDEFINED;
    if (p == NULL) return;
    snprintf(out->slot, sizeof out->slot, "%s", p->label);
    esp_app_desc_t d;
    if (esp_ota_get_partition_description(p, &d) == ESP_OK) {
        out->has_image = true;
        memcpy(out->ver, d.version, sizeof d.version);
        out->ver[sizeof out->ver - 1] = '\0'; /* image content: never trust the terminator */
    }
    esp_ota_img_states_t st;
    /* ESP_ERR_NOT_FOUND: no state recorded (e.g. slot flashed over USB) -> undefined. */
    if (esp_ota_get_state_partition(p, &st) == ESP_OK) {
        out->state = st;
        out->state_known = true;
    }
}

/* The other slot can be booted: a readable image that is VALID or has no recorded state
 * (UNDEFINED). NEW, PENDING_VERIFY, INVALID and ABORTED are not offered. */
static bool slot_rollback_ok(const slot_info_t *s)
{
    if (!s->has_image) return false;
    if (!s->state_known) return true;
    return s->state == ESP_OTA_IMG_VALID || s->state == ESP_OTA_IMG_UNDEFINED;
}

/* JSON string body: printable ASCII only, quote and backslash escaped, anything else -> '?'.
 * Always terminates; truncates if dst is too small. */
static void json_esc(char *dst, size_t cap, const char *src)
{
    size_t n = 0;
    if (cap == 0) return;
    for (; *src != '\0' && n + 2 < cap; src++) {
        unsigned char c = (unsigned char)*src;
        if (c == '"' || c == '\\') {
            if (n + 3 >= cap) break;
            dst[n++] = '\\';
            dst[n++] = (char)c;
        } else if (c >= 0x20 && c < 0x7f) {
            dst[n++] = (char)c;
        } else {
            dst[n++] = '?';
        }
    }
    dst[n] = '\0';
}

/* ---- GET /update, GET /update/info ---- */

static esp_err_t update_page_get(httpd_req_t *req)
{
    if (!http_srv_req_on_ap(req)) return httpd_resp_send_err(req, HTTPD_404_NOT_FOUND, NULL);
    httpd_resp_set_type(req, "text/html; charset=utf-8");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    return httpd_resp_send(req, ota_page_html_start, HTTPD_RESP_USE_STRLEN);
}

static esp_err_t update_info_get(httpd_req_t *req)
{
    if (!http_srv_req_on_ap(req)) return httpd_resp_send_err(req, HTTPD_404_NOT_FOUND, NULL);

    const esp_partition_t *run = esp_ota_get_running_partition();
    const esp_partition_t *next = esp_ota_get_next_update_partition(NULL);
    const esp_app_desc_t *app = esp_app_get_description();
    slot_info_t cur, oth;
    slot_read(run, &cur);
    slot_read(next, &oth);

    char ver[40], date[40], tm[40], over[40];
    json_esc(ver, sizeof ver, app->version);
    json_esc(date, sizeof date, app->date);
    json_esc(tm, sizeof tm, app->time);
    json_esc(over, sizeof over, oth.ver);

    char buf[640];
    int n = snprintf(buf, sizeof buf,
                     "{\"ver\":\"%s\",\"date\":\"%s\",\"time\":\"%s\",\"slot\":\"%s\",\"state\":\"%s\","
                     "\"other_slot\":\"%s\",\"other_ver\":%s%s%s,\"other_state\":\"%s\","
                     "\"max\":%lu,\"rollback\":%s}",
                     ver, date, tm, cur.slot, state_name(&cur), oth.slot,
                     oth.has_image ? "\"" : "null", oth.has_image ? over : "", oth.has_image ? "\"" : "",
                     state_name(&oth), (unsigned long)(next ? next->size : 0),
                     slot_rollback_ok(&oth) ? "true" : "false");
    if (n < 0 || (size_t)n >= sizeof buf) {
        return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, NULL);
    }
    httpd_resp_set_type(req, "application/json");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    return httpd_resp_send(req, buf, n);
}

/* ---- POST /update ---- */

static esp_err_t update_post(httpd_req_t *req)
{
    if (!http_srv_req_on_ap(req)) return httpd_resp_send_err(req, HTTPD_404_NOT_FOUND, NULL);
    if (!http_srv_origin_ok(req)) return ESP_FAIL;

    const esp_partition_t *next = esp_ota_get_next_update_partition(NULL);
    if (next == NULL) return reply_close(req, HTTPD_500, "no update partition");
    size_t total = req->content_len;
    if (total == 0) return reply_close(req, HTTPD_400, "empty body (Content-Length required)");
    if (total > next->size) return reply_close(req, "413 Payload Too Large", "image too large for the slot");

    if (atomic_flag_test_and_set(&s_busy)) {
        return reply_close(req, "409 Conflict", "another update is in progress or the device is rebooting");
    }

    uint8_t *buf = malloc(OTA_CHUNK);
    if (buf == NULL) {
        atomic_flag_clear(&s_busy);
        return reply_close(req, HTTPD_500, "out of memory");
    }

    esp_ota_handle_t h = 0;
    esp_err_t err = esp_ota_begin(next, OTA_WITH_SEQUENTIAL_WRITES, &h);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "esp_ota_begin(%s): %s", next->label, esp_err_to_name(err));
        free(buf);
        atomic_flag_clear(&s_busy);
        if (err == ESP_ERR_OTA_ROLLBACK_INVALID_STATE) {
            return reply_close(req, "409 Conflict",
                               "running image is not confirmed yet, retry in a minute");
        }
        return reply_close(req, HTTPD_500, "cannot start update");
    }
    ESP_LOGI(TAG, "update to %s: %u bytes", next->label, (unsigned)total);

    size_t got = 0;
    int timeouts = 0;
    unsigned last_pct = 0;
    const char *fail_status = NULL, *fail_msg = NULL;
    bool conn_dead = false;
    while (got < total) {
        size_t want = total - got;
        if (want > OTA_CHUNK) want = OTA_CHUNK;
        int r = httpd_req_recv(req, (char *)buf, want);
        if (r == HTTPD_SOCK_ERR_TIMEOUT) {
            if (++timeouts > OTA_RECV_RETRIES) {
                fail_status = HTTPD_408;
                fail_msg = "upload timed out";
                break;
            }
            continue;
        }
        if (r <= 0) { /* 0: peer closed early, <0: socket error; no response can be sent */
            ESP_LOGW(TAG, "upload aborted after %u of %u bytes (recv %d)", (unsigned)got,
                     (unsigned)total, r);
            conn_dead = true;
            break;
        }
        timeouts = 0;
        err = esp_ota_write(h, buf, (size_t)r);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "esp_ota_write: %s", esp_err_to_name(err));
            if (err == ESP_ERR_OTA_VALIDATE_FAILED) {
                fail_status = HTTPD_400;
                fail_msg = "image invalid";
            } else {
                fail_status = HTTPD_500;
                fail_msg = "flash write failed";
            }
            break;
        }
        got += (size_t)r;
        unsigned pct = (unsigned)((uint64_t)got * 100u / total);
        if (pct / 10u > last_pct / 10u) {
            last_pct = pct;
            ESP_LOGI(TAG, "update: %u %%", pct);
        }
    }
    free(buf);

    if (got < total) {
        esp_ota_abort(h);
        atomic_flag_clear(&s_busy);
        if (conn_dead) return ESP_FAIL;
        return reply_close(req, fail_status, fail_msg);
    }

    /* esp_ota_end releases the handle whether or not it succeeds: no abort after it. */
    err = esp_ota_end(h);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "esp_ota_end: %s", esp_err_to_name(err));
        atomic_flag_clear(&s_busy);
        if (err == ESP_ERR_OTA_VALIDATE_FAILED) return reply(req, HTTPD_400, "image invalid");
        return reply(req, HTTPD_500, "image check failed");
    }
    err = esp_ota_set_boot_partition(next);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "esp_ota_set_boot_partition: %s", esp_err_to_name(err));
        atomic_flag_clear(&s_busy);
        return reply(req, HTTPD_500, "cannot select the new image");
    }
    ESP_LOGI(TAG, "update to %s done, rebooting", next->label);
    /* s_busy stays set: a reboot is pending. */
    esp_err_t sent = reply(req, HTTPD_200, "OK, rebooting");
    schedule_restart();
    return sent;
}

/* ---- POST /update/rollback ---- */

static esp_err_t rollback_post(httpd_req_t *req)
{
    if (!http_srv_req_on_ap(req)) return httpd_resp_send_err(req, HTTPD_404_NOT_FOUND, NULL);
    if (!http_srv_origin_ok(req)) return ESP_FAIL;
    /* Any body is ignored; "Connection: close" in reply() keeps it from being read as a request. */

    if (atomic_flag_test_and_set(&s_busy)) {
        return reply(req, "409 Conflict", "another update is in progress or the device is rebooting");
    }
    const esp_partition_t *other = esp_ota_get_next_update_partition(NULL);
    slot_info_t oth;
    slot_read(other, &oth);
    if (other == NULL || !slot_rollback_ok(&oth)) {
        atomic_flag_clear(&s_busy);
        return reply(req, "409 Conflict", "no valid previous image to roll back to");
    }
    esp_err_t err = esp_ota_set_boot_partition(other);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "rollback: esp_ota_set_boot_partition(%s): %s", other->label,
                 esp_err_to_name(err));
        atomic_flag_clear(&s_busy);
        return reply(req, HTTPD_500, "rollback failed");
    }
    ESP_LOGW(TAG, "rollback to %s (%s), rebooting", other->label, oth.ver);
    esp_err_t sent = reply(req, HTTPD_200, "OK, rebooting to the previous image");
    schedule_restart();
    return sent;
}

static bool s_registered;

bool ota_http_registered(void)
{
    return s_registered;
}

esp_err_t ota_http_register(void)
{
    static const httpd_uri_t get_page = {.uri = "/update", .method = HTTP_GET, .handler = update_page_get};
    static const httpd_uri_t get_info = {.uri = "/update/info", .method = HTTP_GET, .handler = update_info_get};
    static const httpd_uri_t post_up = {.uri = "/update", .method = HTTP_POST, .handler = update_post};
    static const httpd_uri_t post_rb = {.uri = "/update/rollback", .method = HTTP_POST, .handler = rollback_post};
    esp_err_t err = http_srv_register(&get_page);
    if (err == ESP_OK) err = http_srv_register(&get_info);
    if (err == ESP_OK) err = http_srv_register(&post_up);
    if (err == ESP_OK) err = http_srv_register(&post_rb);
    s_registered = (err == ESP_OK);
    return err;
}
