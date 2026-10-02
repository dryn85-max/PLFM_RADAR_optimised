/* Status LED: one task owns the RGB LED (20 ms tick, colour from the core sl_color(), written
 * only on change). The setters are thread-safe and may be called before status_led_start()
 * (state is just stored) or after a failed start (the LED stays dark). */
#ifndef STATUS_LED_TASK_H
#define STATUS_LED_TASK_H

#include <stdbool.h>
#include "esp_err.h"
#include "status_led.h"

/* rgb_led_init() + the task. An error is returned for logging; nothing else depends on it. */
esp_err_t status_led_start(void);

/* BOOT button held (bb_is_held) and its zone (bb_held_zone). */
void status_led_set_button(bool held, bb_zone_t zone);
/* AP state; a change to SL_AP_ON_DEMAND restarts the blink phase. */
void status_led_set_ap(sl_ap_t ap);
/* Confirmation flash in the given colour, starting now. */
void status_led_flash(sl_rgb_t color);

#endif
