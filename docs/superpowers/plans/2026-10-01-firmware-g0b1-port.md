# Firmware Port to STM32G0B1 (Track B) Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** A C-only firmware tree `9_Firmware/9_1_Microcontroller/g0b1/` that builds from a clean checkout with `make` (arm-none-eabi-gcc), runs on a NUCLEO-G0B1RE, implements the prototype control plane (power sequencing, 1× ADAR1000 + 4× ADTR1107, LO PLL, FPGA DIG0–7 contract, outer-loop AGC, text command interface on USART2) and is covered by host unit tests (`make test`) running in CI.

**Architecture:** Thin HAL abstraction (`hal_spi`, `hal_i2c`, `hal_gpio`, `hal_time`) over the STM32CubeG0 HAL on target and over recording mocks on the host. Drivers (`adar1000`, `pll_lo`, `ads7830`) and application modules (`sequencer`, `fpga_if`, `agc`, `beam`, `cmd`, `fault`, `app`) depend only on the abstraction, never on `stm32g0xx_hal.h`, so every one of them is compiled and tested on the host. `main.c` is the only file that wires target peripherals.

**Tech Stack:** C11, arm-none-eabi-gcc 13.x + newlib-nano, STM32CubeG0 HAL v1.4.7 / CMSIS device G0 v1.4.5 / CMSIS core 5.9.0 (vendored subset), GNU make, host gcc for tests, Python 3 (reference beam table only), GitHub Actions.

**Spec:** `docs/superpowers/specs/2026-10-01-firmware-g0b1-port.md` (read it first). Background: `BOM_OPTIMIZATION_REPORT.md` §2.4 (upstream defects), §5.2.

## Global Constraints

- **C only.** No `.cpp`, no C++ headers, no `double`, no `float` in drivers/app (`um982_gps.c` is the only exception — compiled, not wired). No `printf("%f")`; integer milli-/deci-units only.
- Target flags: `-mcpu=cortex-m0plus -mthumb -Os -std=c11 -Wall -Wextra -Werror -ffunction-sections -fdata-sections --specs=nano.specs`, link with `-Wl,--gc-sections`. Vendored HAL/CMSIS sources may be compiled with `-Wno-unused-parameter` only if they fail `-Wextra` (record which ones in the Makefile).
- Budget: flash ≤ 128 KB, RAM (data+bss+stack+heap reserve) ≤ 32 KB; `make` prints `arm-none-eabi-size`; `make size-check` fails the build if exceeded.
- Drivers and app modules **must not include** any `stm32g0xx*.h`; only `Core/hal/*.c`, `Core/main.c`, `Core/stm32g0xx_it.c`, `Core/system_*.c` and `Core/stm32g0xx_hal_msp.c` may. A grep gate in `make test` enforces it.
- Exactly one delay implementation: `delay_us()` / `delay_ms()` in `hal_time`. No `HAL_Delay` outside `hal_time.c`, no busy `__NOP` loops anywhere else.
- No `HAL_MAX_DELAY` anywhere. I²C timeout 100 ms, SPI timeout 10 ms, UART TX timeout 50 ms.
- `DIAG*` macros compile to nothing with `-DDIAG_DISABLE` (`make DIAG=0`). No `printf`/`DIAG` inside `agc_*`, `fpga_if_*` toggle functions or anything called per chirp.
- ADAR1000 channel indices are **0-based** everywhere (`0..3`); global channel index `g = dev*4 + ch`, `0 .. ADAR_COUNT*4-1`.
- `ADAR_COUNT` is a compile-time constant in `Core/app/config.h`, default `1`; every array sized by it; tests build with `ADAR_COUNT=1` and again with `ADAR_COUNT=4`.
- Install packages only via `sfw` (`sfw uv pip install …`); apt packages for the toolchain are allowed in CI.
- Old F7 tree (`9_1_1_*`, `9_1_2_*`, `9_1_3_*`, `tests/`) is **not modified**; `cd 9_Firmware/9_1_Microcontroller/tests && make` must keep passing.
- Every task: write the failing test first, then the code; `make -C g0b1 test` and `make -C g0b1` both green before the commit. Commit trailers: the ones the session provides.

## Decisions taken while planning (read before starting)

1. **TR switching via the ADAR1000 TR pin, owned by the FPGA.** Per ADAR1000 Rev. B p. 37: `TR_SOURCE` (reg 0x031 bit 2) selects *SPI control (0)* vs *TR pin control (1)*. Upstream wrote `TR_SOURCE=1` believing it meant "TX" — a defect (listed in README). Port: after init the device is in **pin mode** (`TR_SOURCE=1`, `SW_DRV_EN_TR=1`, `0x02E=0x7F`, `0x02F=0x7F`); TX/RX enables then follow the TR pin. Commands `tx` / `rx` force **SPI mode** (`TR_SOURCE=0`, `TR_SPI` and `TX_EN`/`RX_EN` set accordingly) for bench tests; command `auto` returns to pin mode (see open question Q1).
2. **Upstream channel-index defect fixed.** Upstream passes 1-based channels and masks with `& 0x03`, so "channel 1" wrote CH2 registers and "channel 4" wrote CH1. The port is 0-based and has a test that channel 3 hits `0x013` / `0x01A`–`0x01B` / `0x01F` / `0x026`–`0x027`.
3. **Beam math (integer, exact).** Elements are spaced λ/2, so the per-element phase step is `180°·sin θ`. In units of 1/64 phase LSB (LSB = 360°/128) this is `n · 4096 · sin θ`. With `sin` in Q15 from a 91-entry LUT (`round(sin(d°)·32768)`, clamped to 32767), `phase_q6(n) = (n · sin_q15(θ)) >> 3` (arithmetic shift), wrapped mod 8192; register index `= ((phase_q6 + 32) >> 6) & 127` (round-to-nearest; upstream truncated). Angles are integer degrees, `-90..90`. Verified against float math for −45, −20, 0, 10, 30, 60, ±90 (table in Task 6).
4. **`beam <az> <el>` semantics.** As in upstream, the array column is steered electronically in **elevation**; azimuth is mechanical (no motor in this track). `el` drives the ADAR1000 phases and toggles DIG1 `new_elevation` when it changes; `az` is stored, reported in `STATUS` and toggles DIG2 `new_azimuth` when it changes. Range: `az -180..180`, `el -60..60` (grating-lobe-free range for λ/2 is ±90; ±60 is the practical scan limit — see Q2).
5. **Fault latch in RAM-retained `.noinit`** (not TAMP backup registers): survives IWDG/software/NRST reset, cleared by power cycle — the "manual intervention" the upstream comments ask for. Stored as `{magic 0x4641554C, code, ~code}`; any mismatch = not latched. On boot with a valid latch: RF rails stay off, command parser runs, `STATUS` reports `fault=<code> latched=1`, every command except `status` answers `ERR latched`.
6. **Fault classes.** *Latched* (emergency stop + latch): `stop` command, over-temperature (≥ 75.0 °C, upstream threshold), HardFault/`Error_Handler`. *Non-latched* (RF rails off, base rails stay, retried on next boot): PLL lock failure at init or lock loss during run, ADAR1000 scratchpad failure. Thermal sensor read errors are reported (`temp_err=1`) but are not a fault (no PA on this prototype).
7. **Emergency stop order** (spec R4): DIG3 `mixers_enable` low → PA 5 V rail → ADTR LNA 3V3 → ADTR VSS_SW → ADTR VDD_SW → ADAR supply → LO enable → FPGA enable. GPIO-only (no SPI), so it works with a hung bus. `Error_Handler()` and `HardFault_Handler()` run it, set the latch and then spin **without** refreshing IWDG → reset → boot sees the latch and stays safe (fixes upstream "IWDG reset re-energises rails").
8. **Power-up is split in two tables** because the ADTR1107 needs a negative `VGG_PA` (set over SPI through the ADAR1000) before `VDD_PA` is applied: `SEQ_BASE_UP` (FPGA, LO, ADAR, VDD_SW, VSS_SW) → ADAR init + safe bias → `SEQ_RF_UP` (LNA 3V3, then PA 5 V).
9. **AGC frame = 250 ms tick** (upstream frame ≈ 258 ms; the MCU no longer runs the pulse loop). DIG6 2-frame debounce from upstream is kept. Gains written **only for channels whose effective value changed** (cache of last written value per channel, invalidated by `gain` command and by ADAR init).
10. **PLL tables are placeholders.** The vendor tools (ADI ACE, TICS Pro) cannot run here. `pll_tables/adf4372_10500MHz.h` and `lmx2594_10500MHz.h` contain the correct *format* (24-bit words: ADF4372 `addr15<<8 | data8`, LMX2594 `addr7<<16 | data16`, sent MSB-first) and a header comment block, plus `#define PLL_TABLE_PLACEHOLDER 1`. `main.c` logs `PLL table is a placeholder` at boot; the lock check then fails → non-latched fault. Replace with a real export before hardware use (BACKLOG item).
11. **HAL vendoring:** `g0b1/tools/fetch_cube.sh` downloads the three pinned tags as tarballs (`codeload.github.com`), verifies the commit SHAs below, and copies only the files in its allow-list into `g0b1/Drivers/`. The copied files **are committed** (spec: build from a clean checkout); the script documents provenance and allows re-vendoring.
    - `STMicroelectronics/stm32g0xx_hal_driver` tag `v1.4.7` → `a0cf8a8b96183fdcc2e3b1cf0bcf0825f27bd0c9`
    - `STMicroelectronics/cmsis_device_g0` tag `v1.4.5` → `f576c24e123edf3332988ecd49512c0f35f85186`
    - `STMicroelectronics/cmsis_core` tag `v5.9.0_20250520` → `b7487de1303e2eb77c402432e368691e9dcfb5f0`
12. **Test framework:** a 60-line header `tests/tinytest.h` (`TT_ASSERT`, `TT_ASSERT_EQ`, `TT_RUN`), one executable per test file, `make test` runs all and fails on the first non-zero exit. No cpputest dependency.
13. **Old tests stay in place**; the five Gap-3 intents are re-expressed against the new modules in Task 12 (`tests/test_gap3_port.c`).

### Open questions for the owner (defaults applied unless answered)

- **Q1.** `auto` command to return ADAR1000 to FPGA TR-pin mode after `tx`/`rx` — not in the spec's command list. Default: **add it** (otherwise a reboot is the only way back).
- **Q2.** Elevation range `-60..60`. Default: yes.
- **Q3.** `gain <ch> <val>` writes the channel's RX VGA register directly and is overwritten by the AGC on its next change if the FPGA has AGC enabled. Default: yes (simplest; alternative is to store it as `cal_offset`).

## Files

All paths below are relative to `9_Firmware/9_1_Microcontroller/g0b1/`.

```
g0b1/
├── Makefile                  target build, flash, size-check, test (delegates to tests/)
├── README.md
├── STM32G0B1RETx_FLASH.ld
├── startup_stm32g0b1xx.s     from cmsis_device_g0 (gcc variant)
├── tools/fetch_cube.sh
├── Drivers/                  vendored subset (Task 1)
├── Core/
│   ├── main.c                target wiring + superloop (only file with HAL handles besides hal/)
│   ├── stm32g0xx_it.c        SysTick, HardFault → fault_panic()
│   ├── stm32g0xx_hal_conf.h  only used modules enabled
│   ├── stm32g0xx_hal_msp.c
│   ├── system_stm32g0xx.c    from cmsis_device_g0
│   ├── syscalls.c            _write → USART2 (for DIAG/printf), _sbrk with heap limit
│   ├── hal/                  hal_gpio.h hal_spi.h/.c hal_i2c.h/.c hal_time.h/.c hal_uart.h/.c pins.h
│   ├── drivers/              adar1000.h/.c pll_lo.h/.c ads7830.h/.c um982_gps.h/.c pll_tables/*.h
│   └── app/                  config.h diag_log.h fault.h/.c sequencer.h/.c fpga_if.h/.c
│                             beam.h/.c sin_lut.h agc.h/.c cmd.h/.c app.h/.c thermal.h/.c
└── tests/
    ├── Makefile tinytest.h
    ├── mocks/ mock_spi.c mock_i2c.c mock_gpio.c mock_time.c mock_uart.c mock_log.h
    ├── ref_beam_table.py gen_sin_lut.py
    └── test_*.c
```

Interfaces are fixed in each task. Module dependency order: `hal` → `drivers` → `fault`, `sequencer`, `fpga_if`, `beam`, `agc`, `thermal` → `cmd` → `app` → `main.c`.

---

### Task 1: Toolchain, vendored HAL, skeleton that builds

**Files:** create `tools/fetch_cube.sh`, `Drivers/**`, `startup_stm32g0b1xx.s`, `STM32G0B1RETx_FLASH.ld`, `Core/system_stm32g0xx.c`, `Core/stm32g0xx_hal_conf.h`, `Core/stm32g0xx_hal_msp.c`, `Core/stm32g0xx_it.c`, `Core/syscalls.c`, `Core/main.c` (temporary: clock + blink LD4 + "AERIS-10 G0B1 boot\r\n" on USART2), `Makefile`, `.gitignore` (`build/`).

- [ ] **Step 1:** `which arm-none-eabi-gcc || sudo apt-get install -y gcc-arm-none-eabi libnewlib-arm-none-eabi`; record `arm-none-eabi-gcc --version | head -1` in the commit message.
- [ ] **Step 2:** Write `tools/fetch_cube.sh` (bash, `set -euo pipefail`): for each repo in Decision 11 download `https://codeload.github.com/<repo>/tar.gz/<sha>`, extract to a temp dir, copy:
  - HAL `Inc/`: `stm32g0xx_hal.h stm32g0xx_hal_def.h stm32g0xx_hal_cortex.h stm32g0xx_hal_rcc.h stm32g0xx_hal_rcc_ex.h stm32g0xx_hal_gpio.h stm32g0xx_hal_gpio_ex.h stm32g0xx_hal_spi.h stm32g0xx_hal_spi_ex.h stm32g0xx_hal_i2c.h stm32g0xx_hal_i2c_ex.h stm32g0xx_hal_uart.h stm32g0xx_hal_uart_ex.h stm32g0xx_hal_tim.h stm32g0xx_hal_tim_ex.h stm32g0xx_hal_iwdg.h stm32g0xx_hal_pwr.h stm32g0xx_hal_pwr_ex.h stm32g0xx_hal_flash.h stm32g0xx_hal_flash_ex.h stm32g0xx_hal_dma.h stm32g0xx_hal_dma_ex.h stm32g0xx_hal_exti.h stm32g0xx_ll_*.h` that those include (copy what the compiler asks for — iterate until it builds), `Legacy/stm32_hal_legacy.h`, and `LICENSE.md`.
  - HAL `Src/`: the `.c` of each module above except `dma`, `dma_ex`, `exti` unless the linker needs them.
  - CMSIS device: `Include/stm32g0xx.h stm32g0b1xx.h system_stm32g0xx.h`, `Source/Templates/gcc/startup_stm32g0b1xx.s` → `g0b1/`, `Source/Templates/system_stm32g0xx.c` → `Core/`, `LICENSE.md`.
  - CMSIS core: `CMSIS/Core/Include/{core_cm0plus.h,cmsis_compiler.h,cmsis_gcc.h,cmsis_version.h,mpu_armv7.h}` (whichever `core_cm0plus.h` includes), `LICENSE.txt`.
  Verify each tarball's top directory ends with the expected SHA prefix; abort otherwise.
- [ ] **Step 3:** Run it; commit nothing yet. Linker script: FLASH 512K @0x08000000, RAM 144K @0x20000000; sections `.isr_vector .text .rodata .data .bss` + `.noinit (NOLOAD)` placed in RAM **after** `.bss`, not zeroed by startup; `_estack = ORIGIN(RAM)+LENGTH(RAM)`; `_Min_Heap_Size = 0x400`, `_Min_Stack_Size = 0x1000`; symbols `_sheap/_eheap` for `_sbrk`.
- [ ] **Step 4:** Clock: HSI16 → PLL (M=1, N=8, R=2) → SYSCLK 64 MHz, AHB/APB /1, flash latency 2. USART2 PA2/PA3 AF1 115200 8N1. LD4 = PA5.
- [ ] **Step 5:** `Makefile` targets: `all` (→ `build/aeris_g0b1.elf/.bin/.hex`, then `arm-none-eabi-size build/aeris_g0b1.elf`), `size-check` (awk over `size -A`: text+rodata+data ≤ 131072, data+bss+noinit+_Min_Stack_Size+_Min_Heap_Size ≤ 32768; exit 1 otherwise; `all` depends on it), `flash` (`st-flash --reset write build/aeris_g0b1.bin 0x08000000`, or `STM32_Programmer_CLI -c port=SWD -w build/aeris_g0b1.elf -rst` when `PROGRAMMER=cube`), `test` (`$(MAKE) -C tests`), `clean`. Variables: `ADAR_COUNT ?= 1`, `DIAG ?= 1` (`DIAG=0` → `-DDIAG_DISABLE`). Defines `-DSTM32G0B1xx -DUSE_HAL_DRIVER -DADAR_COUNT=$(ADAR_COUNT)`.
- [ ] **Step 6:** `make clean && make` → ELF, size printed, size-check passes. `make DIAG=0` also builds.
- [ ] **Step 7:** Commit (`fw(g0b1): vendored STM32CubeG0 subset, startup, linker script, Makefile skeleton`).

### Task 2: Host test harness and HAL abstraction headers + mocks

**Files:** `Core/hal/{pins.h,hal_gpio.h,hal_spi.h,hal_i2c.h,hal_time.h,hal_uart.h}`, `tests/{Makefile,tinytest.h}`, `tests/mocks/*`, `tests/test_mocks.c`, `tests/check_no_hal_includes.sh`.

Interfaces (exact):

```c
/* hal_gpio.h */
typedef enum { PIN_LED, PIN_ADAR_CS0, PIN_ADAR_CS1, PIN_ADAR_CS2, PIN_ADAR_CS3, PIN_PLL_CS, PIN_PLL_CE,
               PIN_PLL_LD, PIN_EN_FPGA, PIN_EN_LO, PIN_EN_ADAR, PIN_EN_ADTR_VDD_SW, PIN_EN_ADTR_VSS_SW,
               PIN_EN_LNA, PIN_EN_PA, PIN_FPGA_DIG0, /* … */ PIN_FPGA_DIG7, PIN_COUNT } gpio_t;
void gpio_write(gpio_t pin, int level);
int  gpio_read(gpio_t pin);
void gpio_toggle(gpio_t pin);
uint8_t gpio_read_fpga_port(void);     /* DIG0..7 as bits 0..7, one IDR read */

/* hal_spi.h */
typedef enum { SPI_BUS_ADAR, SPI_BUS_PLL, SPI_BUS_COUNT } spi_bus_t;
int spi_xfer(spi_bus_t bus, gpio_t cs, const uint8_t *tx, uint8_t *rx, size_t n); /* 0 or -EIO/-ETIMEDOUT; rx may be NULL */
uint32_t spi_prescaler_for(uint32_t pclk_hz, uint32_t max_hz);   /* pure: smallest 2^k (2..256) with pclk/2^k <= max_hz */

/* hal_i2c.h */
int i2c_write(uint8_t addr7, const uint8_t *buf, size_t n);      /* 100 ms timeout, 0 / -EIO / -ETIMEDOUT */
int i2c_read(uint8_t addr7, uint8_t *buf, size_t n);

/* hal_time.h */
uint32_t millis(void);  uint32_t micros(void);
void delay_us(uint32_t us);  void delay_ms(uint32_t ms);  /* delay_ms refreshes nothing; callers keep loops < 1 s */

/* hal_uart.h */
int uart_write(const char *s, size_t n);   /* blocking, 50 ms timeout */
int uart_getc(void);                        /* -1 if no byte (RX ring buffer filled by IRQ) */
```

Error codes: `#include <errno.h>`, return negative `-EIO`, `-ETIMEDOUT`, `-EINVAL`.

`pins.h` (MCU pin → role; verify AFs against the STM32G0B1 datasheet *Alternate function* tables and the Nucleo connector against UM2324 before finalising the README wiring table):

| Role | Pin | Notes |
|---|---|---|
| ADAR1000 SPI2 SCK/MISO/MOSI | PB13 / PB14 / PB15 (AF0) | ≤ 20 MHz → 16 MHz (64 MHz / 4) |
| ADAR CS0..CS3 | PB12, PB11, PB10, PB2 | only CS0 wired when `ADAR_COUNT=1` |
| PLL SPI1 SCK/MISO/MOSI | PB3 / PB4 / PB5 (AF0) | ≤ 10 MHz → 8 MHz (64 MHz / 8) |
| PLL CS / CE / LD | PB1 / PB0 (out) / PB6 (in) | |
| I2C1 SCL / SDA (ADS7830) | PB8 / PB9 (AF6) | 100 kHz |
| USART2 TX / RX (ST-LINK VCP) | PA2 / PA3 (AF1) | 115200 8N1 |
| LED LD4 | PA5 | |
| EN_FPGA, EN_LO, EN_ADAR | PA0, PA1, PA4 | active high |
| EN_ADTR_VDD_SW, EN_ADTR_VSS_SW | PA6, PA7 | |
| EN_LNA (ADTR 3V3), EN_PA (5 V) | PA8, PA9 | |
| FPGA DIG0 new_chirp, DIG1 new_elevation, DIG2 new_azimuth, DIG3 mixers_enable, DIG4 fpga_reset_n | PC0..PC4 (out) | same roles as upstream PD8..PD12 |
| FPGA DIG5 agc_saturation, DIG6 agc_enable, DIG7 reserved | PC5..PC7 (in) | same roles as upstream PD13..PD15 |

Mocks record every call into a global event log `mock_log` (array of `{kind, a, b, c, bytes[8], n}`, cap 4096) so tests can assert exact ordering across GPIO/SPI/I²C/time. `mock_spi_set_rx(…)` queues MISO bytes; `mock_i2c_fail_next(int err)`; `mock_time_advance_us()`; `mock_gpio_set_input(pin, level)`.

`tests/check_no_hal_includes.sh`: fails if `grep -lE '#include\s*"stm32g0' Core/drivers Core/app` finds anything. `tests/Makefile` runs it first, then builds each `test_*.c` with `-std=c11 -Wall -Wextra -Werror -I../Core/hal -I../Core/drivers -I../Core/app -Imocks -DHOST_TEST -DADAR_COUNT=$(ADAR_COUNT)`, runs all; `make test` at top runs with `ADAR_COUNT=1` and then `ADAR_COUNT=4`.

- [ ] **Step 1:** `tests/test_mocks.c`: assert `spi_prescaler_for(64000000, 20000000) == 4`, `(…, 10000000) == 8`, `(…, 100000000) == 2`, `(…, 100000) == 256` (clamped), and that the mock log records `gpio_write`, `spi_xfer` bytes, `i2c_write` in order. Run → fails (nothing exists).
- [ ] **Step 2:** Implement headers, `spi_prescaler_for` (in `hal_spi_common.c`, shared by target and host), mocks, tinytest, Makefiles. `make test` passes.
- [ ] **Step 3:** Commit.

### Task 3: Target HAL implementations

**Files:** `Core/hal/{hal_gpio.c,hal_spi.c,hal_i2c.c,hal_time.c,hal_uart.c}`, `Core/main.c` (init calls), `Core/stm32g0xx_hal_msp.c`, `Core/stm32g0xx_it.c` (USART2 IRQ → RX ring 128 B).

- `hal_time`: TIM2 (32-bit) prescaler 63 → 1 MHz free-running; `micros()` = `TIM2->CNT`; `millis()` = `HAL_GetTick()`; `delay_us` busy-waits on `micros()` with wrap-safe subtraction; `delay_ms(ms)` loops `delay_us(1000)`. This is the **only** delay implementation.
- `hal_spi`: SPI mode 0, MSB first, 8-bit, software CS (`SPI_NSS_SOFT`); prescaler from `spi_prescaler_for(HAL_RCC_GetPCLK1Freq(), max_hz)` with `max_hz` = 20 MHz (ADAR) / 10 MHz (PLL) from `config.h`; CS low → `HAL_SPI_TransmitReceive`/`Transmit` with 10 ms timeout → CS high; map `HAL_TIMEOUT` → `-ETIMEDOUT`, other errors → `-EIO`.
- `hal_i2c`: I2C1 100 kHz (timing from CubeMX for 64 MHz: `0x10B17DB5`, verify), `HAL_I2C_Master_Transmit/Receive(addr7<<1, …, 100)`.
- `hal_gpio`: table `{GPIO_TypeDef *port; uint16_t pin;}` indexed by `gpio_t`, filled from `pins.h`; `gpio_read_fpga_port()` = `(uint8_t)(GPIOC->IDR & 0xFF)`. Outputs init **low** (all rails off, `fpga_reset_n` low, mixers off) before any other init.
- [ ] **Step 1:** `make` builds; manual smoke (documented in README, not CI): boot banner on VCP, LD4 blinks at 1 Hz using `millis()`.
- [ ] **Step 2:** Commit.

### Task 4: `diag_log.h`, `config.h`, `fault` (latch + panic)

**Files:** `Core/app/{diag_log.h,config.h,fault.h,fault.c}`, `tests/test_fault.c`.

```c
/* fault.h */
typedef enum { FAULT_NONE=0, FAULT_PLL_LOCK=1, FAULT_ADAR_COMM=2, FAULT_OVERTEMP=10,
               FAULT_ESTOP_CMD=11, FAULT_PANIC=12 } fault_t;     /* >= 10 are latched */
void    fault_init(void);              /* reads .noinit latch; validates magic/~code */
int     fault_is_latched(void);
fault_t fault_latched_code(void);
fault_t fault_active(void);            /* latched code, else current non-latched code, else NONE */
void    fault_raise(fault_t f);        /* latched codes: sequencer_emergency_stop() then latch; others: sequencer_rf_off() */
void    fault_clear_nonlatched(void);
void    fault_panic(void);             /* Error_Handler/HardFault: e-stop (GPIO only), latch FAULT_PANIC, spin without IWDG refresh */
```

The latch variable: `__attribute__((section(".noinit"))) static volatile struct { uint32_t magic, code, ncode; } s_latch;` (host build: plain static, plus `fault_test_simulate_reset()` that re-runs `fault_init()` without clearing it).

`diag_log.h`: port of upstream macros, `HAL_GetTick()` → `millis()`, `-DDIAG_DISABLE` → `((void)0)`; fix upstream's `DIAG_GPIO` arity (takes `(subsys, name, gpio_t pin)`). `config.h`: `ADAR_COUNT` default 1 (`#ifndef`), SPI max Hz, `OVERTEMP_DECI_C 750`, `THERMAL_PERIOD_MS 5000`, `AGC_PERIOD_MS 250`, `IWDG_PRESCALER_DIV 256`, `IWDG_RELOAD 500` (LSI 32 kHz → 4.0 s), `PLL_LOCK_TIMEOUT_MS 100`.

- [ ] **Step 1:** Tests (fail first): latch survives simulated reset; corrupted `ncode` → not latched; `fault_raise(FAULT_OVERTEMP)` latches and calls `sequencer_emergency_stop` (use a weak stub/spy in the test); `fault_raise(FAULT_PLL_LOCK)` does **not** latch and calls `sequencer_rf_off`; latched code wins over a later non-latched one.
- [ ] **Step 2:** Implement; `make test` and `make` green. Commit.

### Task 5: `adar1000` driver

**Files:** `Core/drivers/{adar1000.h,adar1000.c}`, `tests/test_adar1000.c`.

Port of `ADAR1000_Manager.cpp` register layer to C. Keep register defines and the `VM_I[128]`/`VM_Q[128]` tables verbatim (with their provenance comment).

```c
typedef enum { ADAR_MODE_TR_PIN, ADAR_MODE_SPI_TX, ADAR_MODE_SPI_RX } adar_mode_t;
int  adar_write(uint8_t dev, uint16_t reg, uint8_t val);      /* 3 bytes: (addr2<<5)|(reg>>8 & 0x1F), reg&0xFF, val */
int  adar_read(uint8_t dev, uint16_t reg, uint8_t *val);      /* SDO active before, inactive after (upstream sequence) */
int  adar_init(uint8_t dev);           /* soft reset 0x000=0x81, 10 ms, ConfigA SDO_ACTIVE, RAM bypass, ADC 2 MHz+EN,
                                          scratchpad 0xA5 readback → -EIO on mismatch (upstream ignored it: defect) */
int  adar_set_safe_bias(uint8_t dev);  /* LNA bias ON/OFF = 0x00, PA bias ON = 0x5D (kPaBiasTxSafe), load working */
int  adar_set_operational_bias(uint8_t dev); /* PA_BIAS_ON = 0x7F? NO — see note */
int  adar_set_rx_gain(uint8_t dev, uint8_t ch, uint8_t gain);  /* 0x010+ch, then LOAD_WORKING=0x01 */
int  adar_set_tx_gain(uint8_t dev, uint8_t ch, uint8_t gain);  /* 0x01C+ch, then LOAD_WORKING=0x02 (LDTX override) */
int  adar_set_rx_phase(uint8_t dev, uint8_t ch, uint8_t idx);  /* 0x014+2ch / 0x015+2ch from VM tables, LOAD_WORKING=0x01 */
int  adar_set_tx_phase(uint8_t dev, uint8_t ch, uint8_t idx);  /* 0x020+2ch / 0x021+2ch, LOAD_WORKING=0x02 */
int  adar_set_mode(uint8_t dev, adar_mode_t m);
int  adar_read_temp_raw(uint8_t dev, uint8_t *raw);            /* ADC start 0x70, poll bit0 ≤ 100 ms → -ETIMEDOUT */
```

Bias note: keep upstream constants (`kLnaBiasOff 0x00`, `kLnaBiasOperational 0x30`, `kPaBiasTxSafe 0x5D`, `kPaBiasRxSafe 0x20`, `kPaBiasOperational 0x7F`, `kTxBiasCurrent 0x2D`, `kTxDriverBiasCurrent 0x06`) as named `#define`s; `adar_set_operational_bias` writes `PA_CHx_BIAS_ON = kPaBiasOperational`, `PA_CHx_BIAS_OFF = kPaBiasRxSafe`, `LNA_BIAS_ON = kLnaBiasOperational`, `LNA_BIAS_OFF = kLnaBiasOff`, TX bias currents, and `MISC_ENABLES` with `BIAS_CTRL` + `LNA_BIAS_OUT_EN` so the ON/OFF pairs follow TR. The implementer **must** confirm the `0x030` and `0x031` bit positions against `7_Components Datasheets and Application notes/ADAR1000.pdf` (Register map, Tables "Register 0x030"/"0x031") and the ADTR1107 CTRL_SW polarity (`adtr1107.pdf`), and cite the page numbers in a comment.

`adar_set_mode`: `TR_PIN` → `0x02E=0x7F`, `0x02F=0x7F`, `0x031 = TR_SOURCE(1<<2) | SW_DRV_EN_TR(1<<4)`; `SPI_TX` → `0x031 = SW_DRV_EN_TR | TX_EN(1<<6) | TR_SPI(1<<1)`; `SPI_RX` → `0x031 = SW_DRV_EN_TR | RX_EN(1<<5)`. Single register write each — no read-modify-write (upstream did 2 SPI reads per bit).

- [ ] **Step 1 (tests first):** instruction encoding for dev 0 and dev 3 (address bits), reg 0x0FF and 0x400 high bits; channel 3 RX gain hits `0x013`, RX phase hits `0x01A/0x01B` with `VM_I[idx]/VM_Q[idx]`, TX phase `0x026/0x027`; LOAD_WORKING values; `adar_init` on scratchpad mismatch returns `-EIO`; temp read times out after 100 ms of mock time; `adar_set_mode(TR_PIN)` emits exactly the three writes above; out-of-range `dev >= ADAR_COUNT` or `ch > 3` → `-EINVAL` with no SPI traffic.
- [ ] **Step 2:** Implement; green; commit.

### Task 6: `beam` (integer beam tables) + Python reference

**Files:** `Core/app/{beam.h,beam.c,sin_lut.h}`, `tests/{gen_sin_lut.py,ref_beam_table.py,test_beam.c}`.

```c
int beam_phase_indices(int el_deg, uint8_t idx[ADAR_COUNT*4]);  /* -EINVAL outside -90..90 */
int beam_apply(int el_deg);  /* computes idx[], writes RX and TX phases for every dev/ch; returns first error */
```

`sin_lut.h` is generated by `gen_sin_lut.py` (`round(sin(d)*32768)` clamped to 32767, `d = 0..90`, `static const int16_t SIN_Q15[91]`), checked in; a test re-runs the generator and diffs (skipped with a printed SKIP only if `python3` is absent — CI has it). Element `n = dev*4 + ch`; algorithm exactly as Decision 3.

`ref_beam_table.py` prints the float reference `round(n*180*sin(θ)/2.8125) mod 128` for n = 0..15. Expected (n = 0..3), already cross-checked:

| el | idx |
|---|---|
| −45 | 0, 83, 37, 120 |
| −20 | 0, 106, 84, 62 |
| 0 | 0, 0, 0, 0 |
| 10 | 0, 11, 22, 33 |
| 30 | 0, 32, 64, 96 |
| 60 | 0, 55, 111, 38 |
| ±90 | 0, 64, 0, 64 |

- [ ] **Step 1:** `test_beam.c` hard-codes this table (and, when `ADAR_COUNT=4`, n = 0..15 from `ref_beam_table.py` output pasted into the test) and a test that runs `python3 ref_beam_table.py` and compares all 16 elements for the 5 spec angles (−45, −20, 10, 30, 60). Fails first.
- [ ] **Step 2:** Implement; green; commit.

### Task 7: `pll_lo` + example tables

**Files:** `Core/drivers/{pll_lo.h,pll_lo.c}`, `Core/drivers/pll_tables/{adf4372_10500MHz.h,lmx2594_10500MHz.h}`, `tests/test_pll_lo.c`.

```c
typedef struct { const uint32_t *words; uint16_t count; uint16_t settle_ms; const char *name; } pll_regs_t;
int pll_init(const pll_regs_t *t);   /* CE high, 1 ms, each word → 3 bytes MSB-first on SPI_BUS_PLL/PIN_PLL_CS, settle_ms */
int pll_is_locked(void);             /* gpio_read(PIN_PLL_LD) */
int pll_wait_lock(uint32_t timeout_ms); /* polls every 1 ms, 0 or -ETIMEDOUT */
void pll_power_down(void);           /* CE low */
```

Tables per Decision 10 (format + placeholder flag + header documenting: reference clock assumed 100 MHz, target 10.5 GHz, which tool/version to export with, LMX2594 programming order R112→R0 then R0 with FCAL_EN, ADF4372 order per datasheet). Selected by `#define PLL_PART_LMX2594` / `PLL_PART_ADF4372` in `config.h` (default LMX2594).

- [ ] **Step 1 (tests first):** word `0x123456` → bytes `12 34 56` within one CS assertion; all words sent in table order; CE asserted before first byte; `pll_wait_lock` succeeds when the mock LD goes high at 40 ms, fails at 100 ms; SPI error aborts with the error code.
- [ ] **Step 2:** Implement; green; commit.

### Task 8: `ads7830` + `thermal`

**Files:** `Core/drivers/{ads7830.h,ads7830.c}`, `Core/app/{thermal.h,thermal.c}`, `tests/test_ads7830.c`, `tests/test_thermal.c`.

```c
int ads7830_read_se(uint8_t addr7, uint8_t ch, uint8_t pd_bits);  /* 0..255, or negative errno — never 0xFF on error */
int thermal_read_deci_c(int16_t *deci_c);  /* ADS7830 @0x48 ch0, internal ref 2.5 V: mv = raw*2500/255;
                                              TMP37 20 mV/°C: deci_c = mv*10/20 (integer, rounded) */
void thermal_tick(void);   /* every THERMAL_PERIOD_MS: read; ≥ OVERTEMP_DECI_C → fault_raise(FAULT_OVERTEMP) */
int16_t thermal_last(void); int thermal_last_err(void);
```

Command byte = `SD_SINGLE(0x80) | ((ch>>1)|((ch&1)<<2))<<4 | pd_bits` — note the ADS7830 single-ended channel select is **not** `ch<<4` (datasheet Table 2: C2 C1 C0 = ch0→000, ch1→100, ch2→001, …). Upstream used `ch<<4` — verify against `ads7830.pdf` and, if confirmed, list as an upstream defect. Upstream also stored raw counts as "°C" — defect, listed.

- [ ] **Step 1 (tests first):** command byte for ch 0..7; I²C write error → `-EIO` and no read issued; read timeout → `-ETIMEDOUT`; raw 0xFF with OK status returns 255 (not an error); conversion: raw 0 → 0, raw 76 → 373 (745 mV → 37.3 °C), raw 153 → 750 → overtemp fault raised; thermal timer does not fire before 5 s after boot (cold-start, Gap-3 intent).
- [ ] **Step 2:** Implement; green; commit.

### Task 9: `sequencer`

**Files:** `Core/app/{sequencer.h,sequencer.c}`, `tests/test_sequencer.c`.

```c
typedef struct { gpio_t pin; uint8_t level; uint16_t delay_ms; } seq_step_t;
extern const seq_step_t SEQ_BASE_UP[], SEQ_RF_UP[], SEQ_DOWN[], SEQ_ESTOP[];
void sequencer_run(const seq_step_t *steps, size_t n);   /* write, then delay_ms */
int  sequencer_power_up(void);   /* fault latched → -EPERM, nothing written */
void sequencer_rf_off(void);     /* DIG3 low, EN_PA low, EN_LNA low (no delays > 1 ms) */
void sequencer_power_down(void); /* SEQ_DOWN */
void sequencer_emergency_stop(void);  /* SEQ_ESTOP, all delays 0, GPIO only */
```

Tables:
- `SEQ_BASE_UP`: EN_FPGA→1 (100 ms), EN_LO→1 (10), EN_ADAR→1 (500, upstream), EN_ADTR_VDD_SW→1 (1), EN_ADTR_VSS_SW→1 (1).
- `SEQ_RF_UP`: EN_LNA→1 (2), EN_PA→1 (50).
- `SEQ_DOWN`: DIG3→0 (0), EN_PA→0 (10), EN_LNA→0 (10), EN_ADTR_VSS_SW→0 (1), EN_ADTR_VDD_SW→0 (1), EN_ADAR→0 (10), EN_LO→0 (10), DIG4→0 (0), EN_FPGA→0 (0).
- `SEQ_ESTOP`: same order as `SEQ_DOWN`, delays 0.

- [ ] **Step 1 (tests first):** exact GPIO order and delays of each table from the mock log; `sequencer_power_up` with a latched fault writes nothing; e-stop issues no SPI/I²C traffic; e-stop order = Decision 7; `ADAR_COUNT` irrelevant here.
- [ ] **Step 2:** Implement; green; commit.

### Task 10: `fpga_if`

**Files:** `Core/app/{fpga_if.h,fpga_if.c}`, `tests/test_fpga_if.c`.

```c
void fpga_if_init(void);                 /* DIG0..4 low */
void fpga_if_reset_pulse(void);          /* DIG4 low, 10 ms, high (upstream) */
void fpga_if_set_mixers(int on);         /* DIG3 */
void fpga_if_toggle_chirp(void);         /* DIG0 */
void fpga_if_toggle_elevation(void);     /* DIG1 */
void fpga_if_toggle_azimuth(void);       /* DIG2 */
typedef struct { uint8_t saturation, agc_enable; } fpga_status_t;
fpga_status_t fpga_if_sample(void);      /* one gpio_read_fpga_port() → bits 5,6 */
int fpga_if_agc_enable_debounced(uint8_t dig6_now);  /* upstream 2-frame rule: change state only if now == prev */
```

- [ ] **Step 1 (tests first):** reset pulse timing; toggles flip only their own bit; `fpga_if_sample` performs exactly one port read; debounce: sequence 0,1,0,1,1 → enable stays 0 until the 5th sample.
- [ ] **Step 2:** Implement; green; commit.

### Task 11: `agc`

**Files:** `Core/app/{agc.h,agc.c}`, `tests/test_agc.c`.

C port of `ADAR1000_AGC` (defaults: base 30, step_down 4, step_up 1, min 0, max 127, holdoff 4, disabled), arrays sized `ADAR_COUNT*4`:

```c
typedef struct { uint8_t base, step_down, step_up, min_gain, max_gain, holdoff_frames, enabled;
                 uint8_t holdoff_counter, last_saturated; uint32_t sat_events;
                 int8_t cal_offset[ADAR_COUNT*4]; int16_t written[ADAR_COUNT*4]; /* -1 = unknown */ } agc_t;
void agc_init(agc_t *a);
int  agc_update(agc_t *a, int saturated);       /* returns 1 if base changed */
uint8_t agc_effective(const agc_t *a, uint8_t g);
int  agc_apply(agc_t *a);                        /* writes only channels where effective != written; returns #writes or -err */
void agc_invalidate(agc_t *a);                   /* written[] = -1 */
void agc_tick(agc_t *a);                         /* every AGC_PERIOD_MS: sample FPGA, debounce DIG6, update, apply */
```

- [ ] **Step 1 (tests first):** saturation → base −4 (floor at min); 4 clear frames → +1 (ceiling at max); no SPI write when base unchanged (`agc_apply` returns 0, mock log has no SPI); after `agc_invalidate` all channels are written once; `cal_offset` clamping; disabled → no update, no writes.
- [ ] **Step 2:** Implement; green; commit.

### Task 12: `cmd` parser + Gap-3 ports

**Files:** `Core/app/{cmd.h,cmd.c}`, `tests/test_cmd.c`, `tests/test_gap3_port.c`.

Line-oriented, `\r`/`\n` terminated, max 63 chars (longer → `ERR too_long`, line discarded), case-sensitive, tokens split on spaces, integers parsed with a hand-written `parse_int` (no `atoi`/`strtol` silently accepting junk: `"12x"` is an error).

| Command | Effect | Reply |
|---|---|---|
| `beam <az> <el>` | `beam_apply(el)`; toggles DIG1/DIG2 if changed | `OK` / `ERR range` / `ERR spi` |
| `gain <ch> <val>` | `ch 0..ADAR_COUNT*4-1`, `val 0..127` → `adar_set_rx_gain`, `agc.written[ch]=val` | `OK` / `ERR range` |
| `tx` / `rx` | `adar_set_mode(SPI_TX/SPI_RX)` on all devices | `OK` |
| `auto` (Q1) | `adar_set_mode(TR_PIN)` | `OK` |
| `status` | `STATUS lock=%d temp=%d temp_err=%d fault=%d latched=%d mode=%s az=%d el=%d agc=%d base=%u gains=g0,g1,…` | the line |
| `stop` | `fault_raise(FAULT_ESTOP_CMD)` | `OK stopped` |
| anything else | — | `ERR unknown` |

When latched, everything except `status` → `ERR latched`. `cmd_feed(int c)` accumulates; `cmd_exec(const char *line, char *out, size_t outlen)` is pure-ish (calls modules) and is what tests drive. Status formatting with a tiny integer formatter (no `printf` dependency in `cmd.c`, so the status path is printf-free).

Gap-3 ports (`test_gap3_port.c`), one test each:
1. *Emergency stop cuts rails* → `sequencer_emergency_stop` order (Decision 7).
2. *Emergency state set before stop / latch* → after `fault_raise(FAULT_ESTOP_CMD)` the latch is valid and `sequencer_power_up()` refuses (−EPERM) after simulated reset.
3. *IWDG config* → `IWDG_PRESCALER_DIV*IWDG_RELOAD*1000/32000 == 4000` ms and `app_loop` refreshes the watchdog every iteration (spy).
4. *Over-temperature → emergency stop* → thermal 75.0 °C raises latched fault.
5. *Health-watchdog cold start* → thermal/AGC timers initialised to `millis()` at `app_init`, so no spurious tick at t = 0 even when `millis()` starts large.
(Upstream *IDQ periodic re-read* and *max of 8 sensors* do not apply: no Idq channels, one sensor — noted in README.)

- [ ] **Step 1:** Tests first: each command valid + each invalid form (missing args, extra args, out of range, junk digits, too long), latched behaviour, status line exact text for a mocked state. Gap-3 tests.
- [ ] **Step 2:** Implement; green; commit.

### Task 13: `app` + `main.c` wiring, `um982_gps` compiled

**Files:** `Core/app/{app.h,app.c}`, `Core/main.c`, `Core/stm32g0xx_it.c`, `Core/drivers/{um982_gps.h,um982_gps.c}`, `tests/test_app.c`.

`app_init()` (host-testable; `main.c` calls it after clocks/peripherals/IWDG):
1. `fault_init()`; if latched → `sequencer_emergency_stop()` (ensure everything off), print `BOOT latched fault=<n>`, return.
2. `fpga_if_init()`; `sequencer_run(SEQ_BASE_UP)`.
3. `pll_init(&PLL_TABLE)`; `pll_wait_lock(PLL_LOCK_TIMEOUT_MS)` fail → `fault_raise(FAULT_PLL_LOCK)`, return.
4. for each dev: `adar_init` fail → `fault_raise(FAULT_ADAR_COMM)`, return; `adar_set_safe_bias`.
5. `sequencer_run(SEQ_RF_UP)`; for each dev `adar_set_operational_bias`, `adar_set_mode(TR_PIN)`; `beam_apply(0)`; `agc_init`.
6. `fpga_if_reset_pulse()`; `fpga_if_set_mixers(1)`; timers = `millis()`.

`app_loop()`: drain `uart_getc()` → `cmd_feed`; every `AGC_PERIOD_MS` → `agc_tick`; every `THERMAL_PERIOD_MS` → `thermal_tick`; every 100 ms if no fault → `pll_is_locked()` else `fault_raise(FAULT_PLL_LOCK)`; `iwdg_refresh()` (weak hook in `hal_time`, real IWDG in `main.c`). Non-blocking: no call in the loop may block > 150 ms.

`main.c`: `HAL_Init`, clock, GPIO (all outputs low first), USART2+IRQ, SPI1/2, I2C1, TIM2, IWDG (prescaler 256, reload 500), `app_init()`, `for(;;) app_loop();`. `Error_Handler()` and `HardFault_Handler()` → `fault_panic()`.

`um982_gps.c/.h`: copy from `9_1_3_C_Cpp_Code/`, replace `stm32f7xx_hal.h` with `stm32g0xx_hal.h`, fix only what `-Wall -Wextra -Werror` rejects; add to the build, not called. (It is the float exception; it is under `Core/drivers/` but allowed the HAL include — add it to the grep-gate allow-list.)

- [ ] **Step 1 (tests first, `test_app.c`):** happy-path init order (mock log: base rails → PLL words → lock poll → ADAR init/scratchpad → safe bias → RF rails → operational bias → mode → beam → FPGA reset → mixers on); PLL lock timeout → RF rails never enabled, fault 1 reported; latched boot → no SPI traffic and `ERR latched` to `tx`; loop refreshes IWDG every call.
- [ ] **Step 2:** Implement; `make test`, `make`, `make DIAG=0`, `make ADAR_COUNT=4` all green; `make size-check` passes. Commit.

### Task 14: README, CI, final gates

**Files:** `g0b1/README.md`, `.github/workflows/ci-tests.yml` (add job `mcu-g0b1`), `BACKLOG.md` (repo root; create if absent).

README sections: build/flash/test commands; toolchain versions; Nucleo wiring table (every signal: ADAR1000 board SPI/CS/supplies, PLL eval SPI/CS/CE/LD, FPGA DIG0–7, ADS7830 I²C/address, rail enables, VCP) with MCU pin, Nucleo connector pin (CN7/CN10 Morpho, from UM2324) and direction; command reference (from Task 12); memory report (paste `arm-none-eabi-size` output, `ADAR_COUNT=1`); fault model (Decisions 5–7); upstream defects fixed, item by item against `BOM_OPTIMIZATION_REPORT.md` §2.4 plus those found here (TR_SOURCE misuse, 1-based channel masking, scratchpad failure ignored, ADS7830 0xFF-as-error, raw counts as °C, ADS7830 channel encoding if confirmed, SPI at 36 MHz, `HAL_MAX_DELAY`, Error_Handler re-energising rails, 4 delay implementations); Gap-3 mapping; placeholder PLL tables warning.

CI job (`ubuntu-latest`): `sudo apt-get install -y gcc-arm-none-eabi libnewlib-arm-none-eabi`, then in `9_Firmware/9_1_Microcontroller/g0b1`: `make test`, `make`, `make DIAG=0 clean all`, `make ADAR_COUNT=4 clean all`. Keep the existing `mcu-tests` job unchanged. **Coordinate with the RTL track's Task 12, which also edits this workflow — apply this change after it, on top of its version.**

Final gates (record outputs in the commit message):
- [ ] `make clean && make` → ELF, size within budget.
- [ ] `make test` → all pass for `ADAR_COUNT=1` and `4`.
- [ ] `grep -rn "printf" Core/app/agc.c Core/app/fpga_if.c Core/app/cmd.c` → empty (spec acceptance: no printf in pulse/chirp/agc paths).
- [ ] `grep -rn "HAL_MAX_DELAY\|HAL_Delay" Core --include=*.c | grep -v hal_time.c` → empty.
- [ ] `cd ../tests && make` (old F7 tests) → still pass.
- [ ] BACKLOG entries: real PLL register exports; USB-CDC on PA11/PA12; GPS/IMU wiring; GUI parser for `STATUS`; hardware bring-up checklist.
- [ ] Commit.

## Self-review against the spec

| Spec item | Task |
|---|---|
| R1 layout, C only, make/flash/test, flags, budget, old tree untouched | 1, 2, 13, 14 |
| R2 carry-over list / do-not-copy list | 4 (diag_log), 5 (ADAR), 7 (PLL ref only), 8 (ADS7830), 11 (AGC), 12 (settings → cmd; `RadarSettings` binary USB packet is not ported: no USB in this track — noted in README), 13 (um982) |
| R3 hal_spi per-device speed, hal_i2c 100 ms, pins.h one table, DIG on one port, one delay | 2, 3 |
| R4 adar1000 (no per-pulse SPI TR, scratchpad, integer beam), pll_lo + 2 tables, ads7830 negative errors, agc writes on change, sequencer + e-stop + no re-enable after reset, fpga_if | 5, 6, 7, 8, 9, 10, 11, 4 |
| R5 main flow, commands, AGC/thermal/IWDG, DIAG_DISABLE, STATUS line | 12, 13 |
| R6 mocks, tests incl. Python beam ref for 5 angles, AGC, sequencer, parser, ADS7830 error, Gap-3 ports, CI | 2, 6, 8, 9, 11, 12, 14 |
| R7 README | 14 |

High-risk items for the final review: fault latch + e-stop ordering + `Error_Handler`/HardFault path (Tasks 4, 9, 13); ADAR1000 register bit positions for TR/bias (Task 5); FPGA DIG0–7 contract vs `radar_system_top.v` (Task 10); integer overflow in beam/thermal math (Tasks 6, 8).
