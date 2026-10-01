# AERIS-10 FPGA pipeline - vendor-neutral RTL (track A)

Verilog-2001 radar signal-processing pipeline that synthesizes on Quartus and
Vivado with inferred DSP and RAM only. No vendor primitives and no synthesis
attributes: `run_regression.sh` Phase 0 enforces this with a grep gate, an
`iverilog -Wall` compile without `-DSIMULATION`, and a static resource gate
(`tb/golden/count_multipliers.py`).

Design documents: `docs/superpowers/specs/2026-10-01-rtl-vendor-neutral-port.md`
(spec) and `docs/superpowers/plans/2026-10-01-rtl-vendor-neutral-port.md`
(plan). Follow-up work is in `BACKLOG.md` ("RTL track").

## Signal chain

```
ADC 12-bit CMOS, 100 MSPS, 20 MHz IF (adc_clk = clk_proc = clk_100m)
  |  12 bit offset binary, 1 sample/clk
  v
adc_cmos_interface      sample[11:0], sample_valid, overrange (sticky)
  |  12 bit, 100 MSPS
  v
ddc: nco 20 MHz -> mixer 12x16 -> [27:12] -> CIC N=5 R=4 -> FIR 32-tap (folded, 4-phase)
  |  16 bit I/Q, 25 MSPS (one sample per 4 clk)
  v
rx_gain_control         host gain shift / AGC
  |  16 bit I/Q, 25 MSPS
  v
matched_filter_multi_segment   buffers the whole receive window (1024 x 16 RAM per
  |                            channel), then 4 overlap-save segments of 256
  +-- matched_filter_processing_chain (one segment at a time):
  |     fft_engine (256 pt, 25-bit internal) FFT -> x conj(ref_spectrum_rom) -> IFFT
  |  16 bit I/Q, 256 bins per segment, 1 bin/clk bursts
  v
range_bin_decimator     256 -> 64 bins per segment (peak detect)
  v
mti_canceller           2-pulse canceller (RAM history)
  v
doppler_processor       Hamming window x two 16-pt FFTs (xfft_16 / fft_engine, 24-bit internal)
  v
cfar_ca -> usb_data_interface_ft2232h   (host packet format unchanged)
```

Clocks: `clk_100m` must be the ADC data clock. `clk_120m_dac` (TX) and the USB
clock cross into it through `cdc_single_bit` / `cdc_handshake` only; there is no
Gray-coded multi-bit crossing anywhere.

### Rates and widths at every boundary

| Boundary | Rate | Width (bits) | Notes |
|---|---|---|---|
| ADC pins -> `adc_cmos_interface` | 100 MSPS | 12, offset binary | single-edge capture, 2-edge latency |
| ADC delay -> mixer input | 100 MSPS | 12, signed | MSB inverted, 4-register delay to match the NCO latency |
| NCO | 100 MSPS | 16 (Q15) | 32-bit phase accumulator, `PHASE_INC = 0x3333_3333` (20 MHz) |
| mixer product | 100 MSPS | 28 | 12 x 16 |
| mixer -> CIC | 100 MSPS | 16 | product[27:12], exact bit positions in `ddc.v` |
| CIC integrators / combs | 100 / 25 MSPS | 26 | Hogenauer: 5 x log2(4) + 16 |
| CIC -> FIR | 25 MSPS | 16 | top 16 bits of the last comb (exact >>> 10, unity DC gain) |
| FIR pre-add / product / accumulator | 25 MSPS | 17 / 35 / 40 | 5 guard bits for 32 taps |
| FIR -> gain control | 25 MSPS | 16 | acc >>> 17, saturated |
| gain control -> segmenter | 25 MSPS | 16 | power-of-two shift, saturated |
| segmenter receive window RAM | 25 MSPS in | 16 x 928 used of 1024 | long chirp 928 samples, short chirp 13 |
| FFT data RAM (range) | - | 25 | 16 + 9 bits growth for N = 256 |
| FFT data RAM (Doppler, 16 pt) | - | 24 | |
| FFT twiddles | - | 16 (Q15) | quarter-wave ROM, `fft_twiddle_256.mem` / `_16.mem` |
| conjugate multiply out | 100 MHz bursts | 16 | Q30 rounded >> 15, saturated |
| range profile (per segment) | 1 bin/clk | 16 I + 16 Q | 256 bins, 4 segments per long chirp |
| `range_bin_decimator` | 1 bin/clk in | 16 I + 16 Q | 256 -> 64 per segment (see limitation c) |
| MTI | per bin | 16 | saturating subtract, 2-clock latency |
| Doppler window | per bin | 16 x 16 -> 32 -> 16 | Q15, rounded >>> 15 |
| Doppler frame RAM | - | 2 x 16 x 2048 | 64 range bins x 32 chirps |
| CFAR magnitude / sum / threshold product | per cell | 17 / 23 / 31 | `\|I\|+\|Q\|`, alpha Q4.4 |

## Resource budget (static, `tb/golden/count_multipliers.py`)

Budget: at most 55 multipliers (18x18 equivalents) and at most 1 Mbit of RAM.

**How a product is counted.** Every `*` in synthesizable code is a site; the
sites of each module must match the table in the script (the gate fails if an
unlisted multiplication or memory appears). An a x b product costs
`ceil(a/18) * ceil(b/18)` 18x18 blocks, so 12x16, 16x16 and 17x18 products are
1 block, while the 25 x 16 range-FFT product and the 24 x 16 Doppler-FFT product
are 2 blocks each (one 27x27 DSP mode or two 18x18 blocks). "Nominal" merges
sites with identical operands in mutually exclusive branches (the three
Doppler window multiplies); "worst" counts every site separately. Both must be
within the limit.

| Module | Instances | Product (sites) | 18x18 blocks, nominal / worst |
|---|---|---|---|
| `ddc` mixer | 1 | 12 x 16 (2: I, Q) | 2 / 2 |
| `fir_lowpass` | 2 | 17 x 18 (4 each) | 8 / 8 |
| `fft_engine`, range FFT (`matched_filter_processing_chain`) | 1 | 25 x 16 (4) | 8 / 8 |
| `fft_engine`, Doppler FFT (`xfft_16`) | 1 | 24 x 16 (4) | 8 / 8 |
| `frequency_matched_filter` | 1 | 16 x 16 (4) | 4 / 4 |
| `doppler_processor_optimized` | 1 | window 16 x 16 (6 sites, 2 distinct); 2 address sites `index * 64` are shifts | 2 / 6 |
| `cfar_ca` | 1 | alpha 8 x 23 (1); GO/SO cross products 23 x 7 (4 sites, 2 distinct) | 6 / 10 |
| nco, cic, mti, gain control, decimator, rest | - | none | 0 |
| **Total** | | | **38 / 46** (limit 55) |

| Memory (logical bits) | Bits | Kind |
|---|---|---|
| `ref_spectrum_rom` 2 x 1280 x 16 | 40 960 | ROM |
| `matched_filter_multi_segment` receive window 2 x 1024 x 16 | 32 768 | RAM |
| `matched_filter_processing_chain` sig / prod buffers 4 x 256 x 16 | 16 384 | RAM |
| range `fft_engine` 2 x 256 x 25 + twiddle 64 x 16 | 13 824 | RAM + ROM |
| Doppler `fft_engine` 2 x 16 x 24 + twiddle 4 x 16 | 832 | RAM + ROM |
| `xfft_16` buffers 4 x 16 x 16 | 1 024 | RAM |
| `doppler_processor_optimized` frame 2 x 2048 x 16 + window 16 x 16 | 65 792 | RAM + ROM |
| `cfar_ca` magnitude 2048 x 17 + column buffer 64 x 17 | 35 904 | RAM |
| `mti_canceller` history 2 x 64 x 16 | 2 048 | RAM |
| `plfm_chirp_controller_enhanced` TX LUTs 3600 x 8 + 60 x 8 | 29 280 | ROM |
| `nco` quarter-wave LUT 64 x 16 | 1 024 | ROM |
| `fir_lowpass` coefficients 2 x 16 x 18 | 576 | ROM |
| `fpga_self_test` RAM 64 x 16 | 1 024 | RAM |
| **Total** | **241 440 (235.8 kbit)** | limit 1 048 576 |

Arrays that are flip-flops by design (CIC, FIR delay line, ADC delay, status
words, about 2 kbit) are listed by the script but not counted. The totals are
logical bits; block-RAM granularity (M10K/M20K/RAMB36) rounds individual
memories up but leaves ample margin.

Run `python3 tb/golden/count_multipliers.py` for the full per-line report.

## Removed modules

| File | Reason |
|---|---|
| `ad9484_interface_400m.v`, `adc_clk_mmcm.v`, `adc_clk_mmcm_integration.md`, `tb/ad9484_interface_400m_stub.v`, `tb/tb_ad9484_xsim.v` | 400 MHz LVDS/DDR front end built on IBUFDS/IDDR/BUFIO/MMCM primitives; replaced by `adc_cmos_interface.v` |
| `ddc_400m.v`, `nco_400m_enhanced.v`, `tb/tb_ddc_400m.v`, `tb/tb_nco_400m.v`, `tb/tb_nco_xsim.v`, `tb/tb_ddc_cosim.v` | 400 MHz design with DSP48E1 instances; replaced by `ddc.v`, `nco.v` |
| `cdc_adc_to_processing` (in `cdc_modules.v`), `formal/fv_cdc_adc.*` | Gray coding is only safe for +-1 changes; the 400 -> 100 MHz crossing disappeared with the 400 MHz domain, and the 6-bit chirp counter now crosses with `cdc_handshake` |
| `ddc_input_interface.v`, `tb/tb_ddc_input_interface.v` | 18 -> 16-bit rescale no longer needed (the FIR outputs 16 bits) |
| `latency_buffer.v`, `tb/tb_latency_buffer.v` | 4096 x 32 RAM that only delayed static ROM data; the reference spectrum is addressed by segment instead |
| `chirp_memory_loader_param.v`, `long_chirp_seg*.mem`, `short_chirp_*.mem` | time-domain reference ROMs (mostly zeros); replaced by `ref_spectrum_rom.v` with precomputed spectra |
| `radar_system_top_50t.v` | Xilinx XC7A50T wrapper with LVDS ADC ports and `DONT_TOUCH` |
| `tb/tb_matched_filter_processing_chain.v`, `tb/tb_mf_chain_synth.v`, `tb/tb_mf_cosim.v`, `tb/tb_multiseg_cosim.v`, `tb/cosim/{compare,compare_mf,gen_mf_cosim_golden,gen_multiseg_golden,gen_chirp_mem,validate_mem_files}.py` | tests and scripts of the removed matched-filter memory interface |

## Verification

`bash run_regression.sh` (about one minute, 38 testbenches):

- Phase 0: `iverilog -Wall` with `-DSIMULATION`; the same compile without
  `-DSIMULATION` (synthesizable branches); case-default lint; vendor
  primitive / attribute grep gate; resource gate.
- Golden tests (generators in `tb/golden/gen_*.py`, numpy; vectors committed;
  CI regenerates them and fails on any diff): (a) DDC bit-exact against the
  integer model, full-scale vector included; (b) folded FIR equals the direct
  form bit-exactly; (c) 256-point `fft_engine` against numpy (documented
  tolerance); (d) full chain, range-peak displacement between two target delays,
  on both the behavioral and the synthesizable matched filter.
- The synthesizable matched-filter chain is compiled without `-DSIMULATION` in
  `tb_mf_chain`, `tb_mf_segmenter` and `tb_mf_multiseg`, bit-exact against
  `tb/golden/gen_mf_chain_golden.py`.
- Receiver and system tests (`tb_radar_receiver_final`, `radar_system_tb`,
  `tb_system_e2e`, both USB modes), real-data Doppler and decimator-to-Doppler
  exact-match tests (ADI CN0566 vectors), and unit tests for CIC, NCO, FIR,
  ADC interface, reference ROM, MTI, CFAR, gain control, CDC, USB, mode and
  chirp controllers, edge detector, self-test.
- Formal (`formal/*.sby`: `cdc_handshake`, `cdc_single_bit`,
  `doppler_processor`, `radar_mode_controller`, `range_bin_decimator`): SymbiYosys
  (`sby`) was not installed in the environment where this port was developed,
  so the formal runs were NOT re-executed; CI does not run them either. Only
  the stale twiddle-file entry in `fv_doppler_processor.sby` was updated.
  Re-run with `cd formal && for f in *.sby; do sby -f "$f"; done` where `sby`
  is available.
- Cross-layer contract tests (host / RTL opcodes and packet layout) are
  unchanged and run in their own CI job.

## Known limitations

a. **Matched-filter range window.** Segment `s` is correlated only against
   reference segment `s` (the spectrum of chirp samples `[224 s, 224 s + 256)`),
   so a detectable delay is `d < 256` samples: 10.24 us at 25 MSPS, about
   1.5 km one-way. This is the same instrumented range as the upstream design
   (1024 samples at 100 MSPS). Longer range needs uniformly-partitioned
   convolution (each input segment against all reference segments); see
   `BACKLOG.md`. A full echo inside segment 3 additionally requires
   `d <= 178` (window 928 - chirp 750).
b. **`mf_overrun` is not host-visible.** A chirp start that arrives while the
   segmenter is still busy (about 43 800 clk = 0.44 ms per long chirp, measured
   by `tb_mf_segmenter`) is ignored and sets a sticky flag; the USB protocol was
   deliberately left unchanged, so the flag is visible in simulation only. The
   PRI of long chirps must exceed the busy time.
c. **Four range-bin sets per long chirp.** `range_bin_decimator` runs on each
   segment's 256-bin profile and emits 64 bins per segment, i.e. 4 x 64 outputs
   per long chirp (one set per segment); the upstream design produced one set
   of 64 per chirp. Consequence, from reading `doppler_processor.v`
   (lines 264-268, not simulated with a multi-segment input): the Doppler
   processor and the MTI history treat every 64-bin set as one chirp (the chirp
   index advances after 64 bins), so the four sets of one long chirp occupy four
   consecutive slow-time slots and a 32-slot frame spans 8 long chirps instead
   of 32. The slow-time axis is therefore not meaningful for the long chirp
   until the sets are merged or selected (one set per chirp); this needs a
   design decision and is listed in `BACKLOG.md`.
d. **Stale board files (Decision 8).** `constraints/`, `scripts/50t`,
   `scripts/200t`, `scripts/te0712`, `scripts/te0713`,
   `radar_system_top_te07*_dev.v` and `constraints/README.md` belong to the old
   Xilinx flows and reference removed tops/ports/modules. They are kept for
   reference and are neither built nor tested.
e. **No per-stage FFT scaling.** `fft_engine` outputs saturate at 16 bits when
   the input exceeds roughly 1/16 of full scale; keep the matched-filter input
   small with `host_gain_shift` / AGC. Internal words that would overflow wrap
   (the Python models do the same).
f. **Header comment in `cfar_ca.v`** still quotes upstream resource numbers
   (8 x 21 multiplier); the counted values above are the actual ones (8 x 23 and
   23 x 7).

## Hand-off: what remains board-specific

- Top-level wrapper for the Cyclone dev board: pin assignments and I/O
  standards for the 12-bit ADC bus, DAC bus and FT232H; `clk_100m` driven by the
  ADC data clock (or a PLL locked to it); the 120 MHz DAC clock and clock
  forwarding to the DAC / FT232H pins (`dac_clk`, `ft601_clk_out` are plain
  signals here).
- PLL and reset sequencing (`reset_n` is an asynchronous active-low input).
- Timing constraints (SDC), including input delays for the ADC data pins.
- A Quartus (or Vivado) project: none exists. The `.mem` files (twiddles,
  reference spectra, TX chirp LUT) are read with `$readmemh` by relative name,
  so they must be added to the project and found by the synthesis tool; confirm
  ROM inference on the first real compile.
- The first synthesis run on the chosen device will give the real DSP / RAM
  mapping and Fmax at 100 MHz; the numbers above are static estimates.
