# Spec: Microcontroller firmware clean-up and port to STM32G0B1 (track B)

Date: 2026-10-01. Owner: Andrii. Status: approved for planning.

## Context

Upstream firmware (`9_Firmware/9_1_Microcontroller/`) targets an STM32F746ZGT7, does not
build as shipped (no linker script/startup/project, a macro-arity error, case-mismatched
includes, missing CubeMX USB files), and carries ~21 k lines of unused vendored code.
Details: `BOM_OPTIMIZATION_REPORT.md` section 2.

The prototype MCU is a **NUCLEO-G0B1RE** (STM32G0B1RET6: Cortex-M0+ 64 MHz, no FPU,
512 KB flash, 144 KB RAM, USB-FS device, 3× SPI, 3× I²C, 6× USART). The prototype has
**1× ADAR1000 + 4× ADTR1107**, one LO PLL (ADF4372 or LMX2594 eval board — driver to be
written against a thin SPI abstraction so either fits), no GaN PA, therefore **no Idq loop,
no DAC5578, no INA241, no ADS7830 current channels**. Thermal monitoring: one ADS7830 (or
MCU internal ADC) — keep the driver. GPS/IMU/barometer: out of scope for this track
(keep the UM982 parser compiling but not wired).

## Goal

A firmware tree that builds from a clean checkout with a single command
(`make` with arm-none-eabi-gcc, CMake optional), runs on the Nucleo-G0B1RE, and
implements the prototype's control plane: power/enable sequencing, ADAR1000 configuration
and beam tables, LO PLL configuration and lock check, FPGA GPIO contract (DIG0–7), AGC
outer loop, a text command interface over the ST-LINK VCP (USART2), and the existing
host-side unit tests extended to the ported code.

## Non-goals

- No USB-CDC yet (ST-LINK VCP first; USB on PA11/PA12 is a later task).
- No GPS/IMU/baro integration.
- No stepper motor.
- No 16-channel / 4× ADAR1000 support — but the ADAR1000 manager must take the device
  count as a compile-time constant (`ADAR_COUNT`, default 1) so scaling later is a constant
  change, not a rewrite.

## Requirements

### R1. Repository layout and build

- New tree `9_Firmware/9_1_Microcontroller/g0b1/` with:
  - `Core/` (`main.c`, `app/` modules, `drivers/`), `Drivers/` (STM32G0xx HAL + CMSIS,
    vendored from STM32CubeG0 — only the HAL modules actually used), `startup_stm32g0b1xx.s`,
    `STM32G0B1RETx_FLASH.ld`, `Makefile`, `README.md`.
  - **C only** (no C++): the ADAR1000 manager, AGC and settings code are rewritten/ported to
    C. No `std::vector`, no `iostream`, no `printf("%f")` (use integer milli-units).
- `make` produces `build/aeris_g0b1.elf/.bin/.hex` and prints `arm-none-eabi-size`;
  `make flash` uses `st-flash` or `STM32_Programmer_CLI`; `make test` runs host unit tests.
- Old F7 tree is left in place untouched (reference), but the old `tests/Makefile` must
  keep working or be migrated under `g0b1/tests/`.
- Build flags: `-Os -Wall -Wextra -Werror -ffunction-sections -fdata-sections --specs=nano.specs`.
  Size budget: ≤ 128 KB flash, ≤ 32 KB RAM, reported in README.

### R2. Dead code removal (upstream tree is the source; copy only what is used)

Carry over **only**: `ADAR1000_Manager` (ported to C), `ADAR1000_AGC` (C), `adf4382a_manager`
only as a reference for the new PLL driver interface, `ADS7830.c`, `RadarSettings`
(as a C struct + parser), `diag_log.h`, `um982_gps.c` (compiled, not wired).
Do **not** copy: IIO/iiod stack, `no_os_*` platform layer, `TinyGPS++`, `platform_noos_stm32`,
`stm32_*` no-OS shims, `DA5578.c`, `GY_85_HAL.c`, `BMP180.cpp`, `USBHandler.cpp`,
`gps_handler.cpp`, `ad9523.c`, `adf4382.c`.

### R3. Hardware abstraction

- `hal_spi.h/.c`: `int spi_xfer(spi_bus_t bus, gpio_t cs, const uint8_t *tx, uint8_t *rx, size_t n)`;
  blocking, CS managed by software, bus frequency set per device
  (ADAR1000 ≤ 20 MHz, PLL ≤ 10 MHz) — the F7 code ran both at 36 MHz; that defect is fixed here.
- `hal_i2c.h/.c`: `int i2c_write(addr, const uint8_t*, n)`, `int i2c_read(addr, uint8_t*, n)` with
  a **100 ms timeout** and error propagation; no `HAL_MAX_DELAY`.
- `hal_gpio.h`: named pins from one table `pins.h` (Nucleo-64 Arduino/Morpho mapping,
  documented in README with a wiring table). FPGA DIG0–7 on one GPIO port for atomic reads.
- `hal_time.h`: `millis()`, `micros()` (TIM-based), `delay_us()`. Exactly **one** delay
  implementation.

### R4. Drivers

- `adar1000.c/.h`: register read/write, init sequence, per-channel gain/phase set, TX/RX mode,
  `ADAR_COUNT` devices on one SPI bus with CS array. **TR switching is not done over SPI per
  pulse** — the FPGA drives the ADAR1000 TR pin; the MCU only sets up modes. Keep the
  scratchpad/readback self-test. Beam tables computed with integer fixed-point
  (phase in 1/64 LSB units), no `double`.
- `pll_lo.c/.h`: generic `pll_init(const pll_regs_t *table)`, `pll_is_locked()`
  (GPIO LD pin), register table provided as a `const uint32_t[]` generated from the vendor tool
  (ADF4372: ADI software; LMX2594: TICS Pro) — two example tables checked in
  (`pll_tables/adf4372_10500MHz.h`, `pll_tables/lmx2594_10500MHz.h`).
- `ads7830.c`: port of upstream, timeout fixed, error returned as negative int (never `0xFF`
  data).
- `agc.c/.h`: port of `ADAR1000_AGC` — integer only, reads saturation/enable GPIO from the FPGA
  once per frame, writes gains **only when changed**.
- `sequencer.c/.h`: power-up/down ordering as a table of `{gpio, delay_ms}` steps; emergency
  stop drops PA/ADTR rails first, then LO, then FPGA enable, and **does not re-enable on
  watchdog reset** (persist a "fault latched" flag in backup register / RAM-retained word).
- `fpga_if.c/.h`: DIG0–7 contract: toggle `new_chirp`, `new_elevation`, `new_azimuth`, set
  `mixers_enable`, `fpga_reset_n`, read `agc_saturation`, `agc_enable`.

### R5. Application

- `main.c`: init → sequencer power-up → PLL lock (fail → fault) → ADAR init + readback → FPGA
  reset release → superloop: command parser (USART2, 115200, line-oriented text:
  `beam <az> <el>`, `gain <ch> <val>`, `tx`, `rx`, `status`, `stop`), AGC tick, thermal tick
  (every 5 s), IWDG refresh (4 s window), fault handling.
- No `printf` in any per-pulse path; `DIAG` macros compile out with `-DDIAG_DISABLE`.
- Status report as a fixed ASCII line (`STATUS lock=1 temp=312 gains=...`), parsed by the
  existing Python GUI later (out of scope to adapt the GUI now).

### R6. Tests (host, gcc/clang, no hardware)

- `g0b1/tests/` with a mock HAL (`mock_spi.c`, `mock_i2c.c`, `mock_gpio.c`, `mock_time.c`) and
  tests for: ADAR1000 register encode/decode and beam table math (compare to a Python reference
  in `tests/ref_beam_table.py` for 5 angles), AGC step logic (saturation → gain down, clear →
  gain up, no write when unchanged), sequencer ordering and emergency-stop ordering, command
  parser (valid/invalid lines), ADS7830 error path. `make test` runs them; CI job added.
- Port the five upstream "Gap 3 safety" test intents (`9_1_Microcontroller/tests/`) to the new
  code where applicable.

### R7. Documentation

- `g0b1/README.md`: build/flash/test, Nucleo pin table, command reference, memory report,
  list of upstream defects fixed (reference `BOM_OPTIMIZATION_REPORT.md` §2.4 item by item).

## Acceptance

- Fresh clone + `make` → ELF built, size report shows flash ≤ 128 KB, RAM ≤ 32 KB.
- `make test` → all host tests pass; CI green.
- `grep -rn "printf" Core/app/*.c` shows no call in `pulse`/`chirp`/`agc` paths.
- README wiring table covers every signal to ADAR1000 board, PLL eval, FPGA DIG0–7, ADS7830.
