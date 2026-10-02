#include "http_srv.h"

#include <string.h>

#include "esp_log.h"
#include "esp_netif.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "lwip/sockets.h"

#include "wifi_form.h"
#include "wifi_mgr.h"

static const char *TAG = "http";
static httpd_handle_t s_server;

static const char WIFI_PAGE[] =
    "<!doctype html><html><head><meta charset=\"utf-8\">"
    "<meta name=\"viewport\" content=\"width=device-width,initial-scale=1\">"
    "<title>AERIS-10 Lite Wi-Fi</title></head><body>"
    "<h1>Wi-Fi setup</h1>"
    "<form method=\"post\" action=\"/wifi\">"
    "<p><label>SSID<br><input name=\"ssid\" maxlength=\"32\" required></label></p>"
    "<p><label>Password (empty = open network, otherwise 8-64 characters)<br>"
    "<input name=\"password\" type=\"password\" maxlength=\"64\"></label></p>"
    "<p><button type=\"submit\">Save and reboot</button></p>"
    "</form></body></html>";

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
    httpd_resp_set_type(req, "text/html");
    return httpd_resp_send(req, WIFI_PAGE, HTTPD_RESP_USE_STRLEN);
}

static void restart_cb(void *arg)
{
    (void)arg;
    esp_restart();
}

static esp_err_t wifi_post(httpd_req_t *req)
{
    if (!http_srv_req_on_ap(req)) return httpd_resp_send_err(req, HTTPD_404_NOT_FOUND, NULL);
    if (req->content_len == 0 || req->content_len > WF_BODY_MAX) {
        return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "bad length");
    }
    char body[WF_BODY_MAX];
    size_t got = 0;
    while (got < req->content_len) {
        int r = httpd_req_recv(req, body + got, req->content_len - got);
        if (r == HTTPD_SOCK_ERR_TIMEOUT) continue;
        if (r <= 0) return ESP_FAIL;
        got += (size_t)r;
    }
    char ssid[WF_SSID_MAX + 1], pass[WF_PASS_MAX + 1];
    int rc = wf_parse_credentials(body, got, ssid, pass);
    memset(body, 0, sizeof body);
    if (rc != WF_OK) {
        memset(pass, 0, sizeof pass);
        return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "invalid ssid or password");
    }
    esp_err_t err = wifi_mgr_save_credentials(ssid, pass);
    memset(pass, 0, sizeof pass);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "saving credentials failed: %s", esp_err_to_name(err));
        return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "save failed");
    }
    httpd_resp_set_type(req, "text/plain");
    httpd_resp_send(req, "Saved, rebooting", HTTPD_RESP_USE_STRLEN);

    const esp_timer_create_args_t targs = {.callback = restart_cb, .name = "wifi_restart"};
    esp_timer_handle_t t;
    if (esp_timer_create(&targs, &t) == ESP_OK) esp_timer_start_once(t, 1000000);
    return ESP_OK;
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
    cfg.max_uri_handlers = 8;
    cfg.stack_size = 6144;
    cfg.lru_purge_enable = true;
    esp_err_t err = httpd_start(&s_server, &cfg);
    if (err != ESP_OK) return err;

    static const httpd_uri_t get_wifi = {.uri = "/wifi", .method = HTTP_GET, .handler = wifi_get};
    static const httpd_uri_t post_wifi = {.uri = "/wifi", .method = HTTP_POST, .handler = wifi_post};
    err = http_srv_register(&get_wifi);
    if (err == ESP_OK) err = http_srv_register(&post_wifi);
    return err;
}
