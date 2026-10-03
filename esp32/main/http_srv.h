/* HTTP server (port 80), shared with the live page / WebSocket (Task 5). */
#ifndef HTTP_SRV_H
#define HTTP_SRV_H

#include <stdbool.h>

#include "esp_err.h"
#include "esp_http_server.h"

/* Start the server and register the /wifi setup handlers. Call once, after wifi_mgr_start. */
esp_err_t http_srv_start(void);

/* Register more handlers (the uri string must stay valid). */
esp_err_t http_srv_register(const httpd_uri_t *uri);

/* Server handle (NULL before http_srv_start). */
httpd_handle_t http_srv_handle(void);

/* True once the server has been started. */
bool http_srv_is_running(void);

/* Called (in the httpd task) when a client socket is closed; set before http_srv_start. */
void http_srv_set_close_hook(void (*hook)(int fd));

/* True if the request arrived on the soft-AP interface (conservative: unknown -> false). */
bool http_srv_req_on_ap(httpd_req_t *req);

/* The soft-AP address (ESP-IDF default) and the only browser Origin the AP-only POST endpoints
 * accept (CSRF guard, see origin_check.h). */
#define HTTP_SRV_AP_ORIGIN "http://192.168.4.1"

/* CSRF guard for the state-changing POST handlers; call after http_srv_req_on_ap. Absent Origin
 * (curl) passes. Otherwise it must match HTTP_SRV_AP_ORIGIN, else this sends 403 "cross-site
 * request rejected" with Connection: close, logs one warning and returns false: the handler
 * must then return ESP_FAIL without reading the body (httpd closes the socket). An Origin
 * header longer than 64 bytes is rejected too. */
bool http_srv_origin_ok(httpd_req_t *req);

#endif
