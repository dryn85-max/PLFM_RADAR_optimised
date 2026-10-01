# Spec: Vendor-neutral RTL port of the AERIS-10 radar pipeline (track A)

Date: 2026-10-01. Owner: Andrii. Status: approved for planning.

## Context

The upstream FPGA design (`9_Firmware/9_2_FPGA/`, ~13 k lines Verilog-2001) targets a Xilinx
XC7A50T and is built around a 400 MSPS, 8-bit AD9484 ADC on a 120 MHz IF. The new prototype
(4 channels, 1× ADAR1000) will use an Intel/Altera Cyclone dev board (exact model TBD), a
**12-bit CMOS-parallel ADC at ~100 MSPS on a ~20 MHz IF** (single LO shared by TX and RX),
an 8-bit CMOS-parallel DAC at 100–125 MSPS for the 10–30 MHz chirp, and an FT232H in 245
synchronous FIFO mode for the host link.

The full analysis is in `BOM_OPTIMIZATION_REPORT.md` (sections 1 and 5).

## Goal

Make the radar signal-processing RTL synthesizable on any vendor (Quartus and Vivado) with
inferred DSP and RAM, remove the 400 MHz ADC domain, and apply the resource optimizations
that make the design fit a mid-size Cyclone (≤ 55 multipliers 18×18, ≤ 1 Mbit RAM) — while
keeping the existing Icarus regression (`run_regression.sh`) and real-data co-simulation
bit-exact where the datapath is unchanged, and adding golden-model checks where it changes.

## Non-goals

- No Quartus project, pin assignment or board-specific top yet (board model unknown).
- No changes to the host USB protocol (packet format of `usb_data_interface_ft2232h.v`).
- No changes to CFAR, MTI, Doppler algorithms beyond resource fixes listed below.
- No STM32 interface changes (DIG0–7 GPIO contract stays).

## Requirements

### R1. Remove Xilinx primitives; everything inferred

- No instances of `DSP48E1`, `xpm_memory_*`, `IBUFDS`, `IDDR`, `ODDR`, `BUFG`, `BUFIO`,
  `MMCME2_*`, `PLLE2_*` remain in files compiled by the pipeline build (bring-up tops for
  TE07xx may be deleted or left out of the file list; they are not ported).
- Multipliers written as `a * b` on signed regs with explicit pipeline registers so both
  Quartus and Vivado infer DSP blocks. Vendor synthesis attributes (`USE_DSP`, `ram_style`,
  `rom_style`, `DONT_TOUCH`, `KEEP`, `ASYNC_REG`) are removed or replaced by a single
  `(* ... *)` block that both tools ignore gracefully. Quartus-specific attributes may be
  added later; the port must not depend on them.
- RAMs/ROMs written as inferable dual-port arrays (`reg [W-1:0] mem [0:D-1]`, synchronous
  read, one write port per always block), per Intel "Recommended HDL Coding Styles".
- Lint gate: `verilator --lint-only -Wall` (or `iverilog -g2001 -Wall`) passes with no
  errors on the pipeline file list.

### R2. Replace the ADC front end

- Delete `ad9484_interface_400m.v` and `adc_clk_mmcm.v` from the pipeline.
- New `adc_cmos_interface.v`: parameters `DATA_W = 12`; inputs `adc_clk` (ADC DCO, ≤ 125 MHz),
  `adc_data[DATA_W-1:0]`, `adc_ovr`; outputs a registered sample stream `sample[DATA_W-1:0]`,
  `sample_valid` in the `adc_clk` domain, plus a sticky, clearable `overrange` flag.
  Single-edge capture; no DDR.
- The processing clock becomes the ADC clock (`clk_proc = adc_clk`, nominal 100 MHz), so the
  400→100 MHz CDC (`cdc_adc_to_processing`, Gray-coded data — unsafe) is deleted. If a
  separate clock is still required by the top, the crossing must be a dual-clock FIFO, never
  Gray coding of arbitrary data.

### R3. Re-parameterize the DDC for a 20 MHz IF at 100 MSPS

- NCO (`nco_400m_enhanced.v` → rename `nco.v`): parameter `PHASE_INC` default for
  f_IF = 20 MHz at f_s = 100 MHz (`0x3333_3333`). Phase accumulator width stays 32 bits.
  Quarter-wave LUT stays; output width 16 bits.
- Mixer: `DATA_W` (12) × 16-bit NCO → single inferred multiplier per I/Q; truncation
  documented with the bit positions.
- CIC (`cic_decimator_4x_enhanced.v`): keep 5 stages, R = 4, M = 1 but recompute register
  widths from `DATA_W` with the Hogenauer formula (`B_max = N·log2(R·M) + B_in`), no 48-bit
  integrators. Integrators written as `+` on signed regs; no explicit DSP primitives.
- FIR (`fir_lowpass.v`): symmetric 32-tap, **folded** — 16 multipliers per channel using
  pre-adders, 32 total. Accumulator width = product width + log2(taps) guard bits (fix the
  existing missing-guard-bit defect). Coefficients unchanged (designed for 25 MSPS).
- **New second decimation ×4 after the CIC and before the FIR** (`decimator_4x.v`, simple
  sample-drop with valid gating), so the FIR and everything downstream run at 25 MSPS
  (one sample every 4 `clk_proc` cycles). FIR may therefore be time-multiplexed: 8 physical
  multipliers per channel processing 4 phases, if the implementer finds it simpler; either
  way ≤ 32 multipliers for both channels.

### R4. Shrink the pulse-compression chain

- Range FFT size `N_FFT` becomes a parameter, default **256** (30 µs long chirp at 25 MSPS
  = 750 samples → 3 overlap-save segments of 256 with overlap 32 / advance 224; or whatever
  the implementer derives — document the arithmetic in the module header).
- `fft_engine.v`: `INTERNAL_W` parameter default **24** (was 32); twiddles 16 bits; complex
  multiply as 3 real multiplies (Gauss) or 4 — implementer's choice, documented; ≤ 4
  multipliers per engine at 24-bit.
- Reference-chirp spectrum stored precomputed in ROM (`ref_spectrum_rom.v`, generated by a
  Python script `tb/golden/gen_ref_spectrum.py` into a `.mem` file); the per-segment
  "reference FFT" pass is removed. One engine does signal FFT + inverse FFT only.
- Delete `latency_buffer.v`; the reference ROM is addressed with an offset instead.
- Short-chirp reference ROM sized to its real length (≤ 64 entries), not 1024.
- Range-bin decimator keeps producing 64 bins (256 → 64).

### R5. Small resource fixes

- `mti_canceller.v`: history arrays rewritten so they infer RAM (synchronous reset of
  pointers only; no reset on array contents).
- Remove debug-only counters that drive no output (`sample_counter` in the DDC,
  `frame_counter` in the receiver, CIC monitors) unless they feed the status packet.
- `cfar_ca.v`: no algorithm change; fix the header comment to state the real LUT cost.

### R6. Verification

- `run_regression.sh` keeps passing. Tests that depended on deleted modules
  (`tb_ad9484_xsim.v`, `tb_latency_buffer.v`, CDC formal for the removed crossing) are deleted
  with their entries; no silent skips.
- The regression must exercise the **synthesizable** matched-filter path: compile without
  `-DSIMULATION` for `tb_mf_chain_synth` (or add the equivalent) and add it to the script.
- New golden-model tests (Python, numpy, in `tb/golden/`): (a) DDC+2× decimation output vs
  numpy model for a synthetic 20 MHz-IF chirp; (b) folded FIR vs direct-form FIR
  bit-exact; (c) 256-point FFT engine vs numpy FFT within a documented tolerance;
  (d) full chain range profile peak location for a synthetic target delay. Each is a
  Verilog testbench reading `.hex` vectors generated by a script, same pattern as
  `tb/cosim/`.
- Formal (`formal/*.sby`) files for retained modules keep passing with `sby`; obsolete ones
  are deleted.
- CI (`.github/workflows/ci-tests.yml`) runs the updated regression.

### R7. Documentation

- `9_Firmware/9_2_FPGA/README.md` (create if absent): block diagram of the new chain with
  sample rates and bit widths at every boundary, the multiplier/RAM budget table per module,
  and the list of removed modules with reasons.

## Acceptance

- `./run_regression.sh` → all PASS, including the synth matched-filter test and 4 new golden
  tests.
- `grep -lE "DSP48E1|xpm_memory|IBUFDS|IDDR|ODDR|MMCME2|BUFG|BUFIO" <pipeline file list>` → empty.
- A static count (`tb/golden/count_multipliers.py` or by inspection, documented in README)
  shows ≤ 55 inferred multipliers and ≤ 1 Mbit of inferred RAM for the pipeline.
- Hand-off note lists exactly what remains board-specific (top-level wrapper, PLL, pins).
