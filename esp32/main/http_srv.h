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

/* Called (in the httpd task) when a client socket is closed; set before http_srv_start. */
void http_srv_set_close_hook(void (*hook)(int fd));

/* True if the request arrived on the soft-AP interface (conservative: unknown -> false). */
bool http_srv_req_on_ap(httpd_req_t *req);

#endif
