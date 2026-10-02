/* Live view: page at GET /, WebSocket /ws pushing the latest snapshot at 10 Hz. */
#ifndef LIVE_H
#define LIVE_H

#include "esp_err.h"

/* Call before http_srv_start(): creates the lock and installs the close hook. */
void live_prepare(void);
/* Call after http_srv_start(): registers / and /ws, starts the 10 Hz broadcaster. */
esp_err_t live_start(void);

#endif
