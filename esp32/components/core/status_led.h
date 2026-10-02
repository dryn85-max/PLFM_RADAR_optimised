/* Status LED colour logic (plain C11, pure function, no hardware access).
 * Priority: button held > confirmation flash > AP only (steady) > AP on demand (blink) > off.
 * Colours are full scale; brightness scaling is the driver's job. Times are uint32 ms and
 * may wrap: all differences use unsigned subtraction. */
#ifndef STATUS_LED_H
#define STATUS_LED_H

#include <stdbool.h>
#include <stdint.h>
#include "boot_btn.h"

#define SL_FLASH_COUNT 3u
#define SL_FLASH_ON_MS 100u
#define SL_FLASH_PERIOD_MS 200u
#define SL_FLASH_TOTAL_MS (SL_FLASH_COUNT * SL_FLASH_PERIOD_MS) /* 600 */
#define SL_BLINK_PERIOD_MS 1000u
#define SL_BLINK_ON_MS 500u

typedef struct { uint8_t r, g, b; } sl_rgb_t;

#define SL_OFF   ((sl_rgb_t){0, 0, 0})
#define SL_BLUE  ((sl_rgb_t){0, 0, 255})
#define SL_RED   ((sl_rgb_t){255, 0, 0})

typedef enum { SL_AP_OFF = 0, SL_AP_ON_DEMAND = 1, SL_AP_ONLY = 2 } sl_ap_t;

typedef struct {
    bool held;              /* button currently held (bootbtn_is_held) */
    bb_zone_t zone;         /* its zone (bootbtn_held_zone); NONE/CANCEL zones show off */
    sl_ap_t ap;
    sl_rgb_t flash_color;   /* confirmation flash: SL_FLASH_COUNT x (100 ms on, 100 ms off) */
    uint32_t flash_start_ms;
    bool flash_active;      /* the caller may leave it set; the flash ends by itself */
    uint32_t ap_ref_ms;     /* blink phase reference: on in the first 500 ms of each 1 s */
} sl_in_t;

sl_rgb_t sl_color(const sl_in_t *in, uint32_t t_ms);

#endif
