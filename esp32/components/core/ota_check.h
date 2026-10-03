/* OTA validity decision (plain C11, no hardware access).
 * Evaluated once per second while the running image is "pending verify":
 *   MARK_VALID: Wi-Fi up and HTTP server running and uptime >= OTA_VALID_AFTER_MS.
 *   ROLLBACK:   uptime >= OTA_ROLLBACK_AFTER_MS and the conditions above do not hold.
 * MARK_VALID takes priority: if the conditions hold, a healthy image is marked valid even when
 * the uptime is already past the rollback limit (e.g. a late evaluation).
 * Not pending (first USB flash, already valid): always NONE. */
#ifndef OTA_CHECK_H
#define OTA_CHECK_H

#include <stdbool.h>
#include <stdint.h>

#define OTA_VALID_AFTER_MS    30000u
#define OTA_ROLLBACK_AFTER_MS 120000u

typedef enum { OTA_ACT_NONE = 0, OTA_ACT_MARK_VALID = 1, OTA_ACT_ROLLBACK = 2 } ota_act_t;

ota_act_t ota_check_step(bool pending, bool wifi_up, bool http_up, uint32_t uptime_ms);

#endif
