/* SNTP time (ESP-IDF esp_netif_sntp): started when the STA gets an IP, each sync is recorded
 * as a time_sync record (source SNTP). Needs ld2410_start() to have created the ring. */
#ifndef SNTP_SYNC_H
#define SNTP_SYNC_H

#include "time_source.h"

/* Start SNTP once (later calls are no-ops, so STA reconnects are safe). */
void sntp_sync_start(void);

/* GPS while a GPS time_sync happened within the last 5 s, else SNTP if any sync happened. */
time_source_t time_source_current(void);

#endif
