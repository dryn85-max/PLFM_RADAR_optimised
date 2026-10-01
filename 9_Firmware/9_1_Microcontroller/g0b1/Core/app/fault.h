/* Fault latch (RAM-retained .noinit) and fault handling. */
#ifndef FAULT_H
#define FAULT_H
#include <stdint.h>

typedef enum { FAULT_NONE = 0, FAULT_PLL_LOCK = 1, FAULT_ADAR_COMM = 2,
               FAULT_OVERTEMP = 10, FAULT_ESTOP_CMD = 11, FAULT_PANIC = 12 } fault_t;
/* codes >= 10 are latched */

#define FAULT_MAGIC 0x4641554Cu   /* "FAUL" */

void    fault_init(void);              /* reads .noinit latch; validates magic/~code */
int     fault_is_latched(void);
fault_t fault_latched_code(void);
fault_t fault_active(void);            /* latched code, else current non-latched code, else NONE */
/* Latched codes: sequencer_emergency_stop() then latch (first latch is kept).
 * Non-latched codes: sequencer_rf_off() and remember (ignored while latched).
 * FAULT_NONE is ignored. */
void    fault_raise(fault_t f);
void    fault_clear_nonlatched(void);
/* Error_Handler / HardFault: GPIO-only e-stop, latch FAULT_PANIC, then spin
 * WITHOUT refreshing the IWDG so the reset boots into the latched state.
 * Never returns on target; returns on the host test build. */
void    fault_panic(void);

#ifdef HOST_TEST
void fault_test_simulate_reset(void);   /* re-run fault_init() keeping the latch RAM */
void fault_test_power_cycle(void);      /* wipe the latch RAM and the non-latched code */
void fault_test_set_raw(uint32_t magic, uint32_t code, uint32_t ncode);
void fault_test_corrupt(uint32_t xor_magic, uint32_t xor_code, uint32_t xor_ncode);
#endif

#endif
