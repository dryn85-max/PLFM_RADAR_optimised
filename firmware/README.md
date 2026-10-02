# AERIS-10 STM32G0B1 firmware (NUCLEO-G0B1RE prototype)

Bare-metal C port of the radar MCU control firmware to an STM32G0B1RET6
(Cortex-M0+, 64 MHz; the device has 512 KB flash / 144 KB RAM, the build budget is 128 KB flash / 32 KB RAM).
The upstream STM32F746 tree is kept unchanged as reference under
[`../legacy/9_Firmware/9_1_Microcontroller/`](../legacy/9_Firmware/9_1_Microcontroller/).
Design: [`docs/superpowers/specs/2026-10-01-firmware-g0b1-port.md`](../docs/superpowers/specs/2026-10-01-firmware-g0b1-port.md);
plan: [`docs/superpowers/plans/2026-10-01-firmware-g0b1-port.md`](../docs/superpowers/plans/2026-10-01-firmware-g0b1-port.md).
Back to the [root README](../README.md); see also [docs/architecture.md](../docs/architecture.md)
and [docs/bring-up.md](../docs/bring-up.md).

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

Run everything from `firmware/`.

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

The "Nucleo connector" column is filled only where it follows from the
UM2324-derived notes in the (untrusted, alternative-plan) PR
`dryn85-max/PLFM_RADAR_optimised#1` for the *same MCU pins*; UM2324 itself is not
vendored here, so every other entry says **VERIFY vs UM2324** instead of being
guessed. AF numbers come from the HAL headers (`GPIO_AF0_SPI1/SPI2`,
`GPIO_AF6_I2C1`, `GPIO_AF1_USART2`); the pin-to-AF assignment is **computed, not
verified against the datasheet AF table**. The pin table itself is data in
`Core/hal/pins_table.c`; `tests/test_pins.c` checks it (DIG0..7 = PC0..PC7, no
shared pins, none of the Nucleo-reserved lines: PC13 B1, PC14/PC15 LSE, PF0/PF1
HSE pads, PA13/PA14 SWD, PA11/PA12 future USB, PA9/PA10 which the G0 can remap
onto the PA11/PA12 pads).

| Signal | MCU pin | Dir (MCU) | Role | Nucleo connector |
|---|---|---|---|---|
| ADAR SPI2 SCK | PB13 (AF0) | out | ADAR1000 SPI clock, 16 MHz (64/4), mode 0 | CN10 (Morpho) |
| ADAR SPI2 MISO | PB14 (AF0) | in | ADAR1000 SDO | CN10 (Morpho) |
| ADAR SPI2 MOSI | PB15 (AF0) | out | ADAR1000 SDI | CN10 (Morpho) |
| ADAR CS0..CS3 | PB12, PB11, PB10, PB2 | out | Chip select, active low, idle high (CS1..3 used when `ADAR_COUNT` > 1) | VERIFY vs UM2324 |
| PLL SPI1 SCK | PB3 (AF0) | out | LO PLL SPI clock, 8 MHz (64/8) | VERIFY vs UM2324 |
| PLL SPI1 MISO | PB4 (AF0) | in | PLL readback | VERIFY vs UM2324 |
| PLL SPI1 MOSI | PB5 (AF0) | out | PLL data | VERIFY vs UM2324 |
| PLL CS | PB1 | out | PLL chip select, active low, idle high | VERIFY vs UM2324 |
| PLL CE | PB0 | out | PLL chip enable, idle low | VERIFY vs UM2324 |
| PLL LD | PB6 | in | PLL lock detect | VERIFY vs UM2324 |
| I2C1 SCL | PB8 (AF6) | open-drain | ADS7830 (addr `0x48`, ch 0 = temperature), 100 kHz | CN5 D15; solder bridge to A4 (PC1 = DIG1) must be open, see below |
| I2C1 SDA | PB9 (AF6) | open-drain | ADS7830 | CN5 D14; solder bridge to A5 (PC0 = DIG0) must be open, see below |
| USART2 TX / RX | PA2 / PA3 (AF1) | out / in | ST-LINK virtual COM port, 115200 8N1 | ST-LINK VCP (on-board; routed to ST-LINK by default, not to the headers; VERIFY solder bridges vs UM2324) |
| LED LD4 | PA5 | out | Status LED | on-board LD4 (user LED) |
| EN_FPGA | PA0 | out | FPGA rail enable, active high | VERIFY vs UM2324 |
| EN_LO | PA1 | out | LO rail enable | VERIFY vs UM2324 |
| EN_ADAR | PA4 | out | ADAR1000 supply enable | VERIFY vs UM2324 |
| EN_ADTR_VDD_SW | PA6 | out | ADTR1107 VDD_SW enable | VERIFY vs UM2324 |
| EN_ADTR_VSS_SW | PA7 | out | ADTR1107 VSS_SW enable | VERIFY vs UM2324 |
| EN_LNA | PA8 | out | ADTR LNA 3V3 rail enable | VERIFY vs UM2324 |
| EN_PA | PC8 | out | PA 5 V rail enable (was PA9; moved off the PA9/PA11 remap pair) | VERIFY vs UM2324 |
| FPGA DIG0 | PC0 | out | `new_chirp` | A5 (connector VERIFY vs UM2324) |
| FPGA DIG1 | PC1 | out | `new_elevation` | A4 (connector VERIFY vs UM2324) |
| FPGA DIG2 | PC2 | out | `new_azimuth` | VERIFY vs UM2324 |
| FPGA DIG3 | PC3 | out | `mixers_enable` | VERIFY vs UM2324 |
| FPGA DIG4 | PC4 | out | `fpga_reset_n` | VERIFY vs UM2324 |
| FPGA DIG5 | PC5 | in | `agc_saturation` | VERIFY vs UM2324 |
| FPGA DIG6 | PC6 | in | `agc_enable` | VERIFY vs UM2324 |
| FPGA DIG7 | PC7 | in | reserved | VERIFY vs UM2324 |

**Hardware requirements.**

- **Nucleo solder bridges (UM2324).** PB8/PB9 (I2C1 SCL/SDA, Arduino D15/D14)
  may be tied by solder bridges to A4/A5, which are PC1/PC0 = our DIG1/DIG0; those
  bridges **must be open**, otherwise the I2C bus and the FPGA DIG lines are
  shorted together (also in the BACKLOG bring-up checklist). PA2/PA3 are routed to
  the ST-LINK VCP by default; do not use them for anything else unless the
  bridges are changed. Exact bridge names: VERIFY vs UM2324.
- **External pull-downs on all `EN_*` lines (EN_FPGA, EN_LO, EN_ADAR,
  EN_ADTR_VDD_SW, EN_ADTR_VSS_SW, EN_LNA, EN_PA) and on DIG0..DIG4.** The MCU
  pins are high-Z during reset and flashing, and the FPGA `reset_n` (DIG4) has
  an internal `PULLUP` (`../legacy/9_Firmware/9_2_FPGA/constraints/xc7a50t_ftg256.xdc:121`),
  so without a pull-down the FPGA would be released from reset and the rails could
  float on at power-up or while flashing.
- **ADAR1000 `PA_ON` pin (B3)** is not driven by the MCU (no GPIO assigned). It
  has an internal 100 kohm pull-up to the 1.8 V LDO. In TR-pin mode with
  `BIAS_CTRL=1` it must be high (or left floating) for the PA bias to use the
  `PA_BIAS_ON` values on TR=1; low forces the `OFF` values (ADAR1000 DS p. 39,
  Table 16). Do not pull it low on the board. At present ON = OFF = `0x5D`, so it
  has no effect until the Idq calibration gives the two values different numbers.

Hardware notes: DIG0..DIG7 are PC0..PC7 so a single IDR read gives the whole
bus. All outputs idle low except the chip selects, which idle high. The I2C
`TIMINGR` `0x10B17DB5` (I2CCLK 64 MHz, just under 100 kHz) was computed from the
RM0444 formulae, not measured. The ADTR1107 `CTRL_SW` polarity is assumed to be
driven by the ADAR1000 `TR_SW_POS` output; confirm on the schematic (BACKLOG).

## PLL register tables from vendor exports

`tools/regtable_to_h.py` turns a vendor export into a `pll_tables/*.h` header
that matches `pll_regs_t` (`{words, count, settle_ms, name}`) and defines
`PLL_TABLE_PLACEHOLDER` as 0 (the checked-in placeholder headers leave it at the
default 1). Input, one register per line (blank lines and `#` / `//` comments
are skipped, anything else malformed is an error, empty input is an error):

- LMX2594, TICS Pro "Hex Registers": `R<n><tab>0x<6 hex>` (R0..R112). The
  24-bit frame is used as is; its address byte must equal `<n>`, R/W bit 0.
- ADF4372, ADI software: `0x<4 hex> 0x<2 hex>` (address, data); word =
  `addr15 << 8 | data8`.

Words keep the file order. Example (the symbol must stay
`LMX2594_10500MHZ` / `ADF4372_10500MHZ`, which `pll_lo.c` selects):

```
tools/regtable_to_h.py --chip lmx2594 --name LMX2594_10500MHZ --settle-ms 10 \
    -o Core/drivers/pll_tables/lmx2594_10500MHz.h tics_export.txt
```

`tests/test_regtable_to_h.py` (run by `make test`; prints SKIP if `python3` is
missing) checks word packing, ordering, rejection of empty/malformed input and
that the generated headers compile (`gcc -fsyntax-only`).

## Serial command interface

USART2 via the ST-LINK VCP, 115200 8N1, one command per line (CR or LF
terminated, max line length `CMD_MAX_LINE`; longer lines answer `ERR too_long`).
Tokens are space separated; integers are strict `[+-]digits`. Replies end in
CRLF. Source: `Core/app/cmd.c`.

| Command | Effect | Replies |
|---|---|---|
| `beam <az> <el>` | `az` -180..180, `el` -60..60 (integer degrees). Applies the integer beam table (`beam_apply(el)`; the array is steered electronically in elevation, azimuth is mechanical/stored only). Toggles DIG1 `new_elevation` if `el` changed and DIG2 `new_azimuth` if `az` changed. If an SPI write fails part-way the reply is `ERR spi`, `az`/`el` keep their old values and DIG1/DIG2 are not toggled; some elements may already have the new phases, so the firmware best-effort re-applies the previous elevation (retry the command if the bus recovered). | `OK`, `ERR args`, `ERR range`, `ERR spi` |
| `gain <ch> <val>` | Direct write of RX VGA register of global channel `ch` (0..`ADAR_COUNT*4-1`, 0-based), `val` 0..127. The AGC cache is updated. While the FPGA AGC is enabled (DIG6 high) the value is overwritten on the next AGC tick (within `AGC_PERIOD_MS` = 250 ms) with the AGC's effective gain, so a manual `gain` only sticks while the AGC is disabled. | `OK`, `ERR args`, `ERR range`, `ERR spi` |
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
STATUS lock=<0|1> temp=<deci-degC> temp_err=<0|1> fault=<code> latched=<0|1> mode=<none|auto|tx|rx|mixed> az=<deg> el=<deg> agc=<0|1> base=<n> gains=<g0,g1,...>
```

`mode` is the ADAR1000 mode last applied successfully (`none` before the first successful mode write after init, `mixed` if a failed all-device switch left the devices different). `temp` is in 0.1 degC (750 = 75.0 degC); `gains` lists the last value written
per channel (`-1` = not written since init or since a failed write).
Fault codes: 0 none, 1 `PLL_LOCK`, 2 `ADAR_COMM` (non-latched); 10 `OVERTEMP`,
11 `ESTOP_CMD`, 12 `PANIC`, 13 `WATCHDOG` (latched).

## Memory report

`arm-none-eabi-size` / `make size-check`, re-run for this README
(`size-check` counts flash = isr_vector+text+rodata+data+init arrays, RAM =
data+bss+noinit + 4 KB stack + 1 KB heap reserve):

| Configuration | Flash (of 131072) | RAM (of 32768) |
|---|---|---|
| default (`ADAR_COUNT=1`, `DIAG=1`) | 24268 | 6320 |
| `DIAG=0` | 16932 | 5896 |
| `ADAR_COUNT=4` | 24448 | 6360 |

## Fault model

- **Latched faults** (emergency stop, then latch): `stop` command, over-temperature
  at or above 75.0 degC (checked every 5 s), `Error_Handler()` / `HardFault_Handler()`
  / `NMI_Handler()` / any unexpected interrupt (`Default_Handler`) (`FAULT_PANIC`),
  and a watchdog reset with no earlier latch (`FAULT_WATCHDOG`, below). The first
  latched code is kept; later causes never overwrite it, and the latch is stored
  before the e-stop runs.
- **Non-latched faults** (RF rails off, base rails stay on, retried on next
  boot): PLL lock failure at init or lock loss while running (`FAULT_PLL_LOCK`),
  ADAR1000 scratchpad failure (`FAULT_ADAR_COMM`). Thermal sensor read errors
  are only reported (`temp_err=1`); they are not a fault. The PA is inside the
  ADTR1107, so while the sensor is unreadable there is no over-temperature
  protection.
- **The latch lives in RAM `.noinit`** as `{magic 0x4641554C, code, ~code}`; any
  mismatch means "not latched". It survives IWDG, software and NRST resets and
  is **cleared only by a power cycle**. On boot with a valid latch the RF rails
  stay off, the command parser runs, `status` reports `latched=1`, and every
  other command answers `ERR latched`. Sequencer power-up returns `-EPERM`.
- **Emergency-stop order** (GPIO only, no SPI, no delays, so it works with a hung
  bus): DIG3 `mixers_enable` low, PA 5 V, LNA 3V3, ADTR VSS_SW, ADTR VDD_SW,
  ADAR supply, ADAR CS0..3 low, LO enable, PLL CE and CS low, then DIG0, DIG1,
  DIG2, DIG4 low, and `EN_FPGA` last. The DIG lines go low before the FPGA loses
  power so a high MCU output cannot back-power an unpowered FPGA through its
  input protection. Chip selects and CE go low only *after* their chip's supply
  is off: a high output into an unpowered ADAR1000/PLL would back-power it
  through its input protection (inputs must stay at or below supply + 0.3 V),
  while a low level is always within the absolute maximum ratings and selecting
  an unpowered chip does nothing. (The orderly `SEQ_DOWN` has the same order,
  with delays.) Known gap: `hal_gpio_init()` still idles the CS lines high
  before the rails come up (BACKLOG).
- **`Error_Handler()`, `HardFault_Handler()`, `NMI_Handler()` and
  `Default_Handler`** (unexpected interrupts) run the e-stop, set the latch
  (`FAULT_PANIC`) and spin **without** refreshing the IWDG; once the IWDG is
  running, the reset after about 4 s boots into the latched, safe state.
  `Error_Handler()` is also reachable before the IWDG is started (clock
  configuration failure in `main()`): then there is no watchdog reset, the rails
  stay off (e-stop applied) and the MCU spins until a manual reset or power
  cycle. (Upstream's `Error_Handler` reset and re-energised the rails.)
- **Reset cause at boot** (spec R4): `main()` reads `RCC->CSR` before the IWDG
  starts; if the IWDG or WWDG flag is set and no valid latch exists,
  `fault_on_boot()` latches `FAULT_WATCHDOG` (13, cleared only by a power cycle)
  and the boot takes the latched path (rails off). With an existing latch the
  first cause is kept. `RMVF` then clears the flags. The G0 has no separate
  LOCKUP reset flag; a lockup ends in the IWDG reset.
- **IWDG** 4 s (LSI 32 kHz / 256, reload 500), refreshed in the main loop and
  between the long `app_init()` stages; `delay_ms()` does not refresh it.
- Power-up is split: base rails (FPGA, LO, ADAR, VDD_SW, VSS_SW) -> ADAR init and
  safe PA bias and `CTRL_SW` driven to receive (`TR_SW_POS` floats after reset,
  ADAR1000 DS p. 38/44) -> RF rails (LNA 3V3, then PA 5 V) -> operational bias,
  TR-pin mode, beam 0, boot gains (TX VGA `0x7F`, RX VGA = AGC base 30), because
  the ADTR1107 needs a negative `VGG_PA` before `VDD_PA`.
- **PA/LNA gate bias is deliberately conservative**: PA ON = PA OFF = `0x5D`
  (-1.75 V, PA pinched), LNA ON `0x00` (0 V), LNA OFF `0x68` (-1.96 V); every
  PA/LNA bias constant is limited to `0x6A` (-2.0 V; DAC is linear, `0xFF` =
  -4.8 V, ADAR1000 DS p. 31) by `_Static_assert`. The real quiescent-current
  (Idq) setting must be calibrated on hardware (BACKLOG).

## Upstream defects fixed

Items from [`docs/bom-optimization.md`](../docs/bom-optimization.md) section 2.4 (code state):

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
| `HAL_MAX_DELAY` I2C timeouts everywhere | No `HAL_MAX_DELAY` in `Core/` (checked by grep, not enforced by a test): I2C 100 ms, SPI 10 ms, UART TX 50 ms |
| 4 delay implementations | Exactly one: `delay_us()` / `delay_ms()` in `hal_time` |
| 3 SPI paths, 3 copies of beam phase math, 3 GPS stacks, duplicated Idq loops | One `hal_spi`, one integer beam table (`beam.c`, with a Python reference), one GPS file, no Idq loop |

Additional defects found during this port:

- **`TR_SOURCE` misuse.** Reg 0x031 bit 2 selects SPI (0) vs TR-pin (1) control
  (ADAR1000 Rev. B p. 37); upstream wrote it believing it meant "TX". Now the
  device runs in pin mode by default (FPGA owns TR), `tx`/`rx` force SPI mode,
  `auto` restores pin mode.
- **`SW_DRV_TR_STATE` (reg 0x031 bit 7) must be set** for correct ADTR1107
  `CTRL_SW` polarity: with `SW_DRV_TR_MODE_SEL=0`, `TR_SW_POS` is 3.3 V in
  receive and 0 V in transmit when the bit is 1 (ADAR1000 Table 14 p. 38 gives
  the output levels; Table 24 p. 42 lists the register 0x31 bits of the setup), and
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
