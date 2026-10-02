#include "status_led_task.h"

#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "rgb_led.h"

#define TICK_MS 20

static const char *TAG = "status_led";

static portMUX_TYPE s_mux = portMUX_INITIALIZER_UNLOCKED;
static sl_in_t s_in; /* guarded by s_mux; zero = nothing held, AP off, no flash */

static uint32_t now_ms(void) { return (uint32_t)(esp_timer_get_time() / 1000); }

void status_led_set_button(bool held, bb_zone_t zone)
{
    portENTER_CRITICAL(&s_mux);
    s_in.held = held;
    s_in.zone = zone;
    portEXIT_CRITICAL(&s_mux);
}

void status_led_set_ap(sl_ap_t ap)
{
    uint32_t t = now_ms();
    portENTER_CRITICAL(&s_mux);
    if (ap != s_in.ap && ap == SL_AP_ON_DEMAND) s_in.ap_ref_ms = t;
    s_in.ap = ap;
    portEXIT_CRITICAL(&s_mux);
}

void status_led_flash(sl_rgb_t color)
{
    uint32_t t = now_ms();
    portENTER_CRITICAL(&s_mux);
    s_in.flash_color = color;
    s_in.flash_start_ms = t;
    s_in.flash_active = true;
    portEXIT_CRITICAL(&s_mux);
}

static void led_task(void *arg)
{
    (void)arg;
    sl_rgb_t last = SL_OFF;
    bool first = true; /* write the initial "off" once so the LED state is known */
    TickType_t period = pdMS_TO_TICKS(TICK_MS);
    if (period == 0) period = 1;
    TickType_t wake = xTaskGetTickCount();
    for (;;) {
        vTaskDelayUntil(&wake, period);
        sl_in_t in;
        portENTER_CRITICAL(&s_mux);
        in = s_in;
        portEXIT_CRITICAL(&s_mux);
        sl_rgb_t c = sl_color(&in, now_ms());
        if (first || c.r != last.r || c.g != last.g || c.b != last.b) {
            first = false;
            last = c;
            (void)rgb_led_set(c.r, c.g, c.b);
        }
    }
}

esp_err_t status_led_start(void)
{
    esp_err_t err = rgb_led_init();
    if (err != ESP_OK) return err;
    if (xTaskCreate(led_task, "status_led", 3072, NULL, 2, NULL) != pdPASS) {
        ESP_LOGE(TAG, "cannot create the LED task");
        return ESP_ERR_NO_MEM;
    }
    return ESP_OK;
}
