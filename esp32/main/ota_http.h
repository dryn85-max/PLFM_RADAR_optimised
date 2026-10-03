/* Over-the-air update over the soft-AP: GET /update (page), GET /update/info (JSON for the page),
 * POST /update (raw .bin body) and POST /update/rollback. Every handler answers 404 unless the
 * request arrived on the AP interface (http_srv_req_on_ap). */
#ifndef OTA_HTTP_H
#define OTA_HTTP_H

#include "esp_err.h"

/* Register the four handlers (4 of the server's URI slots). Call after http_srv_start(). */
esp_err_t ota_http_register(void);

#endif
