/* Recording server: TCP port 5410, one client at a time (a new connection
 * replaces the old one). Protocol: components/core/rec_proto.h. */
#ifndef REC_SRV_H
#define REC_SRV_H

#include <stdint.h>

#include "esp_err.h"

#define REC_SRV_PORT 5410

/* Generate the boot id and start the accept and serve tasks (core 0). Call
 * once, after ld2410_start() and wifi_mgr_start(). */
esp_err_t rec_srv_start(void);

/* Random per boot, never 0; 0 before rec_srv_start(). */
uint32_t rec_srv_boot_id(void);

#endif
