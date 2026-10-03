/* Web console log: esp_log vprintf hook feeding a 16 KB ring (core/log_ring), and the
 * GET /log page plus GET /log/data?from=N poll endpoint. */
#ifndef WEB_LOG_H
#define WEB_LOG_H

#include "esp_err.h"

/* Bytes kept in the ring (the newest ones; older lines are overwritten). */
#define WEB_LOG_BYTES (16u * 1024u)

/* Install the hook. Call first thing in app_main (before other modules log) so boot logs are
 * captured. Console output is unchanged: the previous vprintf is always called too.
 * Idempotent; returns ESP_ERR_NO_MEM if the read mutex cannot be created (the hook is then not
 * installed). */
esp_err_t web_log_init(void);

/* Register /log and /log/data (served on STA and AP alike). Call after http_srv_start(). */
esp_err_t web_log_register(void);

#endif
