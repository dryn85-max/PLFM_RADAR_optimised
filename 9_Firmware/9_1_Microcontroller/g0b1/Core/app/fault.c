#include "fault.h"
#include "sequencer.h"
#include "diag_log.h"

#define FAULT_FIRST_LATCHED 10
#define FAULT_LAST_LATCHED  FAULT_WATCHDOG

/* Survives IWDG/software/NRST reset (the linker script places .noinit after
 * .bss, outside the startup zeroing loop); cleared by a power cycle. */
#ifdef HOST_TEST
static volatile struct { uint32_t magic, code, ncode; } s_latch;
#else
__attribute__((section(".noinit")))
static volatile struct { uint32_t magic, code, ncode; } s_latch;
#endif

static fault_t s_latched = FAULT_NONE;   /* validated copy, set by fault_init() / raise */
static fault_t s_nonlatched = FAULT_NONE;

static int is_latched_code(uint32_t c)
{
    return c >= FAULT_FIRST_LATCHED && c <= FAULT_LAST_LATCHED;
}

/* The first latched code is kept: later causes never overwrite it. */
static void latch_store(fault_t f)
{
    if (s_latched != FAULT_NONE) {
        return;
    }
    s_latch.code = (uint32_t)f;
    s_latch.ncode = ~(uint32_t)f;
    s_latch.magic = FAULT_MAGIC;   /* magic last: a torn write reads as not latched */
    s_latched = f;
}

void fault_init(void)
{
    uint32_t magic = s_latch.magic, code = s_latch.code, ncode = s_latch.ncode;

    s_nonlatched = FAULT_NONE;
    if (magic == FAULT_MAGIC && ncode == ~code && is_latched_code(code)) {
        s_latched = (fault_t)code;
        DIAG_ERR("FLT", "latched fault %u from previous run", (unsigned)code);
    } else {
        s_latched = FAULT_NONE;
        s_latch.magic = 0u;   /* random power-up RAM or corruption: start clean */
        s_latch.code = 0u;
        s_latch.ncode = 0u;
    }
}

fault_t fault_on_boot(uint32_t reset_flags)
{
    if ((reset_flags & (FAULT_RST_IWDG | FAULT_RST_WWDG)) == 0u || s_latched != FAULT_NONE) {
        return FAULT_NONE;
    }
    latch_store(FAULT_WATCHDOG);
    return FAULT_WATCHDOG;
}

int fault_is_latched(void) { return s_latched != FAULT_NONE; }
fault_t fault_latched_code(void) { return s_latched; }

fault_t fault_active(void)
{
    return s_latched != FAULT_NONE ? s_latched : s_nonlatched;
}

void fault_raise(fault_t f)
{
    if (f == FAULT_NONE)
        return;
    if (is_latched_code((uint32_t)f)) {
        latch_store(f);               /* latch first: a hang or reset inside the e-stop still boots latched */
        sequencer_emergency_stop();
        DIAG_ERR("FLT", "latched fault %d", (int)f);
    } else {
        if (s_latched != FAULT_NONE)
            return;   /* emergency stop already applied */
        sequencer_rf_off();
        s_nonlatched = f;
        DIAG_WARN("FLT", "fault %d (RF off)", (int)f);
    }
}

void fault_clear_nonlatched(void) { s_nonlatched = FAULT_NONE; }

void fault_panic(void)
{
    latch_store(FAULT_PANIC);         /* before the e-stop; keeps an earlier code */
    sequencer_emergency_stop();   /* GPIO only: works with a hung bus */
#ifndef HOST_TEST
    for (;;) { }   /* no IWDG refresh: watchdog reset -> boot sees the latch */
#endif
}

#ifdef HOST_TEST
void fault_test_simulate_reset(void) { fault_init(); }

void fault_test_set_raw(uint32_t magic, uint32_t code, uint32_t ncode)
{
    s_latch.magic = magic;
    s_latch.code = code;
    s_latch.ncode = ncode;
}

void fault_test_power_cycle(void)
{
    fault_test_set_raw(0u, 0u, 0u);
    s_latched = FAULT_NONE;
    s_nonlatched = FAULT_NONE;
}

void fault_test_corrupt(uint32_t xm, uint32_t xc, uint32_t xn)
{
    s_latch.magic ^= xm;
    s_latch.code ^= xc;
    s_latch.ncode ^= xn;
}
#endif
