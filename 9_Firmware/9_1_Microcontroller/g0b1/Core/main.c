/* Bring-up main: 64 MHz clock, HAL init, boot banner, LD4 blink at 1 Hz.
 * The application sequencer replaces the loop in a later task. */
#include "stm32g0xx_hal.h"
#include "hal_gpio.h"
#include "hal_init.h"
#include "hal_time.h"
#include "hal_uart.h"
#include "fault.h"

void Error_Handler(void)
{
    fault_panic();   /* e-stop, latch, spin without IWDG refresh */
    for (;;) { }     /* fault_panic() does not return on target */
}

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
    static const char banner[] = "AERIS-10 G0B1 boot\r\n";

    hal_gpio_init();            /* all rails/enables/resets low before anything else */
    HAL_Init();
    SystemClock_Config();
    hal_time_init();
    hal_uart_init();
    hal_spi_init();
    hal_i2c_init();

    (void)uart_write(banner, sizeof banner - 1u);

    uint32_t last = millis();
    for (;;) {
        if ((uint32_t)(millis() - last) >= 500u) {
            last += 500u;
            gpio_toggle(PIN_LED);
        }
    }
}
