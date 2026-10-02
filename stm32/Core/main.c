/* Target wiring: 64 MHz clock, peripherals, IWDG (4 s), then app_init() and the
 * app_loop() superloop. All logic lives in Core/app (host-tested). */
#include "stm32g0xx_hal.h"
#include "hal_gpio.h"
#include "hal_init.h"
#include "hal_time.h"
#include "hal_uart.h"
#include "app.h"
#include "config.h"
#include "fault.h"
#include "pll_lo.h"

/* IWDG: LSI 32 kHz / 256 * 500 counts = 4.0 s window (config.h). */
_Static_assert(IWDG_PRESCALER_DIV == 256, "IWDG prescaler constant must match IWDG_PRESCALER_256");
static IWDG_HandleTypeDef s_hiwdg;

/* Strong override of the weak no-op in hal_time.c; called once per app_loop(). */
void iwdg_refresh(void)
{
    (void)HAL_IWDG_Refresh(&s_hiwdg);
}

static void iwdg_start(void)
{
    s_hiwdg.Instance = IWDG;
    s_hiwdg.Init.Prescaler = IWDG_PRESCALER_256;
    s_hiwdg.Init.Reload = IWDG_RELOAD;
    s_hiwdg.Init.Window = IWDG_WINDOW_DISABLE;
    if (HAL_IWDG_Init(&s_hiwdg) != HAL_OK) {
        Error_Handler();
    }
}

void Error_Handler(void)
{
    fault_panic();   /* e-stop, latch, spin without IWDG refresh */
    for (;;) { }     /* fault_panic() does not return on target */
}

/* fault.h reset-flag masks must be the RCC_CSR flags. */
_Static_assert(FAULT_RST_IWDG == RCC_CSR_IWDGRSTF && FAULT_RST_WWDG == RCC_CSR_WWDGRSTF &&
               FAULT_RST_PIN == RCC_CSR_PINRSTF && FAULT_RST_PWR == RCC_CSR_PWRRSTF &&
               FAULT_RST_SFT == RCC_CSR_SFTRSTF && FAULT_RST_OBL == RCC_CSR_OBLRSTF &&
               FAULT_RST_LPWR == RCC_CSR_LPWRRSTF, "fault.h reset flags != RCC_CSR");

static void SystemClock_Config(void)
{
    RCC_OscInitTypeDef osc = {0};
    RCC_ClkInitTypeDef clk = {0};

    /* HSI16 -> PLL: 16 / M(1) * N(8) = 128 MHz VCO, / R(2) = 64 MHz */
    osc.OscillatorType = RCC_OSCILLATORTYPE_HSI;
    osc.HSIState = RCC_HSI_ON;
    osc.HSIDiv = RCC_HSI_DIV1;
    osc.HSICalibrationValue = RCC_HSICALIBRATION_DEFAULT;
    osc.PLL.PLLState = RCC_PLL_ON;
    osc.PLL.PLLSource = RCC_PLLSOURCE_HSI;
    osc.PLL.PLLM = RCC_PLLM_DIV1;
    osc.PLL.PLLN = 8;
    osc.PLL.PLLP = RCC_PLLP_DIV2;
    osc.PLL.PLLQ = RCC_PLLQ_DIV2;
    osc.PLL.PLLR = RCC_PLLR_DIV2;
    if (HAL_RCC_OscConfig(&osc) != HAL_OK) {
        Error_Handler();
    }
    clk.ClockType = RCC_CLOCKTYPE_HCLK | RCC_CLOCKTYPE_SYSCLK | RCC_CLOCKTYPE_PCLK1;
    clk.SYSCLKSource = RCC_SYSCLKSOURCE_PLLCLK;
    clk.AHBCLKDivider = RCC_SYSCLK_DIV1;
    clk.APB1CLKDivider = RCC_HCLK_DIV1;
    if (HAL_RCC_ClockConfig(&clk, FLASH_LATENCY_2) != HAL_OK) {
        Error_Handler();
    }
}

int main(void)
{
    static const char banner[] = "AERIS-10 Lite G0B1 boot\r\n";
    static const char placeholder[] = "PLL table is a placeholder\r\n";

    hal_gpio_init();            /* all rails/enables/resets low before anything else */
    HAL_Init();
    SystemClock_Config();
    hal_time_init();
    hal_uart_init();
    hal_spi_init();
    hal_i2c_init();

    (void)uart_write(banner, sizeof banner - 1u);
    if (pll_default_table_is_placeholder()) {
        /* The LO cannot lock with a placeholder table: app_init() will end in a
         * non-latched FAULT_PLL_LOCK with the RF rails off. */
        (void)uart_write(placeholder, sizeof placeholder - 1u);
    }

    /* Reset cause (spec R4): an IWDG/WWDG reset that fault_panic() did not
     * explain latches FAULT_WATCHDOG (cleared only by a power cycle). The flags
     * are read before the IWDG is started and cleared afterwards (RMVF) so a
     * stale flag cannot latch a later, healthy boot. app_init() applies the
     * e-stop for a latched boot. (No LOCKUP reset flag exists in RCC_CSR on
     * the G0; a lockup ends in the IWDG, which is covered here.) */
    fault_init();
    (void)fault_on_boot(RCC->CSR);
    RCC->CSR |= RCC_CSR_RMVF;

    iwdg_start();               /* app_init() refreshes it between the long stages */
    app_init();
    for (;;) {
        app_loop();
    }
}
