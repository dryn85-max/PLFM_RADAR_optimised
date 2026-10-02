# AERIS-10 Lite architecture

Facts in this document come from the repository (module READMEs, RTL, firmware
sources, specs). Items that could not be verified there are marked **VERIFY**.
Nothing has been run on hardware; see [bring-up.md](bring-up.md).

## System block diagram

```
                      host PC (PyQt6 V7 GUI, host/)
                                 ^
                                 | USB 2.0, FT232H / FT2232H, 245 sync FIFO, 8-bit
                                 v
 ADC 12-bit CMOS  ---->  +-----------------------+  DIG0..DIG4 (MCU out -> FPGA in)
 100 MSPS, 20 MHz IF     |  FPGA (Cyclone dev    | <------------------------------+
 DAC 8-bit        <----  |  board, TBD)          |  DIG5..DIG7 (FPGA out -> MCU in)|
 (chirp, 120 MHz clk)    |  fpga/ vendor-neutral | ------------------------------+ |
                         +-----------------------+                               | |
                                                                    +-------------v-v--+
 LO PLL eval board (ADF4372 / LMX2594) <-- SPI1 8 MHz + CE/LD ----- |  NUCLEO-G0B1RE   |
 ADAR1000 (4 ch) + 4x ADTR1107          <-- SPI2 16 MHz + 4 CS ---- |  STM32G0B1       |
 ADS7830 (temperature)                  <-- I2C1 100 kHz ---------- |  firmware/       |
 power rail enables (7x EN_*)           <-- GPIO ------------------ |                  |
 ST-LINK VCP (USART2, 115200 8N1)       <-> text commands / STATUS  +------------------+
```

Roles:

- **FPGA** (`fpga/`): captures the ADC, down-converts, pulse-compresses, Doppler
  processes, runs CFAR, generates the TX chirp through the DAC and streams
  results to the host.
- **MCU** (`firmware/`): sequences power rails, programs the LO PLL and the
  ADAR1000, steers the beam, runs the outer AGC loop, monitors temperature,
  enforces the fault model and offers a text command interface on the ST-LINK
  virtual COM port.
- **Host** (`host/`): GUI and USB protocol layer. Not yet adapted to the
  4-channel prototype or to the firmware `STATUS` line (BACKLOG).

## FPGA signal chain (summary)

Full detail, per-boundary widths and the verification description are in
[fpga/README.md](../fpga/README.md).

| Stage | Rate | Width (bits) | Note |
|---|---|---|---|
| ADC capture (`adc_cmos_interface`) | 100 MSPS | 12, offset binary | `clk_100m` is the ADC data clock |
| DDC: NCO 20 MHz, mixer, CIC N=5 R=4 (26-bit Hogenauer) | 100 -> 25 MSPS | 12 -> 16 I/Q | mixer 12 x 16 -> 28 bit, then [27:12] |
| FIR 32-tap, folded 4-phase | 25 MSPS | 16 I/Q (40-bit accumulator) | 4 multipliers per channel |
| Gain control (`rx_gain_control`) | 25 MSPS | 16 | host shift / AGC |
| Matched filter (`matched_filter_multi_segment`) | window buffered, 4 x 256-pt segments | 16 in/out, 25 internal FFT word | receive window RAM 1024 x 16; FFT -> conjugate multiply with ROM reference spectrum -> IFFT |
| Range-bin decimator | 256 -> 64 bins per segment | 16 | peak detect |
| MTI | per bin | 16 | 2-pulse canceller |
| Doppler | 64 range bins x 32 chirps frame | 16, 24-bit FFT word | Hamming window, two 16-pt FFTs |
| CFAR (`cfar_ca`) | per cell | 17 / 23 / 31 | `|I|+|Q|`, alpha Q4.4 |
| USB (`usb_data_interface_ft2232h`) | - | 8-bit bus | host packet format unchanged from upstream |

Long chirp: 928 receive samples at 25 MSPS (6 m range-bin spacing, 256 bins per
segment); short chirp: 13 samples (about 78 m). Matched-filter busy time is
about 43 800 clk (0.44 ms) per long chirp.

## Interfaces

### FPGA <-> MCU: DIG0-7

DIG0..DIG7 are MCU pins PC0..PC7, so one read of the GPIO input register
returns the whole bus (`firmware/Core/hal/pins.h`, `pins_table.c`). Direction is
from the MCU's point of view. Idle level is low for all of them.

| Line | MCU pin | Direction | Signal | FPGA side (`radar_system_top.v`) |
|---|---|---|---|---|
| DIG0 | PC0 | MCU -> FPGA | `new_chirp` (toggle) | `stm32_new_chirp` |
| DIG1 | PC1 | MCU -> FPGA | `new_elevation` (toggle on elevation change) | `stm32_new_elevation` |
| DIG2 | PC2 | MCU -> FPGA | `new_azimuth` (toggle on azimuth change) | `stm32_new_azimuth` |
| DIG3 | PC3 | MCU -> FPGA | `mixers_enable` | `stm32_mixers_enable` (synchronised to `clk_100m`, status bit 0) |
| DIG4 | PC4 | MCU -> FPGA | `fpga_reset_n` | FPGA reset input (`reset_n`); the board wrapper must connect it. **VERIFY** (wrapper does not exist yet) |
| DIG5 | PC5 | FPGA -> MCU | `agc_saturation` | `gpio_dig5` = per-frame saturation count not zero |
| DIG6 | PC6 | FPGA -> MCU | `agc_enable` | `gpio_dig6` = `host_agc_enable`; the MCU applies a 2-frame debounce |
| DIG7 | PC7 | FPGA -> MCU | reserved | `gpio_dig7` tied low |

The upstream RTL comments name the pins of the upstream board (PD13..PD15 and
FPGA balls); those do not apply to Lite. The contract tests
(`tests/cross_layer/`) check roles and directions between RTL and firmware.

### MCU buses and devices

| Bus | Pins | Speed / mode | Devices |
|---|---|---|---|
| SPI2 | PB13 SCK, PB14 MISO, PB15 MOSI (AF0); CS0..CS3 = PB12, PB11, PB10, PB2 | 16 MHz (64/4), mode 0 | ADAR1000 (1 device by default, `ADAR_COUNT` up to 4) |
| SPI1 | PB3 SCK, PB4 MISO, PB5 MOSI (AF0); CS = PB1, CE = PB0, LD (in) = PB6 | 8 MHz (64/8) | LO PLL (LMX2594 or ADF4372) |
| I2C1 | PB8 SCL, PB9 SDA (AF6) | 100 kHz, `TIMINGR` `0x10B17DB5` (computed, not measured) | ADS7830 at `0x48`, channel 0 = temperature |
| USART2 | PA2 TX, PA3 RX (AF1) | 115200 8N1 | ST-LINK virtual COM port: text commands, `STATUS`, diagnostic log |
| GPIO | PA0, PA1, PA4, PA6, PA7, PA8, PC8 | active-high enables | EN_FPGA, EN_LO, EN_ADAR, EN_ADTR_VDD_SW, EN_ADTR_VSS_SW, EN_LNA, EN_PA |

Full wiring table with the Nucleo connector column (many entries still
**VERIFY vs UM2324**): [firmware/README.md](../firmware/README.md#wiring-corehalpinsh-corehalhal_gpioc).
Whether the ADAR1000 SPI also passes through the FPGA (the RTL still has
`stm32_*_3v3` / `*_1v8` pass-through ports) on the Lite board: **VERIFY**.

The MCU command interface (`beam`, `gain`, `tx`, `rx`, `auto`, `status`, `stop`)
and the `STATUS` line format are documented in
[firmware/README.md](../firmware/README.md#serial-command-interface).

### Host USB link

- Protocol is unchanged from upstream: data packets (`0xAA` ... `0x55`), status
  packets (`0xBB` ... `0x55`), 4-byte commands `{opcode, addr, value_hi,
  value_lo}` ([host/README.md](../host/README.md#usb-protocol)).
- `radar_system_top` has a parameter `USB_MODE`: 0 = FT601 (32-bit, USB 3.0,
  100 MHz clock), 1 = FT2232H (8-bit, USB 2.0, 60 MHz clock; default and the
  mode Lite uses with an FT232H/FT2232H in 245 synchronous FIFO mode).
- Host side: `radar_protocol.py` provides `FT2232HConnection` (pyftdi) and
  `FT601Connection` (ftd3xx).
- Raw range-profile words are now 256 bins per segment at 6 m spacing (see
  fpga/README limitation k); the packet format itself is the same.
- Open protocol question: FT2232H write path under host back-pressure may lose
  the `0x55` footer (BACKLOG, owner decision pending). `mf_overrun` and the ADC
  overrange flag are not visible to the host (BACKLOG).

## Clocks and resets

| Clock | Frequency | Use |
|---|---|---|
| `clk_100m` | 100 MHz, **the ADC data clock** | whole processing chain, ADC capture on a single edge |
| `clk_120m_dac` | 120 MHz | DAC / TX chirp controller |
| USB clock | 60 MHz (FT2232H) / 100 MHz (FT601) | USB interface |
| MCU core | 64 MHz | firmware |

- `clk_120m_dac` and the USB clock cross into `clk_100m` only through
  `cdc_single_bit` and `cdc_handshake`; there is no Gray-coded multi-bit
  crossing (fpga/README.md).
- `reset_n` is an asynchronous active-low input, synchronised per domain.
  Reset sequencing, ADC-clock phase handling, DAC clock forwarding and CDC
  constraints are board-specific and still open (fpga/README hand-off list).
- The MCU drives DIG4 as the FPGA reset line and an independent IWDG (4 s) as
  its own watchdog.
- Board wrapper, PLL for the 120 MHz clock and pin assignments depend on the
  Cyclone board choice: **TBD**.

## Resource budgets

FPGA (static estimate, `fpga/tb/golden/count_multipliers.py`; no synthesis has
been run, there is no Quartus project yet):

- Multipliers (18x18 equivalents): **38 nominal / 46 worst case**, limit 55.
- RAM/ROM: **241 440 logical bits (235.8 kbit)**, limit 1 Mbit.
- Breakdown tables: [fpga/README.md](../fpga/README.md#resource-budget-static-tbgoldencount_multiplierspy).

MCU (STM32G0B1RET6 has 512 KB flash / 144 KB RAM; the build budget is 128 KB
flash / 32 KB RAM including a 4 KB stack and 1 KB heap reserve), from
[firmware/README.md](../firmware/README.md#memory-report):

| Configuration | Flash (of 131072) | RAM (of 32768) |
|---|---|---|
| default (`ADAR_COUNT=1`, `DIAG=1`) | 24632 | 6332 |
| `DIAG=0` | 17288 | 5908 |
| `ADAR_COUNT=4` | 24808 | 6372 |

## Known limitations

Full lists: [fpga/README.md](../fpga/README.md#known-limitations) (limitations
a-l), [firmware/README.md](../firmware/README.md#not-covered-here) and
[BACKLOG.md](../BACKLOG.md). Main points:

- Nothing has been tested on hardware; the PLL register tables are placeholders
  (lock fails, `FAULT_PLL_LOCK`).
- Detectable range is limited to about 1.5 km (delay under 256 samples); the
  short chirp covers about 78 m.
- Four 64-bin range sets per long chirp reach the Doppler/MTI path, so the
  slow-time axis is not meaningful for the long chirp (kept by owner decision).
- No per-stage FFT scaling; the matched-filter input must stay small
  (`host_gain_shift` / AGC).
- Matched-filter busy time (0.44 ms) must be checked against the real PRI.
- No synthesis, timing constraints or CDC attributes yet; formal proofs were not
  re-run.
- Pin assignments, AF numbers and I2C timing are computed from documentation and
  not checked against the STM32G0B1 datasheet or UM2324 (**VERIFY**).
- PA/LNA gate bias is deliberately conservative (Idq not calibrated); no USB-CDC,
  GPS/IMU wiring or host `STATUS` parser on the MCU side.
- The host GUI still assumes the upstream radar configuration.
