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

/* True if the request arrived on the soft-AP interface (conservative: unknown -> false). */
bool http_srv_req_on_ap(httpd_req_t *req);

#endif
