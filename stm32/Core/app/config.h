/* Compile-time configuration shared by HAL, drivers and application. */
#ifndef CONFIG_H
#define CONFIG_H

/* Number of ADAR1000 devices (1 on the prototype, 4 on the full array).
 * Channel index g = dev*4 + ch, 0 .. ADAR_COUNT*4-1. Set with -DADAR_COUNT=n. */
#ifndef ADAR_COUNT
#define ADAR_COUNT 1
#endif
#if ADAR_COUNT < 1 || ADAR_COUNT > 4
#error "ADAR_COUNT must be 1..4"
#endif

/* SPI clock ceilings (the HAL picks the largest divider-derived rate <= these). */
#ifndef SPI_ADAR_MAX_HZ
#define SPI_ADAR_MAX_HZ 20000000u
#endif
#ifndef SPI_PLL_MAX_HZ
#define SPI_PLL_MAX_HZ 10000000u
#endif

#define OVERTEMP_DECI_C       750    /* 75.0 degC, latched fault at or above */
#define THERMAL_PERIOD_MS     5000
#define AGC_PERIOD_MS         250
#define IWDG_PRESCALER_DIV    256    /* LSI 32 kHz / 256 = 125 Hz */
#define IWDG_RELOAD           500    /* 500 / 125 Hz = 4.0 s */
#define PLL_LOCK_TIMEOUT_MS   100

/* LO PLL part: define exactly one of PLL_PART_LMX2594 / PLL_PART_ADF4372
 * (default LMX2594). Selects the table returned by pll_default_table(). */
#if !defined(PLL_PART_LMX2594) && !defined(PLL_PART_ADF4372)
#define PLL_PART_LMX2594
#endif
#if defined(PLL_PART_LMX2594) && defined(PLL_PART_ADF4372)
#error "define only one of PLL_PART_LMX2594 / PLL_PART_ADF4372"
#endif

#endif
