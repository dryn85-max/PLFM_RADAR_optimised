#include "ota_check.h"

ota_act_t ota_check_step(bool pending, bool wifi_up, bool http_up, uint32_t uptime_ms)
{
    if (!pending) return OTA_ACT_NONE;
    if (wifi_up && http_up && uptime_ms >= OTA_VALID_AFTER_MS) return OTA_ACT_MARK_VALID;
    if (uptime_ms >= OTA_ROLLBACK_AFTER_MS) return OTA_ACT_ROLLBACK;
    return OTA_ACT_NONE;
}
