#include "hal_time.h"
#include "mock_log.h"

static uint32_t now_us_;

void mock_time_reset(void) { now_us_ = 0; }
void mock_time_advance_us(uint32_t us) { now_us_ += us; }  /* wraps like micros() */

uint32_t micros(void) { return now_us_; }
uint32_t millis(void) { return now_us_ / 1000u; }

void delay_us(uint32_t us)
{
    mock_log_add(MOCK_EV_DELAY_US, (int)us, 0, 0, NULL, 0);
    now_us_ += us;
}

void delay_ms(uint32_t ms)
{
    mock_log_add(MOCK_EV_DELAY_MS, (int)ms, 0, 0, NULL, 0);
    now_us_ += ms * 1000u;
}
