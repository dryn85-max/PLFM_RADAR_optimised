#ifndef HAL_TIME_H
#define HAL_TIME_H
#include <stdint.h>

uint32_t millis(void);
uint32_t micros(void);
void delay_us(uint32_t us);
void delay_ms(uint32_t ms);   /* refreshes nothing; callers keep loops < 1 s */

/* Watchdog kick. hal_time.c provides a weak no-op; main.c overrides it with the
 * real IWDG refresh, the host mock counts calls. Called once per app_loop(). */
void iwdg_refresh(void);

#endif
