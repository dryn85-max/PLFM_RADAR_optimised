#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"
#include "esp_log.h"
#include "esp_ota_ops.h"
#include "esp_timer.h"

#include "http_srv.h"
#include "ota_check.h"
#include "ota_validate.h"
#include "wifi_mgr.h"

#define CHECK_PERIOD_US 1000000

static const char *TAG = "ota";
static esp_timer_handle_t s_timer;

/* Runs in the esp_timer task. The rollback call reboots and does not return; the timer task has
 * no other duty that matters at that point, and logging has already been issued. */
static void check_cb(void *arg)
{
    (void)arg;
    uint32_t up_ms = (uint32_t)(esp_timer_get_time() / 1000);
    ota_act_t act = ota_check_step(true, wifi_mgr_is_up(), http_srv_is_running(), up_ms);
    if (act == OTA_ACT_MARK_VALID) {
        esp_timer_stop(s_timer);
        esp_err_t err = esp_ota_mark_app_valid_cancel_rollback();
        if (err == ESP_OK) {
            ESP_LOGI(TAG, "OTA: new image marked valid");
        } else {
            ESP_LOGE(TAG, "OTA: mark valid failed: %s", esp_err_to_name(err));
        }
    } else if (act == OTA_ACT_ROLLBACK) {
        esp_timer_stop(s_timer);
        ESP_LOGW(TAG, "OTA: new image not healthy after %u s (Wi-Fi %d, HTTP %d), rolling back",
                 (unsigned)(up_ms / 1000), (int)wifi_mgr_is_up(), (int)http_srv_is_running());
        esp_err_t err = esp_ota_mark_app_invalid_rollback_and_reboot();
        /* Only returns on failure (e.g. no previous valid image). */
        ESP_LOGE(TAG, "OTA: rollback failed: %s", esp_err_to_name(err));
    }
}

void ota_validate_start(void)
{
    const esp_partition_t *run = esp_ota_get_running_partition();
    if (run == NULL) {
        ESP_LOGW(TAG, "OTA: running partition unknown");
        return;
    }
    esp_ota_img_states_t state = ESP_OTA_IMG_UNDEFINED;
    esp_err_t err = esp_ota_get_state_partition(run, &state);
    if (err != ESP_OK) {
        /* ESP_ERR_NOT_SUPPORTED: not an OTA slot; ESP_ERR_NOT_FOUND: no otadata state. */
        ESP_LOGI(TAG, "OTA: running %s, no image state (%s)", run->label, esp_err_to_name(err));
        return;
    }
    if (state != ESP_OTA_IMG_PENDING_VERIFY) {
        ESP_LOGI(TAG, "OTA: running %s, state %d", run->label, (int)state);
        return;
    }
    ESP_LOGW(TAG, "OTA: running %s, state pending verify: valid after %u s of Wi-Fi + HTTP, "
                  "rollback at %u s", run->label, (unsigned)(OTA_VALID_AFTER_MS / 1000),
             (unsigned)(OTA_ROLLBACK_AFTER_MS / 1000));
    const esp_timer_create_args_t args = {.callback = check_cb, .name = "ota_check"};
    err = esp_timer_create(&args, &s_timer);
    if (err == ESP_OK) err = esp_timer_start_periodic(s_timer, CHECK_PERIOD_US);
    if (err != ESP_OK) {
        /* Cannot validate: leave it pending, the bootloader rolls back on the next reset. */
        ESP_LOGE(TAG, "OTA: validation timer failed: %s", esp_err_to_name(err));
    }
}
