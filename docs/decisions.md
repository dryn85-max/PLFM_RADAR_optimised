# Decisions log

Dated log of owner decisions for AERIS-10 Lite, newest last within each date
group. Each entry has a one-line reason taken from the repository (commit
messages, specs, module READMEs); "reason not recorded" means the repository
does not state one. SHAs are commits on this fork. PR #2 is the merge
`623086b` (vendor-neutral RTL port, track A, and STM32G0B1 firmware port, track
B). Fork baseline: upstream `b46dd71`.

## 2026-10-01 - firmware (STM32G0B1)

| Decision | Reason | Commit |
|---|---|---|
| **Q1: `auto` command** returns all ADAR1000 devices to TR-pin mode (FPGA owns TX/RX switching); `tx` / `rx` force SPI mode for bench tests. | Upstream misused `TR_SOURCE` (reg 0x031 bit 2 selects SPI vs TR-pin, not "TX"); pin mode is the normal operating mode, so there must be a way back to it. | `a935263` |
| **Q2: elevation limited to +-60 degrees** (`beam <az> <el>`, `el` -60..60; `az` -180..180, stored only). | Reason not recorded in the repository beyond the command contract; the array is steered electronically in elevation only. | `a935263` |
| **Q3: `gain <ch> <val>` is a direct write** of the RX VGA register (0..127) that also updates the AGC cache. | Keeps the AGC's record of what was written equal to the hardware; the value sticks only while the FPGA AGC is disabled (DIG6 low), otherwise the AGC overwrites it within 250 ms. | `a935263` |
| **Safe PA/LNA bias:** PA ON = PA OFF = `0x5D` (-1.75 V, PA pinched), LNA ON `0x00`, LNA OFF `0x68`; every PA/LNA bias constant limited to `0x6A` (-2.0 V) by `_Static_assert`. | The real quiescent current is unknown until Idq is calibrated on hardware; start pinched so the PA cannot be damaged. | `f1aa4c1` |
| **Boot gains:** TX VGA `0x7F` on all channels, RX VGA = AGC base (30); ADAR set to SPI RX mode right after safe bias, before the RF rails come up. | `agc.written[]` must match the hardware; `CTRL_SW` must be in receive before the PA rail rises. | `f1aa4c1` |
| **Watchdog / NMI reset latches `FAULT_WATCHDOG` (13)**; NMI and unexpected interrupts call `fault_panic()`; the latch is stored before the e-stop and the first code is kept. | A hung or crashed MCU must come back in the safe, rails-off state instead of re-energising the PA; upstream's `Error_Handler` reset and re-energised the rails. | `acd8e76` |
| **`EN_PA` moved from PA9 to PC8.** | Removes the PA9/PA11 pad remap risk on STM32G0 (approved by the coordinator). | `8aba250` |
| **E-stop and shutdown drive DIG0-2 and DIG4 low before `EN_FPGA`; chip selects and CE go low only after the chip's supply is off.** | A high MCU output into an unpowered FPGA/ADAR1000/PLL would back-power it through its input protection. | `a935263`, `acd8e76` |

## 2026-10-01 - FPGA (vendor-neutral RTL)

| Decision | Reason | Commit |
|---|---|---|
| **Range FFT `INTERNAL_W` = 25** (spec said 24); the 16-point Doppler FFT keeps 24. Do not revert. | Full-scale I and Q reach sqrt(2) * 2^23 and wrap a 24-bit word; golden test (c) fails with 24 bits. | `a5a524a` |
| **Matched-filter segmenter buffers the whole receive window** (1024 x 16 RAM; long chirp 928 samples, short chirp 13) and then processes the four segments from the buffer. | Samples arriving while segments 0..2 were processed were dropped, so only segment 0 was contiguous (owner-approved design change). | `59a1397` |
| **Keep four 64-bin range sets per long chirp** (one per matched-filter segment), as upstream did. | Pre-existing upstream behaviour, not a regression of the port; the open design question (merge, select or gate) stays in BACKLOG. | `5b22a38` |
| **Process every chirp** (both toggle edges of `mc_new_chirp`). | Upstream detected only the rising edge of a toggle, so every second chirp was silently dropped (an upstream defect). | `59a1397`, documented in `5b22a38` |
| Keep the USB packet format unchanged (so `mf_overrun` and ADC overrange stay invisible to the host for now). | Protocol-affecting changes need an owner decision on the status-word layout (BACKLOG). | `5b22a38` |

## 2026-10-02

| Decision | Reason | Commit |
|---|---|---|
| **ISC004 lint patch included in PR #2** (parenthesised implicit string concatenations in `compare_doppler.py` and `v7/dashboard.py`). | Four pre-existing findings that also fail on `main`; no behaviour change; included in PR #2 at the owner's request. | `6872d2b` |
| **`docs/superpowers/` (specs, plans, notes) is kept in git.** | Owner decision in the restructure brainstorming (reason not recorded beyond that); historical specs and plans are not rewritten when paths change. | `de18760` |
| **Restructure into AERIS-10 Lite layout:** upstream-only material moves to `legacy/` keeping its original relative paths (nothing deleted). | The project has diverged from upstream (4-channel prototype, vendor-neutral RTL, G0B1 firmware); most upstream material no longer applies. History is preserved with `git mv`. | `de18760`, `3b30597` |
| **Semantic top-level names:** `fpga/`, `firmware/`, `host/`, `hardware/`, `docs/`, `tools/`, `tests/`, `legacy/`. | Replace the numbered upstream layout (`1_Project_Description` ... `9_Firmware`) with names that say what is inside. | `cc4fe56`, `c05835b`, `a02a7ca`, `429da93` | <!-- path-gate: legacy-ref -->
| **V7 GUI moves to `host/`; older GUIs go to `legacy/`.** Adapting the GUI to the 4-channel prototype and the G0B1 `STATUS` line is a BACKLOG item. | V7 is the GUI the prototype uses; the adaptation is explicitly out of scope of the restructure. | `a02a7ca` |
| **Documentation is Markdown in `docs/`;** the upstream HTML site and PDFs go to `legacy/docs-site/`. | The upstream HTML docs site describes the upstream system and does not apply (restructure spec context). | `de18760`, `3b30597` |
| **Project name: AERIS-10 Lite;** upstream attribution and licences (MIT software, CERN-OHL-P hardware, `Licence` file) kept. | The project has diverged into a 4-channel prototype (restructure spec context). | `de18760` |
| **Language of README and `docs/` is English;** `docs/bom-optimization.md` stays Russian as a historical document. | It is a historical analysis of the upstream design. | `de18760` |
| **Protocol unit tests restored as `host/test_radar_protocol.py`** (the non-Tk classes of the legacy Tk GUI test file), run in the CI Python job; the legacy file stays untouched and non-importable. | The restructure had dropped the only unit tests of `radar_protocol.py`; the Tk GUI itself is not used by Lite. | `09a29d8` |
| **GitHub Pages is not enabled** (the upstream HTML site is archived in `legacy/docs-site/`). | Owner decision in the restructure final-review round; no Pages site for Lite. | reason not recorded beyond that |
| **Firmware boot banner renamed to `AERIS-10 Lite G0B1 boot`.** | Owner request after the first bench run: the banner still carried the pre-restructure name. | this change |
| **Console echo is on by default;** `echo on` / `echo off` switches it, `echo` alone reports `ECHO on|off`; accepted while a fault is latched (like `status`); the state is not persisted. | Owner decision after the first bench run: typed commands were invisible in `screen`. Spec `docs/superpowers/specs/2026-10-02-console-echo-backspace.md`. | this change |
| **Command-line editing:** BS (0x08) and DEL (0x7F) delete the last character; all other control characters and ANSI escape sequences (arrow keys) are discarded and never enter the line. | Owner decision after the first bench run: a Backspace-corrected typo reached the parser as garbage (`status` was answered `ERR latched`). | this change |
| **`DIAG` log lines end with `\r\n`** (were `\n` only). | Owner request after the first bench run: `screen` showed the log as a staircase; banner and command replies already used `\r\n`. | this change |
| **`firmware/` renamed `stm32/`** alongside the new `esp32/` folder (ESP32-S3 + LD2410C MVP). | Owner decision in the esp32-ld2410-mvp brainstorming: with two MCU targets in the tree, `firmware/` no longer says which one it holds. The row above that lists `firmware/` among the semantic names is historical. | this change | <!-- path-gate: legacy-ref -->
| **MVP = ESP32-S3 + HLK-LD2410C, no STM32, no FPGA.** The STM32G0B1 firmware stays in the repo unchanged (frozen). The choice between "everything on the ESP32-S3" (A) and "hybrid STM32 + ESP32-S3" (B) is postponed until the RF chain is bought (BACKLOG). | The RF chain, FPGA board and ADC are not bought; the owner wants a working radar MVP from bench parts. The LD2410C is a ready 24 GHz FMCW presence radar with UART output. Spec `docs/superpowers/specs/2026-10-02-esp32-ld2410-mvp.md`. | this change |
| **Our own FMCW stays a BACKLOG option; the ESP32-S3 was chosen so that path stays open.** | Owner decision in the esp32-ld2410-mvp brainstorming (see also the low-IF FMCW item in BACKLOG). | this change |
| **MVP framework: ESP-IDF in C, in a separate top-level folder `esp32/`.** | Owner decision in the brainstorming (reason not recorded beyond that). | this change |
| **User interface: a web page served by the ESP32 (live view) plus recording on the PC.** | Owner decision in the brainstorming: nothing to install for the live view, recording survives on the PC. | this change |
| **Recording format: raw LD2410C frames byte for byte** plus frame number, ESP32 time and PC time, in a binary file; CSV is exported from the raw file. | Raw bytes keep every field for later re-decoding if the decoder changes (the frame layout is still VERIFY). | this change |
| **Live view sends the latest snapshot at a fixed 10 Hz (no queue); recording sends batches** with frame numbers from a PSRAM ring buffer and resumes after a reconnect. | A slow viewer must never delay or lose recording; recording must survive Wi-Fi drops. | this change |
| **Wi-Fi: STA with fallback AP.** The AP is WPA2 with a random password generated on the first boot, kept in NVS and printed to the console at every boot. The `/wifi` setup page is reachable only through the AP. Wi-Fi reset: BOOT held >= 5 s while running (not at power-up: GPIO0 low at reset selects the ROM download mode). | Secrets stay out of git; the setup page must not be exposed on the home network. | this change |
| **Console and flashing through the native USB port (USB-Serial-JTAG, GPIO19/20).** | The UART-bridge connector of the owner's board was re-soldered and is not relied on. | this change |
| **First MVP version: no LD2410C configuration** (except enabling engineering mode at start) and no GPS/IMU. | Keeps the first version small; the settings page and GPS NEO-6M / IMU BMI160 are BACKLOG items. | this change |
| **ESP-IDF v5.5.5 is pinned** (`espressif/idf:v5.5.5`). | Latest v5.5 patch release on Docker Hub on 2026-10-02. | this change |
| **The ESP32 build is verified by CI on a draft PR** (job `esp32-mvp`). | The agent container has no Docker or ESP-IDF (and no `sfw`), so only the host tests run locally; `idf.py build` runs in CI. | this change |
| **Recording protocol v2 with `boot_id`** (batch header 20 B; file records type 2 = reboot). A changed `boot_id` resets the recorder's sequence baseline: no gap record, and the recorder reconnects from sequence 0 of the new boot. | Coordinator fix: the sequence restarts at 0 after an ESP32 reboot, so a recorder resuming from the old sequence would drop the new boot's frames as "older" or log a false gap. | this change |
| **Wi-Fi scan list on /wifi implemented** (owner decision after the final review found it missing). Scan on every GET of `/wifi` (about 2 s, brief AP disruption accepted); no-credentials mode now runs AP+STA with an idle STA so the driver can scan. | Spec R6 requires a scan list; it was not implemented. | this change |
| **GPS NEO-6M and IMU BMI160 added to the ESP32 MVP; goals: (1) UTC time and position in the recording, (2) GPS and tilt on the live page, (3) radar tilt (pitch/roll).** Not now: azimuth/magnetometer, target map, motion detector. | Owner decision in the esp32-gps-imu brainstorming; the owner has a GY-NEO6MV2 and a GY-BMI160 (no magnetometer) on the bench. Spec `docs/superpowers/specs/2026-10-02-esp32-gps-imu.md`. | this change |
| **No magnetometer in this cycle;** the IMU code is written so that one can be added later (BNO085 suggested if the owner orders one). | Only a 6-axis BMI160 is on the bench; azimuth needs a magnetometer (BACKLOG). Spec `docs/superpowers/specs/2026-10-02-esp32-gps-imu.md`. | this change |
| **GPS and IMU are recorded as separate typed records in the same stream (recording protocol v3);** v2 files stay readable by the recorder. | One shared sequence keeps GAP, resume and reboot handling unchanged, and each sensor keeps its own rate and payload. Spec `docs/superpowers/specs/2026-10-02-esp32-gps-imu.md`. | this change |
| **IMU is recorded at 10 Hz** (100 ms averages of ~100 Hz sampling); the rate is a single constant. | Enough for tilt and a modest file size; a 100 Hz recording option is in BACKLOG. Spec `docs/superpowers/specs/2026-10-02-esp32-gps-imu.md`. | this change |
| **UTC source: GPS, and SNTP when GPS has no fix** (STA mode with internet); every time sync records its source. | The bench has no guaranteed sky view indoors; SNTP over the home Wi-Fi gives a time stamp meanwhile. Spec `docs/superpowers/specs/2026-10-02-esp32-gps-imu.md`. | this change |
| **Tilt is absolute (relative to the horizon);** the IMU-to-radar axis mapping is a firmware constant (`TILT_MAP_*` in `tilt.h`) documented in the README. No "zero tilt" button in this cycle. | A zero button needs authentication on the live page (BACKLOG); a constant is enough for the bench. Spec `docs/superpowers/specs/2026-10-02-esp32-gps-imu.md`. | this change |
| **Rotating radar / PPI display is out of scope,** noted in BACKLOG with the analysis: azimuth must come from the drive (stepper + homing, or an encoder); angular spacing is limited by the LD2410C 10 frames/s and its wide beam, not by the IMU rate. | Owner idea in the brainstorming; recorded so the analysis is not lost. Spec `docs/superpowers/specs/2026-10-02-esp32-gps-imu.md`. | this change |
| **`imu` record payload is 18 bytes** (the first plan draft said 19). | The fields sum to 18: 3 x i16 + 3 x i16 + i16 + i16 + u8 + u8; plan arithmetic corrected, C, Python and vectors use 18. Plan `docs/superpowers/plans/2026-10-02-esp32-gps-imu.md`. | this change |
| **(Owner-confirmed 2026-10-02.) File record type 6 "unknown"** in `.ldrec` v3 (`seq, esp_time_us, pc_time_ns, wire_type u8, len u16, payload`). | The ESP32 transport is type-agnostic, so the recorder must keep a record type it does not know instead of failing; `info` counts them. Types 0-5 are as in the plan; 6 was added in the implementation. | this change |
| **(Owner-confirmed 2026-10-02.) GPS time from an RMC sentence with status `V` is not used for `time_sync`** (position and date likewise need status `A`; GGA position needs fix quality > 0). | A receiver without a fix can report a free-running RTC time or the GPS epoch date, which must not become a UTC time stamp (conservative rule in `nmea.h`). | this change |
| **Both time sources are recorded (GPS and SNTP `time_sync` records); GPS has priority only for display** (live page: GPS while its latest sync is under 5 s old, else SNTP). | Recording everything lets the host decide; the host converts with GPS priority (see the next row). `components/core/time_source.h`. | this change |
| **Live page marks the IMU `error` when its last record is older than 1 s** (owner-confirmed 2026-10-02). | Agent-made choice in the implementation, confirmed by the owner; spec R6. | this change |
| **GPS priority also in the host UTC conversion, +-2 s window** (owner decision 2026-10-02, spec R4): a GPS `time_sync` of the same boot within +-2 s of a record wins (nearest such one); otherwise the nearest preceding `time_sync` of any source, else the following one. Firmware also re-emits an SNTP `time_sync` every 60 s once SNTP has synced; RMC dates before 2025 are rejected. | An SNTP sync between two GPS syncs must not make `frame_utc` jump; hourly SNTP alone would leave recordings without a sync. `host/ld2410_rec.py` `GPS_PRIORITY_WINDOW_US`. | this change |
| **Default IMU mounting = BMI160 chip facing down** (`TILT_MAP_SIGN` +1, -1, -1: sensor turned 180° about X). | Owner's bench board has the chip on the underside; with the identity mapping it read roll ≈ ±180° at rest (bench, 2026-10-02). | this change |

## Process decisions (earlier)

| Date | Decision | Reason | Commit |
|---|---|---|---|
| 2026-10-01 | Development follows superpowers (brainstorming, spec, plan, subagent-driven development, final review) with explicit models per stage; `main` only via PR. | Defined in [AGENTS.md](../AGENTS.md); fitted to this fork (no `develop` branch) in `97bbfa2`. | `007a8f5`, `97bbfa2` |
