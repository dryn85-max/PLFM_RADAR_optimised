#include <stdio.h>

#include "esp_heap_caps.h"
#include "esp_log.h"
#include "nvs_flash.h"

#include "http_srv.h"
#include "ld2410_task.h"
#include "wifi_mgr.h"

#define RING_BYTES_PSRAM (4u * 1024u * 1024u)
#define RING_BYTES_INTERNAL (32u * 1024u)

static const char *TAG = "app";

void app_main(void)
{
    printf("\nAERIS-10 Lite MVP (ESP32-S3)\n");

    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        err = nvs_flash_init();
    }
    ESP_ERROR_CHECK(err);

    size_t ring_bytes = RING_BYTES_PSRAM;
    void *ring = heap_caps_malloc(ring_bytes, MALLOC_CAP_SPIRAM);
    if (ring == NULL) {
        ESP_LOGW(TAG, "PSRAM ring buffer (%u bytes) failed, using %u bytes of internal RAM",
                 (unsigned)RING_BYTES_PSRAM, (unsigned)RING_BYTES_INTERNAL);
        ring_bytes = RING_BYTES_INTERNAL;
        ring = heap_caps_malloc(ring_bytes, MALLOC_CAP_8BIT);
    }
    if (ring == NULL) {
        ESP_LOGE(TAG, "no memory for the ring buffer");
        return;
    }
    ESP_LOGI(TAG, "ring buffer: %u bytes", (unsigned)ring_bytes);

    err = ld2410_start(ring, ring_bytes);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "ld2410_start failed: %s", esp_err_to_name(err));
    }

    /* Wi-Fi after the sensor task so the LD2410C is not delayed by the 15 s STA attempt. */
    err = wifi_mgr_start();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "wifi_mgr_start failed: %s", esp_err_to_name(err));
    }
    err = wifi_mgr_boot_monitor_start();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "BOOT monitor failed: %s", esp_err_to_name(err));
    }
    err = http_srv_start();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "http_srv_start failed: %s", esp_err_to_name(err));
    }
}
