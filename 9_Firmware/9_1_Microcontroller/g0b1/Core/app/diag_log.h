/* Bring-up diagnostic logging (port of the F7 diag_log.h).
 * Timestamped, subsystem-tagged printf; output goes to USART2 via _write.
 * -DDIAG_DISABLE (make DIAG=0) turns every macro into ((void)0).
 * Never use inside per-chirp or AGC paths. */
#ifndef DIAG_LOG_H
#define DIAG_LOG_H

#include <stdio.h>
#include <stdint.h>
#include "hal_gpio.h"
#include "hal_time.h"

#ifndef DIAG_DISABLE

#ifdef DIAG_VERBOSE
#define DIAG(subsys, fmt, ...) \
    printf("[%7lu ms] %s: " fmt " (%s:%d)\n", \
           (unsigned long)millis(), subsys, ##__VA_ARGS__, __FILE__, __LINE__)
#else
#define DIAG(subsys, fmt, ...) \
    printf("[%7lu ms] %s: " fmt "\n", \
           (unsigned long)millis(), subsys, ##__VA_ARGS__)
#endif

#define DIAG_WARN(subsys, fmt, ...) \
    printf("[%7lu ms] %s WARN: " fmt "\n", \
           (unsigned long)millis(), subsys, ##__VA_ARGS__)

#define DIAG_ERR(subsys, fmt, ...) \
    printf("[%7lu ms] %s **ERR**: " fmt "\n", \
           (unsigned long)millis(), subsys, ##__VA_ARGS__)

#define DIAG_REG(subsys, name, val) \
    printf("[%7lu ms] %s: %s = 0x%02X\n", \
           (unsigned long)millis(), subsys, name, (unsigned int)(val))

#define DIAG_REG32(subsys, name, val) \
    printf("[%7lu ms] %s: %s = 0x%08lX\n", \
           (unsigned long)millis(), subsys, name, (unsigned long)(val))

/* pin is a gpio_t (upstream took port+pin; the HAL abstraction hides those). */
#define DIAG_GPIO(subsys, name, pin) \
    printf("[%7lu ms] %s: GPIO %s = %s\n", \
           (unsigned long)millis(), subsys, name, \
           gpio_read(pin) ? "HIGH" : "LOW")

#define DIAG_BOOL(subsys, name, val) \
    printf("[%7lu ms] %s: %s = %s\n", \
           (unsigned long)millis(), subsys, name, (val) ? "YES" : "NO")

#define DIAG_SECTION(title) \
    printf("[%7lu ms] ======== %s ========\n", \
           (unsigned long)millis(), title)

#define DIAG_ELAPSED(subsys, label, start_tick) \
    printf("[%7lu ms] %s: %s took %lu ms\n", \
           (unsigned long)millis(), subsys, label, \
           (unsigned long)(millis() - (uint32_t)(start_tick)))

#else /* DIAG_DISABLE */

#define DIAG(subsys, fmt, ...)              ((void)0)
#define DIAG_WARN(subsys, fmt, ...)         ((void)0)
#define DIAG_ERR(subsys, fmt, ...)          ((void)0)
#define DIAG_REG(subsys, name, val)         ((void)0)
#define DIAG_REG32(subsys, name, val)       ((void)0)
#define DIAG_GPIO(subsys, name, pin)        ((void)0)
#define DIAG_BOOL(subsys, name, val)        ((void)0)
#define DIAG_SECTION(title)                 ((void)0)
#define DIAG_ELAPSED(subsys, label, start)  ((void)0)

#endif /* DIAG_DISABLE */

/* Subsystem tags: "LO" "BF" (ADAR1000) "PA" "FPGA" "PWR" "AGC" "CMD" "SYS" "FLT" "THM" */

#endif
