/* Fault latch (RAM-retained .noinit) and fault handling. */
#ifndef FAULT_H
#define FAULT_H
#include <stdint.h>

typedef enum { FAULT_NONE = 0, FAULT_PLL_LOCK = 1, FAULT_ADAR_COMM = 2,
               FAULT_OVERTEMP = 10, FAULT_ESTOP_CMD = 11, FAULT_PANIC = 12,
               FAULT_WATCHDOG = 13 } fault_t;
/* codes >= 10 are latched (FAULT_WATCHDOG: the previous run ended in an
 * IWDG/WWDG reset that no fault_panic() / e-stop explained) */

/* RCC_CSR reset-source flags (RM0444, bit positions; main.c asserts they match
 * the CMSIS RCC_CSR_*RSTF masks). Passed to fault_on_boot(). */
#define FAULT_RST_OBL   (1u << 25)
#define FAULT_RST_PIN   (1u << 26)
#define FAULT_RST_PWR   (1u << 27)
#define FAULT_RST_SFT   (1u << 28)
#define FAULT_RST_IWDG  (1u << 29)
#define FAULT_RST_WWDG  (1u << 30)
#define FAULT_RST_LPWR  (1u << 31)

#define FAULT_MAGIC 0x4641554Cu   /* "FAUL" */

void    fault_init(void);              /* reads .noinit latch; validates magic/~code */
int     fault_is_latched(void);
fault_t fault_latched_code(void);
fault_t fault_active(void);            /* latched code, else current non-latched code, else NONE */
/* Boot-time reset-cause policy (spec R4). Call once after fault_init() and
 * before app_init(), with the RCC_CSR reset flags. If the reset was caused by
 * the IWDG or WWDG and no valid latch exists, latch FAULT_WATCHDOG and return
 * it; otherwise (other reset causes, or a valid latch already present: the
 * first cause is kept) return FAULT_NONE. Touches only the latch RAM: no GPIO,
 * app_init() applies the e-stop for a latched boot. */
fault_t fault_on_boot(uint32_t reset_flags);
/* Latched codes: latch first (first latch is kept), then sequencer_emergency_stop().
 * Non-latched codes: sequencer_rf_off() and remember (ignored while latched).
 * FAULT_NONE is ignored. */
void    fault_raise(fault_t f);
void    fault_clear_nonlatched(void);
/* Error_Handler / HardFault / NMI / unexpected IRQ: latch FAULT_PANIC (unless a
 * code is already latched: the first cause is kept), GPIO-only e-stop, then
 * spin WITHOUT refreshing the IWDG so the reset boots into the latched state.
 * Never returns on target; returns on the host test build. */
void    fault_panic(void);

#ifdef HOST_TEST
void fault_test_simulate_reset(void);   /* re-run fault_init() keeping the latch RAM */
void fault_test_power_cycle(void);      /* wipe the latch RAM and the non-latched code */
void fault_test_set_raw(uint32_t magic, uint32_t code, uint32_t ncode);
void fault_test_corrupt(uint32_t xor_magic, uint32_t xor_code, uint32_t xor_ncode);
#endif

#endif
