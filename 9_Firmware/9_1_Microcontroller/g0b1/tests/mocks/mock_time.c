#include "hal_time.h"
#include "mock_log.h"

static uint32_t now_us_;
static void (*hook_)(uint32_t now_us);

/* Optional callback run after every time advance (lets a test change an input,
 * e.g. PLL lock detect, at a given mock time). Cleared by mock_reset(). */
void mock_time_set_hook(void (*fn)(uint32_t now_us)) { hook_ = fn; }
static void tick_(void) { if (hook_ != NULL) hook_(now_us_); }

void mock_time_reset(void) { now_us_ = 0; hook_ = NULL; }
void mock_time_advance_us(uint32_t us) { now_us_ += us; tick_(); }  /* wraps like micros() */

uint32_t micros(void) { return now_us_; }
uint32_t millis(void) { return now_us_ / 1000u; }

void delay_us(uint32_t us)
{
    mock_log_add(MOCK_EV_DELAY_US, (int)us, 0, 0, NULL, 0);
    now_us_ += us;
    tick_();
}

void delay_ms(uint32_t ms)
{
    mock_log_add(MOCK_EV_DELAY_MS, (int)ms, 0, 0, NULL, 0);
    now_us_ += ms * 1000u;
    tick_();
}
