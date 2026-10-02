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
 ADS7830 (temperature)                  <-- I2C1 100 kHz ---------- |  stm32/          |
 power rail enables (7x EN_*)           <-- GPIO ------------------ |                  |
 ST-LINK VCP (USART2, 115200 8N1)       <-> text commands / STATUS  +------------------+
```

Roles:

- **FPGA** (`fpga/`): captures the ADC, down-converts, pulse-compresses, Doppler
  processes, runs CFAR, generates the TX chirp through the DAC and streams
  results to the host.
- **MCU** (`stm32/`): sequences power rails, programs the LO PLL and the
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
returns the whole bus (`stm32/Core/hal/pins.h`, `pins_table.c`). Direction is
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
**VERIFY vs UM2324**): [stm32/README.md](../stm32/README.md#wiring-corehalpinsh-corehalhal_gpioc).
Whether the ADAR1000 SPI also passes through the FPGA (the RTL still has
`stm32_*_3v3` / `*_1v8` pass-through ports) on the Lite board: **VERIFY**.

The MCU command interface (`beam`, `gain`, `tx`, `rx`, `auto`, `status`, `stop`)
and the `STATUS` line format are documented in
[stm32/README.md](../stm32/README.md#serial-command-interface).

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
[stm32/README.md](../stm32/README.md#memory-report):

| Configuration | Flash (of 131072) | RAM (of 32768) |
|---|---|---|
| default (`ADAR_COUNT=1`, `DIAG=1`) | 24632 | 6332 |
| `DIAG=0` | 17288 | 5908 |
| `ADAR_COUNT=4` | 24808 | 6372 |

## MVP (ESP32-S3 + HLK-LD2410C, GPS, IMU)

A bench MVP independent of everything above: **the STM32 firmware and the FPGA
are not part of it** (the RF chain, FPGA board and ADC are not bought yet). It
lives in `esp32/` ([esp32/README.md](../esp32/README.md)); the STM32 firmware
stays in the repo unchanged.

```
 HLK-LD2410C (24 GHz FMCW presence radar)
      | UART1 256000 8N1: TX -> GPIO18 (ESP32 RX), RX <- GPIO17 (ESP32 TX)
      v
 GY-NEO6MV2 GPS: UART2 9600 8N1, TX -> GPIO5, RX <- GPIO4 (optional)
 GY-BMI160 IMU:  I2C 400 kHz, SDA GPIO8, SCL GPIO9, addr 0x68/0x69 (optional)
      v
 +-------------------- ESP32-S3-DevKitC-1 (N32R8V) --------------------+
 | ld2410 task (core 1, prio 5)                                        |
 |   UART -> ld2410_parser (frames) -> ld2410_frame (decode)           |
 |        +--> ring buffer, 4 MiB PSRAM (typed records, one seq)       |
 |        +--> latest snapshot (mutex)                                 |
 |   request queue (depth 1): /ld2410 settings, between frame reads    |
 | gps task (core 1, prio 4): UART2 -> nmea (RMC/GGA)                  |
 |        +--> gps_fix per epoch, time_sync (GPS) per valid RMC        |
 | imu task (core 1, prio 3): I2C -> BMI160 100 Hz -> tilt filter      |
 |        +--> imu record every 100 ms (10 Hz)                         |
 | SNTP (pool.ntp.org, on STA IP) -> time_sync (SNTP)                  |
 | HTTP :80  /  /wifi, /ld2410 (AP only)  /ws  <- 10 Hz snapshot timer |
 | TCP :5410 recording server <- ring buffer batches (core 0)          |
 | Wi-Fi: STA from NVS, fallback AP AERIS-MVP-XXXX; mDNS aeris-mvp     |
 | BOOT (GPIO0), action on release: 2-5 s AP on demand, 5-10 s erase   |
 |   STA credentials, >10 s cancel; zone colour on RGB LED (GPIO38)    |
 | status_led task: RGB LED state (AP / confirmation flashes)          |
 | console / flashing: native USB (USB-Serial-JTAG)                    |
 +---------------------------------------------------------------------+
      | Wi-Fi                                  | Wi-Fi
      v                                        v
 browser: live page (WebSocket)        PC: host/ld2410_rec.py -> .ldrec -> CSV
```

Data flow:

1. The UART reader feeds an incremental parser that resynchronises after
   garbage and keeps the raw bytes of every complete data frame unchanged.
2. Each data frame gets a 32-bit sequence number and the ESP32 time
   (`esp_timer`, us since boot) and is stored in the ring buffer (PSRAM, 4 MiB;
   32 KiB of internal RAM if the PSRAM allocation fails). The decoded fields
   also replace the **latest snapshot**.
2a. **Typed recording stream (protocol v3).** The ring buffer holds typed
   records `{seq u32, esp_time_us u64, type u8, len u16, payload}` with one
   shared sequence: type 0 raw LD2410C frame, 1 `gps_fix` (32 B, one per NMEA
   epoch, also without a fix), 2 `imu` (18 B, 10 Hz: mean raw acceleration and
   rate, pitch, roll), 3 `time_sync` (9 B: UTC microseconds and source). The
   GPS, IMU and SNTP code push into the same ring from their own tasks; the
   transport is type-agnostic, so GAP, keep-alive, `boot_id` and resume work
   unchanged for every type. A missing or silent GPS or IMU never touches the
   LD2410C path.
2b. **Time.** UTC is carried by `time_sync` records: GPS on each RMC with status
   `A` and valid time and date (no PPS: measured 128 ms late), SNTP
   on each synchronisation over the STA link. Both are recorded; the live page
   shows GPS while its latest sync is under 5 s old, else SNTP. The PC converts
   `esp_time_us` to UTC with a GPS `time_sync` of the same boot within +-2 s, else
   the nearest preceding `time_sync` of any source; a GPS sync more than 1 s off
   the nearest SNTP sync of its boot is rejected (whole-second UTC error of the
   NEO-6M after a cold-start fix).
2c. **Tilt.** Pitch and roll are absolute (complementary filter on the gravity
   direction; body frame +X boresight, +Y left, +Z up; sensor-to-body mapping is
   a firmware constant). There is no azimuth: no magnetometer.
3. **Live view:** a 10 Hz timer builds the snapshot JSON and sends it to each
   WebSocket client; a client whose previous send is still pending is skipped,
   so a slow client sees only the newest snapshot (latest-only, no queue). The
   snapshot also carries the GPS state, the tilt and the current time source
   (GPS and Tilt cards on the page).
4. **Recording:** the TCP server on port 5410 reads a 12-byte request with
   `from_seq` and sends batches about once per second (immediately while more
   records are pending) from the ring buffer. A `GAP` flag says the requested
   start was evicted. Every batch carries a `boot_id` (random per boot) because
   the sequence restarts at 0 after a reboot. One client at a time. The PC
   recorder writes every record plus PC time to a `.ldrec` v3 file (v2 files
   remain readable) and exports CSV: radar frames with frame UTC, time source,
   the latest GPS fix and the latest pitch/roll, plus optional per-sensor CSVs
   (`--gps`, `--imu`). Unknown record types are kept raw (file type 6).

5. **LD2410C settings (`/ld2410`).** The LD2410C task owns UART1. The HTTP
   handler submits a request (read, write, Bluetooth off, restart, factory
   reset) and waits up to 5 s; the task takes it from a depth-1 queue with a
   zero-wait poll **between frame reads**, so there is **one request at a time**
   (callers are serialised) and no cost for the frame path while nothing is
   pending. A request runs enable-config, its commands (each ACK checked) and
   end-config (also after a failure; not after a restart, the module is
   rebooting). Data frames pause while it runs (up to about 1 s, VERIFY). After
   a restart the existing engineering-mode recovery re-enables engineering
   mode. Changes are logged on the console only; the recording protocol is
   unchanged.

Wi-Fi modes: STA when credentials are stored and the connection succeeds within
15 s (AP off); otherwise AP+STA (AP `AERIS-MVP-XXXX`, WPA2, random 12-character
password kept in NVS and printed to the console at every boot, STA keeps
retrying); with no credentials AP only. The `/wifi` and `/ld2410` pages are
served only to clients on the AP (no password: the AP's WPA2 password is the
protection); the live page and port 5410 stay open on the home network.

**AP on demand.** Daily work is on STA. Releasing BOOT after 2-5 s switches
STA to AP+STA (the AP uses the STA's channel; the password line is printed
again). The AP clients are counted from the Wi-Fi events; the on-demand AP goes
back to STA-only 10 minutes after the last client left (never while one is
connected). The fallback AP is unchanged and never times out.

**BOOT and the status LED.** The BOOT monitor task polls GPIO0 every 50 ms and
feeds a hardware-independent zone tracker (`boot_btn`): under 2 s nothing, 2-5 s
AP on demand, 5-10 s erase the STA credentials and restart, 10 s or more cancel;
the action fires on release. A button already down at start is ignored until
released once (GPIO0 low at reset is the ROM download mode). The `status_led`
task owns the on-board RGB LED (GPIO38, RMT driver, 5 % brightness) and picks the
colour from a pure function (`status_led`), by priority: button zone colour,
confirmation flashes (3 x), AP only (blue steady), AP on demand (blue slow
blink), off.

Not verified on hardware yet; see the VERIFY list in
[esp32/README.md](../esp32/README.md) (the GPS and IMU parts have never run on
the boards; the BMI160 register values are unverified). Open follow-ups
(rotating radar and PPI display, recording protocol v4, magnetometer, PPS, all-on-ESP32
vs hybrid STM32 + ESP32) are in [BACKLOG.md](../BACKLOG.md).

## Known limitations

Full lists: [fpga/README.md](../fpga/README.md#known-limitations) (limitations
a-l), [stm32/README.md](../stm32/README.md#not-covered-here) and
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
