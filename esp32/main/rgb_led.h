/* On-board addressable RGB LED (WS2812-type) of the ESP32-S3-DevKitC-1, driven by the RMT TX
 * driver. Only the status LED task calls rgb_led_set(); it is not thread-safe. */
#ifndef RGB_LED_H
#define RGB_LED_H

#include <stdint.h>
#include "esp_err.h"

/* Data pin of the on-board LED: GPIO38 on DevKitC-1 v1.1
 * (hardware/datasheets/esp_dev_kits_en_master_esp32s3-3540495.pdf, ch. 1 ESP32-S3-DevKitC-1,
 * "Description of Components" p. 4 and "Hardware Revision Details" p. 5). The v1.0 boards use
 * GPIO48: change this one constant for them. */
#define RGB_LED_GPIO 38

/* Brightness scaling of every channel, percent of full scale (the LED is very bright). */
#define RGB_LED_BRIGHTNESS_PCT 5

/* Create the RMT channel and encoder. Failure is returned (never aborts); rgb_led_set() then
 * returns ESP_ERR_INVALID_STATE. */
esp_err_t rgb_led_init(void);

/* Show a colour (full-scale values, scaled by RGB_LED_BRIGHTNESS_PCT) and wait for the
 * transmission to finish (short timeout). */
esp_err_t rgb_led_set(uint8_t r, uint8_t g, uint8_t b);

#endif
