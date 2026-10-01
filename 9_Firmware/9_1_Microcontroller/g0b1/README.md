# AERIS-10 STM32G0B1 firmware (NUCLEO-G0B1RE prototype)

Bare-metal C port of the radar MCU control firmware to an STM32G0B1RET6
(Cortex-M0+, 64 MHz, 128 KB flash, 36 KB RAM; the build budget uses 32 KB RAM).
The old STM32F746 tree (`9_1_1_*`, `9_1_2_*`, `9_1_3_*`, `../tests`) is untouched.
Design: `docs/superpowers/specs/2026-10-01-firmware-g0b1-port.md`; plan:
`docs/superpowers/plans/2026-10-01-firmware-g0b1-port.md`.

> **Warnings**
> - **Nothing in this firmware has been run on hardware.** It builds, and the
>   host tests (mocks) pass; that is all that has been verified.
> - **The PLL register tables are placeholders** (`Core/drivers/pll_tables/*.h`,
>   `PLL_TABLE_PLACEHOLDER 1`). The boot log says so, the lock check will fail
>   and the unit enters `FAULT_PLL_LOCK` (non-latched, RF rails stay off) until
>   real register exports (TICS Pro / ADI ACE) are dropped in. See `BACKLOG.md`.
> - Pin assignments, alternate-function numbers and the I2C `TIMINGR`
>   (`0x10B17DB5`) were **computed from documentation, not measured and not yet
>   checked against the STM32G0B1 datasheet AF table**.

## Build, flash, test

Run everything from `9_Firmware/9_1_Microcontroller/g0b1`.

| Command | What it does |
|---|---|
| `make` | Build ELF/BIN/HEX into `build/n<ADAR_COUNT>_d<DIAG>/`, print `arm-none-eabi-size`, run `size-check` (fails if flash > 128 KB or RAM incl. 4 KB stack + 1 KB heap reserve > 32 KB) |
| `make DIAG=0 clean all` | Build with `-DDIAG_DISABLE` (all `DIAG*` log macros compile to nothing) |
| `make ADAR_COUNT=4 clean all` | Build for four ADAR1000 devices (default 1; allowed 1..4) |
| `make flash` | `st-flash --reset write ... 0x08000000` (ST-LINK); `make flash PROGRAMMER=cube` uses `STM32_Programmer_CLI` |
| `make test` | Host unit tests (native gcc, recording mocks), run for `ADAR_COUNT=1` and again for `ADAR_COUNT=4`; includes a grep gate that drivers/app do not include `stm32g0xx*.h` |
| `make clean` | Remove `build/` and the host-test build dirs |

Toolchain: `arm-none-eabi-gcc` 13.2.1 (Arm GNU Toolchain 13.2.Rel1, Debian
`15:13.2.rel1-2`) with newlib-nano. Flags: `-mcpu=cortex-m0plus -mthumb -Os
-std=c11 -Wall -Wextra -Werror -ffunction-sections -fdata-sections
--specs=nano.specs`, link `-Wl,--gc-sections`. The vendored HAL compiles under
the same `-Werror` flags (no relaxations needed). CI: job `mcu-g0b1` in
`.github/workflows/ci-tests.yml`.

### Vendored STM32Cube subset (committed in `Drivers/`)

| Repository | Tag | Commit SHA |
|---|---|---|
| `STMicroelectronics/stm32g0xx_hal_driver` | `v1.4.7` | `a0cf8a8b96183fdcc2e3b1cf0bcf0825f27bd0c9` |
| `STMicroelectronics/cmsis_device_g0` | `v1.4.5` | `f576c24e123edf3332988ecd49512c0f35f85186` |
| `STMicroelectronics/cmsis_core` | `v5.9.0_20250520` | `b7487de1303e2eb77c402432e368691e9dcfb5f0` |

`tools/fetch_cube.sh` re-vendors them: it obtains each repository with
`git clone --depth 1 --branch <tag>` (the codeload tarball method from the plan
was not usable behind the sandbox proxy; an existing clone can be reused via
`CUBE_SRC_DIR`), aborts unless `git rev-parse HEAD` equals the pinned SHA, and
copies only an allow-list of files. The copied files are committed, so a clean
checkout builds without network access.

## Wiring (Core/hal/pins.h, Core/hal/hal_gpio.c)

The "Nucleo connector" column is not derivable from anything in this repository
(UM2324 is not vendored), so it is left as **verify vs UM2324** rather than
guessed. AF numbers come from the HAL headers (`GPIO_AF0_SPI1/SPI2`,
`GPIO_AF6_I2C1`, `GPIO_AF1_USART2`); the pin-to-AF assignment is **computed, not
verified against the datasheet AF table**.

| Signal | MCU pin | Dir (MCU) | Role | Nucleo connector |
|---|---|---|---|---|
| ADAR SPI2 SCK | PB13 (AF0) | out | ADAR1000 SPI clock, 16 MHz (64/4), mode 0 | verify vs UM2324 |
| ADAR SPI2 MISO | PB14 (AF0) | in | ADAR1000 SDO | verify vs UM2324 |
| ADAR SPI2 MOSI | PB15 (AF0) | out | ADAR1000 SDI | verify vs UM2324 |
| ADAR CS0..CS3 | PB12, PB11, PB10, PB2 | out | Chip select, active low, idle high (CS1..3 used when `ADAR_COUNT` > 1) | verify vs UM2324 |
| PLL SPI1 SCK | PB3 (AF0) | out | LO PLL SPI clock, 8 MHz (64/8) | verify vs UM2324 |
| PLL SPI1 MISO | PB4 (AF0) | in | PLL readback | verify vs UM2324 |
| PLL SPI1 MOSI | PB5 (AF0) | out | PLL data | verify vs UM2324 |
| PLL CS | PB1 | out | PLL chip select, active low, idle high | verify vs UM2324 |
| PLL CE | PB0 | out | PLL chip enable, idle low | verify vs UM2324 |
| PLL LD | PB6 | in | PLL lock detect | verify vs UM2324 |
| I2C1 SCL | PB8 (AF6) | open-drain | ADS7830 (addr `0x48`, ch 0 = temperature), 100 kHz | verify vs UM2324 |
| I2C1 SDA | PB9 (AF6) | open-drain | ADS7830 | verify vs UM2324 |
| USART2 TX / RX | PA2 / PA3 (AF1) | out / in | ST-LINK virtual COM port, 115200 8N1 | ST-LINK VCP (on-board; verify vs UM2324) |
| LED LD4 | PA5 | out | Status LED | on-board |
| EN_FPGA | PA0 | out | FPGA rail enable, active high | verify vs UM2324 |
| EN_LO | PA1 | out | LO rail enable | verify vs UM2324 |
| EN_ADAR | PA4 | out | ADAR1000 supply enable | verify vs UM2324 |
| EN_ADTR_VDD_SW | PA6 | out | ADTR1107 VDD_SW enable | verify vs UM2324 |
| EN_ADTR_VSS_SW | PA7 | out | ADTR1107 VSS_SW enable | verify vs UM2324 |
| EN_LNA | PA8 | out | ADTR LNA 3V3 rail enable | verify vs UM2324 |
| EN_PA | PA9 | out | PA 5 V rail enable | verify vs UM2324 |
| FPGA DIG0 | PC0 | out | `new_chirp` | verify vs UM2324 |
| FPGA DIG1 | PC1 | out | `new_elevation` | verify vs UM2324 |
| FPGA DIG2 | PC2 | out | `new_azimuth` | verify vs UM2324 |
| FPGA DIG3 | PC3 | out | `mixers_enable` | verify vs UM2324 |
| FPGA DIG4 | PC4 | out | `fpga_reset_n` | verify vs UM2324 |
| FPGA DIG5 | PC5 | in | `agc_saturation` | verify vs UM2324 |
| FPGA DIG6 | PC6 | in | `agc_enable` | verify vs UM2324 |
| FPGA DIG7 | PC7 | in | reserved | verify vs UM2324 |

Hardware notes: DIG0..DIG7 are PC0..PC7 so a single IDR read gives the whole
bus. All outputs idle low except the chip selects, which idle high. The I2C
`TIMINGR` `0x10B17DB5` (I2CCLK 64 MHz, just under 100 kHz) was computed from the
RM0444 formulae, not measured. The ADTR1107 `CTRL_SW` polarity is assumed to be
driven by the ADAR1000 `TR_SW_POS` output; confirm on the schematic (BACKLOG).

## Serial command interface

USART2 via the ST-LINK VCP, 115200 8N1, one command per line (CR or LF
terminated, max line length `CMD_MAX_LINE`; longer lines answer `ERR too_long`).
Tokens are space separated; integers are strict `[+-]digits`. Replies end in
CRLF. Source: `Core/app/cmd.c`.

| Command | Effect | Replies |
|---|---|---|
| `beam <az> <el>` | `az` -180..180, `el` -60..60 (integer degrees). Applies the integer beam table (`beam_apply(el)`; the array is steered electronically in elevation, azimuth is mechanical/stored only). Toggles DIG1 `new_elevation` if `el` changed and DIG2 `new_azimuth` if `az` changed. | `OK`, `ERR args`, `ERR range`, `ERR spi` |
| `gain <ch> <val>` | Direct write of RX VGA register of global channel `ch` (0..`ADAR_COUNT*4-1`, 0-based), `val` 0..127. The AGC cache is updated; the AGC overwrites it on its next change when FPGA AGC is enabled. | `OK`, `ERR args`, `ERR range`, `ERR spi` |
| `tx` | All ADAR1000 devices to SPI-controlled TX (`TR_SOURCE=0`) for bench tests | `OK`, `ERR spi` |
| `rx` | All devices to SPI-controlled RX | `OK`, `ERR spi` |
| `auto` | Return to TR-pin mode (FPGA owns TX/RX switching); the default after init | `OK`, `ERR spi` |
| `status` | One status line (below); the only command answered while a fault is latched | `STATUS ...` |
| `stop` | Latched emergency stop (`FAULT_ESTOP_CMD`) | `OK stopped` |

Other replies: `ERR unknown` (unknown command), `ERR args` (wrong argument
count or non-integer, also for `status`/`tx`/`rx`/`auto`/`stop` with extra
tokens), `ERR latched` (any command except `status` while a fault is latched),
`ERR too_long`. An empty line produces no reply.

Status line format:

```
STATUS lock=<0|1> temp=<deci-degC> temp_err=<0|1> fault=<code> latched=<0|1> mode=<auto|tx|rx> az=<deg> el=<deg> agc=<0|1> base=<n> gains=<g0,g1,...>
```

`temp` is in 0.1 degC (750 = 75.0 degC); `gains` lists the last value written
per channel (`-1` = not written since init or since a failed write).
Fault codes: 0 none, 1 `PLL_LOCK`, 2 `ADAR_COMM` (non-latched); 10 `OVERTEMP`,
11 `ESTOP_CMD`, 12 `PANIC` (latched).

## Memory report

`arm-none-eabi-size` / `make size-check`, re-run for this README
(`size-check` counts flash = isr_vector+text+rodata+data+init arrays, RAM =
data+bss+noinit + 4 KB stack + 1 KB heap reserve):

| Configuration | Flash (of 131072) | RAM (of 32768) |
|---|---|---|
| default (`ADAR_COUNT=1`, `DIAG=1`) | 23916 | 6320 |
| `DIAG=0` | 17116 | 5896 |
| `ADAR_COUNT=4` | 24028 | 6356 |

## Fault model

- **Latched faults** (emergency stop, then latch): `stop` command, over-temperature
  at or above 75.0 degC (checked every 5 s), `Error_Handler()` / `HardFault_Handler()`
  (`FAULT_PANIC`).
- **Non-latched faults** (RF rails off, base rails stay on, retried on next
  boot): PLL lock failure at init or lock loss while running (`FAULT_PLL_LOCK`),
  ADAR1000 scratchpad failure (`FAULT_ADAR_COMM`). Thermal sensor read errors
  are only reported (`temp_err=1`); they are not a fault (no PA on the prototype).
- **The latch lives in RAM `.noinit`** as `{magic 0x4641554C, code, ~code}`; any
  mismatch means "not latched". It survives IWDG, software and NRST resets and
  is **cleared only by a power cycle**. On boot with a valid latch the RF rails
  stay off, the command parser runs, `status` reports `latched=1`, and every
  other command answers `ERR latched`. Sequencer power-up returns `-EPERM`.
- **Emergency-stop order** (GPIO only, no SPI, no delays, so it works with a hung
  bus): DIG3 `mixers_enable` low, PA 5 V, LNA 3V3, ADTR VSS_SW, ADTR VDD_SW,
  ADAR supply, LO enable, then DIG0, DIG1, DIG2, DIG4 low, and `EN_FPGA` last.
  The DIG lines go low before the FPGA loses power so a high MCU output cannot
  back-power an unpowered FPGA through its input protection.
- **`Error_Handler()` and `HardFault_Handler()`** run the e-stop, set the latch
  (`FAULT_PANIC`) and spin **without** refreshing the IWDG; the reset after
  about 4 s boots into the latched, safe state. (Upstream's `Error_Handler`
  reset and re-energised the rails.)
- **IWDG** 4 s (LSI 32 kHz / 256, reload 500), refreshed from the main loop only.
- Power-up is split: base rails (FPGA, LO, ADAR, VDD_SW, VSS_SW) -> ADAR init and
  safe PA bias -> RF rails (LNA 3V3, then PA 5 V), because the ADTR1107 needs a
  negative `VGG_PA` before `VDD_PA`.

## Upstream defects fixed

Items from `BOM_OPTIMIZATION_REPORT.md` section 2.4 (code state):

| Upstream defect | Resolution here |
|---|---|
| Does not build (no `.ld`, startup, Makefile, HAL/CMSIS, USB files; 3 args to 4-arg `DIAG_GPIO`; prototype mismatch for `executeChirpSequence`; case-sensitive include names) | New self-contained tree: linker script, startup, Makefile, vendored Cube subset, `-Wall -Wextra -Werror`, builds on Linux |
| About 21 k lines of dead code (no-OS IIO stack, unused `stm32_*` and `no_os_*` modules, TinyGPS++, `platform_noos_stm32.c`) | Not carried over; only what is used is ported |
| Empty `STM32_ALGO.docx` | Not referenced |
| AD9523 bug: bare `&hspi4` passed as `extra`, CS never toggled | AD9523/ADF4382 path not ported; the PLL driver uses one `hal_spi` with explicit software CS |
| `GPS_Init` never called, GUI gets no GPSB packet | `um982_gps.c` is compiled only, not wired (BACKLOG) |
| `RadarSettings` parsed from USB but never read | Not ported (no USB in this track); runtime settings are the text commands above |
| `Error_Handler()` = `__disable_irq(); while(1)` with no IWDG refresh, so the reset re-energised the power rails | `Error_Handler`/HardFault run the e-stop, latch in `.noinit`, spin; boot with a latch keeps rails off |
| ADS7830 returns `0xFF` on I2C error, indistinguishable from full scale, causing false Idq/165 degC e-stops | Driver returns negative errno; the value is separate from the status; read errors are not faults |
| `HAL_MAX_DELAY` I2C timeouts everywhere | None anywhere: I2C 100 ms, SPI 10 ms, UART TX 50 ms (grep gate) |
| 4 delay implementations | Exactly one: `delay_us()` / `delay_ms()` in `hal_time` |
| 3 SPI paths, 3 copies of beam phase math, 3 GPS stacks, duplicated Idq loops | One `hal_spi`, one integer beam table (`beam.c`, with a Python reference), one GPS file, no Idq loop |

Additional defects found during this port:

- **`TR_SOURCE` misuse.** Reg 0x031 bit 2 selects SPI (0) vs TR-pin (1) control
  (ADAR1000 Rev. B p. 37); upstream wrote it believing it meant "TX". Now the
  device runs in pin mode by default (FPGA owns TR), `tx`/`rx` force SPI mode,
  `auto` restores pin mode.
- **`SW_DRV_TR_STATE` (reg 0x031 bit 7) must be set** for correct ADTR1107
  `CTRL_SW` polarity: with `SW_DRV_TR_MODE_SEL=0`, `TR_SW_POS` is 3.3 V in
  receive and 0 V in transmit when the bit is 1 (ADAR1000 Table 24 p. 42), and
  ADTR1107 `CTRL_SW` is low = transmit, high = receive (ADTR1107 Table 8 p. 6).
  It is set in every mode. Assumes `TR_SW_POS` drives `CTRL_SW` (BACKLOG: confirm
  on the schematic).
- **1-based channel masking.** Upstream passed 1-based channels masked with
  `& 0x03`, so "channel 1" wrote CH2 and "channel 4" wrote CH1. Channels are
  0-based; a test covers channel 3's registers.
- **TX phase loaded LDRX instead of LDTX.**
- **Scratchpad failure ignored.** Now a non-latched `FAULT_ADAR_COMM`.
- **ADS7830:** `0xFF` treated as the error value; channel encoded as `ch << 4`
  (the C2..C0 field is a non-linear mapping, Table 2 p. 14 - fixed); raw counts
  stored as degC (now converted: 20 mV/degC from the 2.5 V reference, in 0.1 degC).
- **SPI at 36 MHz** (above the ADAR1000 limit); now 16 MHz for ADAR, 8 MHz for PLL.
- **`HAL_MAX_DELAY`**, **`Error_Handler` re-energising rails** and the **4 delay
  implementations**: see the table above.

## Gap-3 mapping

The five upstream "Gap 3" safety test intents, as ported in
`tests/test_gap3_port.c` (and `tests/test_app.c` where `app_loop()` is needed):

| Upstream intent | Test here |
|---|---|
| Emergency stop cuts rails in order, FPGA enable last | `test_gap3_estop_cuts_rails` (also `test_sequencer.c`: `test_estop_order_no_delay_no_bus`, `test_estop_dig_low_before_fpga_enable`) |
| Emergency state is set and survives a reset (no re-energise) | `test_gap3_latch_set_and_survives_reset` (also `test_fault.c`) |
| IWDG window / refresh | `test_gap3_iwdg_window` (4.0 s window), `test_loop_refreshes_iwdg` (`test_app.c`); cold-start timer: `test_cold_start_no_spurious_ticks` |
| Over-temperature triggers emergency stop | `test_gap3_overtemp_estops`, `test_overtemp_in_run_latches_and_stops_work` (`test_app.c`) |
| IDQ periodic re-read; max-of-8 sensors | **Not applicable**: no Idq channels and a single temperature sensor on this prototype |

## Not covered here

No USB (a `RadarSettings` binary packet is not ported), no GPS/IMU wiring, no GUI
parser for `STATUS`. See `BACKLOG.md` at the repository root.
