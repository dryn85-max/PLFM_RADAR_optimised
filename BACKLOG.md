# Backlog

Follow-up work that is out of scope for the current plans.

## G0B1 firmware track

- [ ] **Real PLL register exports.** Replace the placeholder tables in
  `stm32/Core/drivers/pll_tables/` with exports from
  TICS Pro (LMX2594) / ADI ACE (ADF4372), remove `PLL_TABLE_PLACEHOLDER`. Until
  then lock fails and the unit reports `FAULT_PLL_LOCK`.
  Generate the headers with `stm32/tools/regtable_to_h.py`
  (usage in `stm32/README.md`; it emits `PLL_TABLE_PLACEHOLDER 0`); do not hand-type
  the 113 LMX2594 words.
- [ ] **Verify pins, AF numbers and I2C TIMINGR** (`0x10B17DB5`) against the
  STM32G0B1 datasheet and UM2324; fill in the "Nucleo connector" column of the
  README wiring table.
- [ ] **Confirm on the schematic** that ADTR1107 `CTRL_SW` is driven by the
  ADAR1000 `TR_SW_POS` output (the `SW_DRV_TR_STATE` polarity fix depends on it).
- [ ] **Idq calibration of the PA gate bias on hardware.** The ADTR1107
  contains the PA. Firmware uses safe values (PA ON = PA OFF = `0x5D`, -1.75 V,
  PA pinched; LNA ON `0x00`, OFF `0x68`; all <= `0x6A` = -2.0 V, enforced by
  `_Static_assert` in `Core/drivers/adar1000.h`). Find the real ON value for the
  target quiescent current (DS example: ON `0x39` -> ~220 mA, OFF `0x85`) by
  stepping the bias up from pinch-off while measuring Idq, then raise
  `kPaBiasOperational` (and the `kBiasDacMaxSafe` limit only with the owner's
  agreement). Check that the ADAR1000 `PA_ON` pin is not pulled low on the board.
- [ ] **CS lines idle high before the rails are up.** `hal_gpio_init()` sets the
  ADAR/PLL chip selects high at boot while the chip supplies are still off, which
  can back-power the unpowered chips. Safer: CS low until the supply is on
  (`SEQ_BASE_UP` already raises them after the supply; only the initial level in
  `hal_gpio.c` and the first-frame edge need handling). The e-stop and shutdown
  already drive them low after the supply is cut.
- [ ] **ADAR1000 soft reset is chip-0-global** (matters for `ADAR_COUNT` > 1):
  a reset issued for one device resets the others.
- [ ] **USB-CDC on PA11/PA12** (host link; `RadarSettings` binary packet).
- [ ] **GPS/IMU wiring** (`um982_gps.c` is compiled but not wired).
- [ ] **GUI parser for `STATUS`** lines.
- [ ] **Hardware bring-up checklist** (first power-up, rail order with a scope,
  DIG0..7 against the FPGA, SPI/I2C waveforms, lock detect, IWDG/e-stop drills).
  Include: **external pull-downs fitted on all `EN_*` lines and DIG0..DIG4**
  (MCU pins are high-Z during reset/flashing; the FPGA `reset_n` has an internal
  `PULLUP`, `xc7a50t_ftg256.xdc:121`); ADAR1000 `PA_ON` not pulled low;
  `CTRL_SW` level (receive) measured before the PA rail rises; a deliberate
  watchdog reset leaves `fault=13` latched; **Nucleo
  solder bridges: PB8/PB9 (I2C1) must NOT be tied to A4/A5 (PC1/PC0 = DIG1/DIG0) -
  verify they are open; PA2/PA3 stay routed to the ST-LINK VCP**; connector
  column of the README wiring table checked against UM2324.
- [ ] **First bench run (2026-10-02, bare NUCLEO-G0B1RE, nothing on the pins).**
  Flash via `make flash` (st-flash 1.8.0, chip id 0x467) OK; clean boot prints the
  banner, the placeholder-PLL notice, `PLL **ERR**: lock failed (-116)` and
  `FLT WARN: fault 1 (RF off)`; `status`, `stop` (latch 11, survives RESET, cleared
  by power-cycling the MCU via the IDD jumper) and `ERR latched` for every other
  command behave as specified. Open findings from that run:
  - [ ] **One-off `FAULT_PANIC` (12)** latched on the very first run after the first
    flash; not reproduced on later flashes, resets or power cycles. The latch does
    not record where the panic happened: store the faulting PC/LR (HardFault
    stacked frame) or the `Error_Handler` caller next to the latch and print it on
    the latched boot, so the next occurrence can be diagnosed.
- [ ] **Console: Linux-console F1-F5 and lone Esc (proposal).** `ESC [ [ A..E`
  (Linux-console F1-F5) and a lone Esc leak or eat one character because the CSI
  parser ends on any 0x40-0x7E byte. Harmless in `screen`; decide whether to
  handle the `ESC [ [` form.
- [ ] **Console: async lines mid-typing (proposal).** Asynchronous DIAG/fault lines
  can land in the middle of a typed line and the partial line is not redrawn
  (pre-existing, more visible now that the console echoes).

## ESP32 MVP track

Follow-ups of `esp32/` (ESP32-S3 + HLK-LD2410C, spec
`docs/superpowers/specs/2026-10-02-esp32-ld2410-mvp.md`).

- [ ] **LD2410C settings page** on the live page: maximum gates, per-gate
  sensitivity, hold time (the first version only enables engineering mode).
- [ ] **Rotating radar and PPI display.** Analysis (owner idea, esp32-gps-imu
  brainstorming): azimuth must come from the drive (stepper motor with a homing
  switch, or an encoder), not from the IMU; the angular spacing of the picture
  is limited by the LD2410C output rate (10 frames/s) and its wide beam, not by
  the IMU rate. Needs its own spec (drive, slip ring or cable wrap, PPI page,
  recording of the azimuth).
- [ ] **Magnetometer or BNO085 for azimuth.** The GY-BMI160 has no
  magnetometer, so there is no heading. A BNO085 (or a separate magnetometer)
  would give it; calibration is needed near the electronics (hard/soft-iron).
  The IMU code is already separated from the tilt filter for such an addition.
- [ ] **Tilt zero button** on the live page (stores an offset so the radar's
  mounting error can be cancelled). Needs authentication of the live page first
  (today anyone on the network can open it); tilt is absolute until then.
- [ ] **Radar motion detector** (presence/motion event on the radar data, with
  its own recording and live indication); out of scope of the GPS/IMU cycle.
- [ ] **IMU recording at 100 Hz option.** The recording carries 10 Hz averaged
  `imu` records; a raw 100 Hz option (larger records or a new type, more ring
  buffer traffic) was left out. The rate is one constant in the firmware.
- [ ] **GPS PPS wiring for precise time.** Without PPS the GPS time stamp is
  taken at reception of the RMC sentence (measured 2026-10-02: 128 ms late,
  p1..p99 119..136 ms, against SNTP). Wire
  the module's PPS output to a free GPIO, stamp the edge in an interrupt and
  correct the `time_sync` record.
- [ ] **GPS UTC wrong by whole seconds after the first fix.** Bench 2026-10-02:
  the NEO-6M reported UTC 3 s ahead for 5.6 min after its first fix (likely the
  default leap-second count before the satellites' UTC parameters are decoded;
  VERIFY). The host conversion gives GPS priority, so those records get a UTC
  about 3 s late. Options for the owner: reject a GPS `time_sync` that differs
  from SNTP by more than 1 s (only with Wi-Fi STA); ignore GPS time for the
  first 12.5 min after a fix; ask the receiver via UBX (`NAV-TIMEUTC` validity
  flags), which ends the "factory configuration, no UBX" rule. Also decide
  whether to subtract the measured 128 ms RMC delay.
- [ ] **BMI160 and NEO-6M datasheets into `hardware/datasheets/`**, then verify
  the BMI160 register values, delays and sensitivities in `esp32/main/imu_task.c`
  (marked VERIFY there and in the README) and the NEO-6M default NMEA output,
  baud rate, cold-start figures and PPS behaviour.
- [ ] **Decide A (everything on the ESP32-S3) vs B (hybrid STM32 + ESP32-S3)
  when the RF chain is bought.** Pin-count analysis summary: the ESP32-S3
  N32R8V has about 27-31 usable GPIO (octal flash/PSRAM take GPIO26-37, strapping
  and USB pins are further limited); the minimum need is about 19-25 pins with
  hardware PG->EN chaining of the base power rails; the PA and LNA enables must
  stay on direct MCU pins with pull-downs so they are off while the MCU boots or
  resets. Numbers to be re-checked against the ESP32-S3 datasheet (**VERIFY**).
- [ ] **PA over-temperature sensor choice for the STM32.** Options: A) TMP3x on
  the internal ADC (needs a pin reshuffle); B) I2C LM75 / TMP102; C) AHT only
  for air temperature; D) keep the ADS7830. NTC probes from the ZFC39 kit are an
  option for the probe itself. Needs an owner decision.
- [ ] **Hi-Link LD2410C protocol manual into `hardware/datasheets/`**, then
  verify the parser, the frame decoder, the command codec and the test vectors
  (`esp32/tests/vectors/`, currently synthetic) against it and a real capture
  (engineering frame layout incl. the extra module-specific bytes, ACK
  sequence, frame rate, maximum payload length).
- [ ] **Commit `esp32/dependencies.lock`.** The `espressif/mdns` version is now
  pinned exactly (`==1.14.0`, as resolved by CI) but the lock file is still
  git-ignored and not committed; commit it for fully reproducible builds.
- [ ] **TCP keepalive for the recording server** (only 1 s keep-alive batches and
  5 s socket timeouts today); a half-open connection from a vanished client is
  noticed by the send timeout only.
- [ ] **Optional authentication on the live page** (none in the MVP; anyone on
  the AP or the home network can open it and the recording port).
- [ ] **A stalled WebSocket client blocks the httpd task** (final review,
  2026-10-02): `httpd_ws_send_frame_async` runs synchronously in the httpd task,
  so one stalled browser delays page loads and the other clients for up to the
  5 s send timeout per frame. Options: shorter send timeout, or a per-client
  send task. Proposal, owner decision.
- [ ] **STA retries disturb the fallback AP** (final review, 2026-10-02): in
  AP+STA fallback every STA retry scans channels and the AP follows the STA
  channel, which can disrupt a phone fixing wrong credentials on the AP. Option:
  pause STA retries while a station is connected to the AP. Proposal, owner
  decision.

## RTL track

Context: `fpga/README.md` (known limitations a-l).

- [ ] **One range-bin set per chirp for the Doppler/MTI path (pre-existing
  upstream behaviour; owner decided to keep it as is for now).**
  Upstream `b46dd71` also produced 4 x 64 bins per long chirp (4 segments,
  each 1024 -> 64), so this is not a regression of the port.
  `range_bin_decimator` emits 4 x 64 bins per long chirp (one set per
  matched-filter segment) and `doppler_processor_optimized` /
  `mti_canceller` count every 64-bin set as one chirp (`doppler_processor.v`
  lines 264-268). The four sets of one chirp therefore fill four slow-time
  slots. Options: merge the segments (peak / max-magnitude per bin), keep only
  the segment that matches the expected delay window, or gate the decimator to
  one set per chirp. Write a multi-segment test first (a moving target over
  8 long chirps must produce a Doppler peak at the right bin).
- [ ] **Partitioned convolution for range beyond 1.5 km.** The matched filter
  correlates segment `s` only with reference segment `s`, so the detectable
  delay is < 256 samples (10.24 us, about 1.5 km at 25 MSPS). A longer window
  needs uniformly-partitioned convolution: each input segment against every
  reference segment, with the partial products accumulated in the frequency
  domain (more ROM reads and one more RAM, no more multipliers).
- [ ] **Expose `mf_overrun` and the ADC `overrange` flag to the host.** Both are
  sticky flags that exist in the RTL but have no status-word bit (the USB
  protocol was kept unchanged). Needs an owner decision on the status-word
  layout, then the same change in the GUI parser, the cross-layer contract
  test and the STM32 side if relevant.
- [ ] **Check the matched-filter busy time against the real PRI.** One long
  chirp occupies the segmenter for about 43 800 clk (0.44 ms at 100 MHz); the
  CFAR header quotes a 1932 Hz PRF (0.52 ms). Confirm the long-chirp PRI
  configured by the mode controller leaves margin, otherwise chirp starts are
  dropped (and `mf_overrun` is set). A pipelined butterfly would halve the time.
- [ ] **Quartus project, pin assignments and SDC** once the Cyclone board is
  chosen: top wrapper (ADC data clock as `clk_100m`, PLL for the 120 MHz DAC
  clock, FT232H clock forwarding), I/O standards, input delays, `.mem`
  initialisation files in the project. Then compare the real DSP / RAM / Fmax
  report with the static budget in the README (38 / 46 multipliers, 236 kbit).
- [ ] **Run the formal proofs.** `sby` was not installed where the port was
  made, so `formal/*.sby` (`cdc_handshake`, `cdc_single_bit`,
  `doppler_processor`, `radar_mode_controller`, `range_bin_decimator`) were not
  re-run; only the stale twiddle-file entry in `fv_doppler_processor.sby` was
  fixed. Run them and add a CI job (OSS CAD Suite) if they pass.
- [ ] **Stale Xilinx flows.** Delete or port (now under `legacy/9_Firmware/9_2_FPGA/`)
  `constraints/`, `scripts/50t`, `scripts/200t`, `scripts/te0712`,
  `scripts/te0713` and `radar_system_top_te07*_dev.v` (they reference removed tops/ports); deleting
  hardware flow files needs the owner's go-ahead.
- [ ] **Stale comments in untouched RTL.** `cfar_ca.v` header quotes upstream's
  Vivado-measured resource numbers (8 x 21 multiplier, 1 BRAM; see README
  limitation f); `doppler_processor.v:247` has a case
  without `default` (advisory SYNTH-6 warning in the regression lint).
- [ ] **Doppler window multiplier sharing.** The three window multiplies in
  `doppler_processor.v` have identical operands in exclusive branches; the
  budget assumes the tools share them (2 multipliers). Restructure to one
  explicit multiplier pair if a synthesis report shows 6.
- [ ] **FFT scaling.** `fft_engine` has no per-stage scaling; forward outputs
  saturate when the input exceeds about 1/16 full scale. Consider block-floating
  or per-stage scaling together with the AGC target.
- [ ] **Real-data validation of the overlap-save alignment** (reference segment
  vs signal segment) with the new 12-bit / 100 MSPS front end; all current
  checks use synthetic chirps.
- [ ] **FT2232H write path may lose the 0x55 footer under host back-pressure
  (protocol-affecting, owner decision).** In `usb_data_interface_ft2232h.v`
  the WR_DONE state drops `ft_wr_n` without checking `ft_txe_n`; if the host
  stalls and TXE# deasserts mid-packet, the last byte(s), in particular the
  `0x55` footer, can be lost. Present at upstream `b46dd71`. Fixing it changes
  the write handshake (hold the byte until TXE# is low), so it needs the
  owner's decision. It also needs a back-pressure end-to-end test:
  `tb/tb_system_e2e.v` never raises `ft_txe_n`, so check G5.5 (footer present)
  passes trivially and would not catch the loss.
- [ ] **CDC synchronizer constraints / attributes.** `ASYNC_REG` was removed per
  the vendor-neutral spec; add the target tool's synchronizer attributes and
  CDC timing exceptions when the project is created (README hand-off list).
- [ ] **DAC forwarded clock and ADC clock phase.** The DAC clock is forwarded on
  the same edge as the data (hold risk) and the ADC DCO phase relation is not
  handled in RTL; resolve with the board wrapper, a PLL phase shift and output /
  input delay constraints (README hand-off list).
- [ ] **Short chirp covers only ~78 m.** The 13-sample short-chirp window (0.5 us
  at 25 MSPS) limits the short-chirp range window to about 78 m (upstream: 50
  samples at 100 MSPS). Decide whether that is acceptable for the near-range
  mode.
- [ ] **Run the new cdc_handshake data-integrity property.** The property added
  to `formal/fv_cdc_handshake.v` (PROPERTY 8) and the changed `src_ready`
  equation were written without `sby` available; run them (bmc and cover) and
  fix the wrapper if clk2fflogic timing makes the monitor off by one. The
  simulation stress test in `tb/tb_cdc_modules.v` is the evidence so far.
- [ ] **Adapt the host GUI (`host/`) to the 4-channel prototype and the G0B1
  `STATUS` line.** The V7 GUI is the unchanged upstream one: it assumes the
  upstream channel/beam configuration and does not parse the G0B1 `STATUS`
  line (see also "GUI parser for `STATUS` lines" in the G0B1 track).

## Architecture options (to evaluate, not decided)

- [ ] **Evaluate a low-IF FMCW variant (owner request, 2026-10-02).** Idea: the PLL
  generates the frequency ramp itself (ramp generator in the PLL, e.g. ADF4159;
  whether the LMX2594 already in the BOM can do it is VERIFY against its
  datasheet), an analog mixer de-chirps the echo, and the beat signal (kHz to
  hundreds of kHz, VERIFY for the target range) is sampled by the STM32's own
  12-bit ADC. No 100 MSPS ADC, no chirp DAC, and most of the FPGA chain
  (DDC, matched filter) is not needed. Questions to answer before any decision:
  - *What it gives:* lower BOM cost and power, simpler bring-up, no FPGA
    board / FT2232H path. Range resolution is set by the sweep bandwidth in
    both concepts, so it is not lost.
  - *Conflict with the current concept:* FMCW transmits and receives at the same
    time, while the ADTR1107 front-end is a half-duplex T/R module (TX/RX switch
    on a shared antenna). FMCW would need separate TX and RX antennas/paths
    and enough TX-to-RX isolation, which changes the RF/array design.
  - *Range and power:* the pulsed concept uses PA peak power and pulse
    compression for the ~1.5 km window; continuous-wave FMCW is limited by
    average power, TX leakage and phase noise. Achievable range needs a link
    budget.
  - *Processing:* range FFT (and Doppler/beam processing) would run on the
    MCU; the G0B1 (Cortex-M0+, no FPU) may be too weak, which could mean a
    different MCU.
  - *Replace or complement:* decide whether it replaces the AERIS-10 Lite
    pulsed concept or becomes a separate short-range variant / bench
    prototype. To be decided in a brainstorming session.
