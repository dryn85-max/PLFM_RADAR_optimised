# Backlog

Follow-up work that is out of scope for the current plans.

## G0B1 firmware track

- [ ] **Real PLL register exports.** Replace the placeholder tables in
  `firmware/Core/drivers/pll_tables/` with exports from
  TICS Pro (LMX2594) / ADI ACE (ADF4372), remove `PLL_TABLE_PLACEHOLDER`. Until
  then lock fails and the unit reports `FAULT_PLL_LOCK`.
  Generate the headers with `firmware/tools/regtable_to_h.py`
  (usage in the g0b1 README; it emits `PLL_TABLE_PLACEHOLDER 0`); do not hand-type
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
