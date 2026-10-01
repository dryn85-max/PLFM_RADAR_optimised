# RTL Vendor-Neutral Port (Track A) Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Make the AERIS-10 radar signal-processing RTL in `9_Firmware/9_2_FPGA/` synthesizable on Quartus and Vivado with inferred DSP/RAM only, replace the 400 MSPS LVDS ADC front end with a 12-bit CMOS ADC at 100 MSPS / 20 MHz IF, and shrink the pulse-compression chain so the design fits ≤ 55 multipliers and ≤ 1 Mbit RAM — with the Icarus regression passing after every task.

**Architecture:** The processing clock becomes the ADC clock (100 MHz). ADC → `adc_cmos_interface` → `ddc` (NCO 20 MHz + 12×16 mixer + CIC R=4 → 25 MSPS + folded 32-tap FIR) → `rx_gain_control` → `matched_filter_multi_segment` (256-pt segments, reference spectrum from ROM, one `fft_engine` doing FFT + IFFT) → `range_bin_decimator` (256→64) → `mti_canceller` → `doppler_processor` → `cfar_ca` → USB. All Xilinx primitives and synthesis attributes are removed; memories are written as inferable arrays.

**Tech Stack:** Verilog-2001, Icarus Verilog (`iverilog -g2001`), Python 3.12 + numpy for golden generators, bash `run_regression.sh`, SymbiYosys (optional, formal), GitHub Actions.

**Spec:** `docs/superpowers/specs/2026-10-01-rtl-vendor-neutral-port.md` (read it first). Background: `BOM_OPTIMIZATION_REPORT.md` §1 and §5.

## Global Constraints

- Verilog-2001 only (`iverilog -g2001`); no SystemVerilog, no `$clog2` (write `LOG2_*` parameters explicitly).
- No `DSP48E1`, `xpm_memory_*`, `IBUFDS`, `IDDR`, `ODDR`, `BUFG`, `BUFIO`, `MMCME2_*`, `PLLE2_*` in any file of the pipeline list (`PROD_RTL` in `run_regression.sh`). These words must not appear even in comments of pipeline files — the final grep gate is `grep -lE "DSP48E1|xpm_memory|IBUFDS|IDDR|ODDR|MMCME2|PLLE2|BUFG|BUFIO"` and it must return nothing.
- No vendor synthesis attributes in pipeline files: `ASYNC_REG`, `USE_DSP`/`use_dsp`, `ram_style`, `rom_style`, `DONT_TOUCH`/`dont_touch`, `KEEP`/`keep =`, `max_fanout`. (They are removed, not replaced.)
- Multipliers are written as `a * b` on `signed` regs with explicit pipeline registers before and after the product.
- RAM/ROM: `reg [W-1:0] mem [0:D-1]`, synchronous read, one write port per `always` block, no reset on array contents.
- Budget: ≤ 55 inferred multipliers, ≤ 1 Mbit inferred RAM for the pipeline.
- ADC: `DATA_W = 12`, single-edge capture, `clk_proc = adc_clk` (nominal 100 MHz). No Gray-coded multi-bit CDC anywhere.
- DDC: NCO `PHASE_INC` default `32'h3333_3333` (20 MHz @ 100 MSPS), 32-bit accumulator, quarter-wave LUT, 16-bit output; CIC 5 stages, R = 4, M = 1, Hogenauer widths (`B_max = N·log2(R·M) + B_in`); FIR 32 taps symmetric, folded, accumulator = product width + log2(taps) guard bits, coefficients unchanged.
- Range FFT `N_FFT` parameter default 256; `fft_engine` `INTERNAL_W` default 24; twiddles 16 bits; ≤ 4 multipliers per engine.
- Reference spectrum precomputed into a `.mem` by `tb/golden/gen_ref_spectrum.py`; `latency_buffer.v` deleted.
- `run_regression.sh` passes after every task (update/delete tests in the same task as the module change). No silent skips.
- Host USB packet format (`usb_data_interface_ft2232h.v`), CFAR/MTI/Doppler algorithms, and the STM32 DIG0–7 GPIO contract are unchanged.
- Each commit message ends with `Co-Authored-By: Claude Fable 5.1 <noreply@anthropic.com>`.

## Decisions taken while planning (read before starting)

1. **No second ×4 decimator is instantiated.** Spec R3 asks for `decimator_4x.v` "after the CIC so the FIR runs at 25 MSPS". With a 100 MSPS ADC the CIC (R = 4) already outputs 25 MSPS = one sample every 4 `clk_proc` cycles, which is exactly the rate the spec wants the FIR to run at. A further ×4 would give 6.25 MSPS, below the 20 MHz chirp bandwidth. The spec's stated goal (FIR and everything downstream at 25 MSPS, one sample per 4 cycles) is met by the CIC alone. This deviation is reported to the spec owner.
2. **FIR is folded *and* time-multiplexed over 4 phases**: 16 pre-added pairs / 4 phases = 4 physical multipliers per channel (8 total). The spec allows either a 16-multiplier folded or a time-multiplexed form; the 4-phase form is chosen because it keeps the total ≤ 55 even when a 24×16 FFT multiply is counted as two 18×18 blocks.
3. **The behavioral (`ifdef SIMULATION`) branch of `matched_filter_processing_chain.v` is kept** (re-sized to 256 points and the ROM reference). The integration/system testbenches rely on it for simulation speed. The synthesizable branch is exercised by `tb/tb_mf_chain.v` compiled **without** `-DSIMULATION` (new `run_test_nosim` helper) with a bit-exact golden.
4. **Long chirp segmentation**: 750 samples at 25 MSPS, `N_FFT = 256`, `OVERLAP = 32`, `ADVANCE = 224` → samples covered after *k* segments = 256 + 224·(k−1): k = 3 covers 704 < 750, k = 4 covers 928 ≥ 750 → **4 segments** (the last one zero-padded). Reference spectrum segment *s* is the FFT of chirp samples `[224·s, 224·s + 256)` so a target at delay *d* peaks at bin *d* in every segment.
5. **Reference ROM** holds 5 spectra (4 long segments + 1 short) = 1280 × 32 bit = 40 kbit. No time-domain reference ROM remains, so the "short-chirp ROM ≤ 64 entries" requirement is satisfied vacuously (the 13-sample short chirp exists only inside the generator script).
6. **`cdc_adc_to_processing` is deleted entirely** (Task 6, together with `ddc_400m.v`, its last user). The 6-bit chirp counter crossing 120 MHz → 100 MHz in `radar_system_top.v` is moved to the existing, formally verified `cdc_handshake` in Task 2.
7. **Golden tests (a), (b) are bit-exact** (tolerance 0): the Python models reproduce the integer arithmetic of the RTL exactly (NCO phase for sample *n* is `PHASE_INC·n` by construction, with `DITHER_EN = 0`). (c) uses numpy float FFT with a documented tolerance; (d) checks peak *displacement* between two delays so it is independent of pipeline latency.
8. `radar_system_top_50t.v` (Xilinx production wrapper with LVDS ADC ports) is deleted; `scripts/50t`, `scripts/200t`, `scripts/te07*`, `constraints/` are left in place but documented as stale/board-specific in the README.

## Files

Pipeline file list after the port (this is `PROD_RTL` in `run_regression.sh`):

```
radar_system_top.v  radar_transmitter.v  dac_interface_single.v  plfm_chirp_controller.v
radar_receiver_final.v  adc_cmos_interface.v  ddc.v  nco.v  cic_decimator_4x_enhanced.v
fir_lowpass.v  cdc_modules.v  matched_filter_multi_segment.v  matched_filter_processing_chain.v
frequency_matched_filter.v  ref_spectrum_rom.v  range_bin_decimator.v  doppler_processor.v
xfft_16.v  fft_engine.v  usb_data_interface.v  usb_data_interface_ft2232h.v  edge_detector.v
radar_mode_controller.v  rx_gain_control.v  cfar_ca.v  mti_canceller.v  fpga_self_test.v
```

| Action | File | Responsibility |
|---|---|---|
| Create | `adc_cmos_interface.v` | 12-bit single-edge ADC capture + sticky overrange |
| Create | `nco.v` (replaces `nco_400m_enhanced.v`) | 20 MHz NCO, plain adder accumulator, internal dither |
| Modify | `cic_decimator_4x_enhanced.v` | Hogenauer-width 5-stage CIC, R = 4, no monitors |
| Create | `ddc.v` (replaces `ddc_400m.v`) | ADC-delay + NCO + 12×16 mixer + 2×CIC + 2×FIR |
| Rewrite | `fir_lowpass.v` | Folded, 4-phase time-multiplexed 32-tap FIR |
| Modify | `fft_engine.v` | `INTERNAL_W = 24`, N default 256, inferred TDP RAM |
| Create | `fft_twiddle_256.mem` | 64-entry quarter-wave cos ROM |
| Create | `ref_spectrum_rom.v`, `ref_spectrum_i.mem`, `ref_spectrum_q.mem` | Precomputed reference spectra |
| Rewrite | `matched_filter_processing_chain.v` | FFT → conj-multiply with ROM → IFFT (both branches) |
| Rewrite | `matched_filter_multi_segment.v` | 256-pt overlap-save segmenter, no memory handshake |
| Modify | `radar_receiver_final.v` | New ADC/DDC wiring, no latency buffer / chirp loader |
| Modify | `radar_system_top.v` | ADC ports, no clock buffers, handshake CDC |
| Modify | `mti_canceller.v` | RAM-inferable history |
| Modify | `cdc_modules.v`, `usb_data_interface*.v`, `edge_detector.v`, `dac_interface_single.v`, `cfar_ca.v`, `doppler_processor.v`, `plfm_chirp_controller.v`, `frequency_matched_filter.v`, `xfft_16.v` | Attribute/primitive scrub |
| Delete | `ad9484_interface_400m.v`, `adc_clk_mmcm.v`, `adc_clk_mmcm_integration.md`, `latency_buffer.v`, `chirp_memory_loader_param.v`, `ddc_input_interface.v`, `radar_system_top_50t.v`, `long_chirp_seg?_?.mem`, `short_chirp_?.mem`, `tb/ad9484_interface_400m_stub.v`, `tb/tb_ad9484_xsim.v`, `tb/tb_nco_xsim.v`, `tb/tb_nco_400m.v`, `tb/tb_ddc_400m.v`, `tb/tb_ddc_cosim.v`, `tb/tb_latency_buffer.v`, `tb/tb_ddc_input_interface.v`, `tb/tb_matched_filter_processing_chain.v`, `tb/tb_mf_chain_synth.v`, `tb/tb_mf_cosim.v`, `tb/tb_multiseg_cosim.v`, `formal/fv_cdc_adc.v`, `formal/fv_cdc_adc.sby` | Obsolete |
| Create | `tb/golden/radar_params.py`, `gen_ddc_golden.py`, `gen_fir_golden.py`, `gen_twiddle_rom.py`, `gen_fft_golden.py`, `gen_ref_spectrum.py`, `gen_mf_chain_golden.py`, `gen_fullchain_golden.py`, `count_multipliers.py` | Golden generators and the resource gate |
| Create | `tb/tb_adc_cmos_interface.v`, `tb/tb_nco.v`, `tb/tb_ref_spectrum_rom.v`, `tb/tb_mf_chain.v`, `tb/golden/tb_ddc_golden.v`, `tb/golden/tb_fir_golden.v`, `tb/golden/tb_fft256_golden.v`, `tb/golden/tb_fullchain_golden.v` | New testbenches |
| Modify | `tb/tb_cic_decimator.v`, `tb/tb_fir_lowpass.v`, `tb/tb_fft_engine.v`, `tb/tb_cdc_modules.v`, `tb/tb_radar_receiver_final.v`, `tb/radar_system_tb.v`, `tb/tb_system_e2e.v`, `tb/tb_mti_canceller.v`, `tb/cosim/fpga_model.py` | Adapted testbenches / model |
| Modify | `run_regression.sh`, `.github/workflows/ci-tests.yml`, create `README.md` | Gates, CI, documentation |

All paths below are relative to `9_Firmware/9_2_FPGA/` unless they start with `docs/` or `.github/`. Run every command from `9_Firmware/9_2_FPGA/`.

---

### Task 1: Pin the baseline

**Files:** none modified.

- [ ] **Step 1: Make sure Icarus Verilog is installed**

Run: `which iverilog || brew install icarus-verilog` (macOS) / `sudo apt-get install -y iverilog` (Linux).
Expected: `iverilog -V | head -1` prints a version (12.x or 13.x).

- [ ] **Step 2: Run the full regression and record the result**

Run: `./run_regression.sh 2>&1 | tee /tmp/baseline_regression.log; tail -5 /tmp/baseline_regression.log`
Expected: `Tests: 27 passed, 0 failed, 0 skipped / 27 total` (the CI comment says 25; the script has 27 `run_test` calls). If the count differs, write the actual numbers into `/tmp/baseline_regression.log` — this is the reference every later task must match or exceed.

- [ ] **Step 3: Record the baseline primitive inventory (for the README removed-modules list)**

Run: `grep -lE "DSP48E1|xpm_memory|IBUFDS|IDDR|ODDR|MMCME2|PLLE2|BUFG|BUFIO" *.v | tee /tmp/baseline_primitives.txt`
Expected: a non-empty list (at least `ad9484_interface_400m.v adc_clk_mmcm.v cic_decimator_4x_enhanced.v dac_interface_single.v ddc_400m.v fft_engine.v nco_400m_enhanced.v radar_system_top.v usb_data_interface.v fir_lowpass.v`).

Do **not** commit anything in this task.

---

### Task 2: Remove Xilinx primitives/attributes from the non-datapath modules; move the chirp-counter CDC to the handshake

**Files:**
- Modify: `cdc_modules.v:1-8, 53-54, 155, 199, 205` (attributes and comments only — `cdc_adc_to_processing` itself is still instantiated by `ddc_400m.v` and is deleted in Task 6)
- Modify: `radar_system_top.v:294-318, 321, 334, 347, 375-397`
- Modify: `usb_data_interface.v:54, 166-168, 229, 260, 305-306, 314, 646-680`
- Modify: `usb_data_interface_ft2232h.v:73, 226, 257, 272-275, 289-290`
- Modify: `edge_detector.v:8-9`
- Modify: `dac_interface_single.v:11-77`
- Modify: `cfar_ca.v:52-58, 147`
- Modify: `doppler_processor.v:112-113`
- Modify: `plfm_chirp_controller.v:71`
- Modify: `frequency_matched_filter.v:44, 62, 81, 118`
**Interfaces:**
- Consumes: `cdc_handshake #(WIDTH)` ports `src_clk, dst_clk, reset_n, src_data, src_valid, src_ready, dst_data, dst_valid, dst_ready` (unchanged, `cdc_modules.v:173-193`).
- Produces: `radar_system_top.v` no longer references `cdc_adc_to_processing`; `tx_current_chirp_sync[5:0]` is now produced by `cdc_handshake`.

- [ ] **Step 1: Strip the attributes in `cdc_modules.v`**

```bash
perl -pi -e 's/\(\* ASYNC_REG = "TRUE" \*\) //g' cdc_modules.v
```

Replace the header comment of `cdc_adc_to_processing` (lines 3–8) with:

```verilog
// ============================================================================
// CDC FOR MULTI-BIT DATA — Gray-coded, ONLY valid for values that change by
// +-1 per source cycle.  Last user: ddc_400m.v (removed in the vendor-neutral
// port; this module is deleted together with it).
// ============================================================================
```

Replace the header comment of `cdc_single_bit` (lines 140-144) with:

```verilog
// ============================================================================
// CDC FOR SINGLE BIT SIGNALS
// Plain multi-stage synchronizer with synchronous reset.  Multi-bit data must
// use cdc_handshake below — never a per-bit synchronizer.
// ============================================================================
```

- [ ] **Step 2: Replace the chirp-counter CDC in `radar_system_top.v`**

Replace lines 375–397 (the comment block and the `cdc_adc_to_processing #(.WIDTH(6), .STAGES(3)) cdc_chirp_counter (...)` instance) with:

```verilog
// ============================================================================
// CLOCK DOMAIN CROSSING: TRANSMITTER (120 MHz) -> SYSTEM (100 MHz)
// ============================================================================

// CDC for chirp_counter: 6-bit value, four-phase handshake (formally verified
// in formal/fv_cdc_handshake.sby).  src_valid is held high so the counter is
// re-sampled continuously; dst_data holds the last transferred value.
cdc_handshake #(
    .WIDTH(6)
) cdc_chirp_counter (
    .src_clk(clk_120m_dac_buf),
    .dst_clk(clk_100m_buf),
    .reset_n(sys_reset_n),
    .src_data(tx_current_chirp),
    .src_valid(1'b1),
    .src_ready(),
    .dst_data(tx_current_chirp_sync),
    .dst_valid(tx_current_chirp_sync_valid),
    .dst_ready(1'b1)
);
```

- [ ] **Step 3: Remove the clock buffers and attributes in `radar_system_top.v`**

Replace lines 294–318 (`// CLOCK BUFFERING` section including the `ifdef SIMULATION` / `BUFG` branches) with:

```verilog
// ============================================================================
// CLOCKS
// ============================================================================
// Global clock buffering is inserted automatically by the synthesis tool
// (Quartus and Vivado both promote clock nets).  Nothing vendor-specific here.
assign clk_100m_buf     = clk_100m;
assign clk_120m_dac_buf = clk_120m_dac;
assign ft601_clk_buf    = ft601_clk_in;
```

Then run:

```bash
perl -pi -e 's/\(\* ASYNC_REG = "TRUE" \*\) //g' radar_system_top.v usb_data_interface.v usb_data_interface_ft2232h.v edge_detector.v
```

- [ ] **Step 4: Remove the output clock forwarders**

In `usb_data_interface.v` replace lines 643–680 (from `// FT601 clock output forwarding` through the matching `` `endif ``) with:

```verilog
// ============================================================================
// FT601 clock output forwarding
// ============================================================================
// The forwarded clock is a plain signal here; a board wrapper that needs
// IOB-aligned clock forwarding adds the vendor clock-output primitive there.
assign ft601_clk_out = ft601_clk_in;
```

Also edit line 54 comment to `output wire ft601_clk_out,        // Output clock to FT601 (plain forward)` and line 73 of `usb_data_interface_ft2232h.v` to `// Clock from FT2232H (used directly)`.

In `dac_interface_single.v` replace lines 11–77 (the register block comment through the `` `endif ``) with:

```verilog
// ============================================================================
// DAC data register
// ============================================================================
reg [7:0] dac_data_reg;

always @(posedge clk_120m or negedge reset_n) begin
    if (!reset_n) begin
        dac_data_reg <= 8'd128;  // Center value
    end else if (chirp_valid) begin
        dac_data_reg <= chirp_data;
    end else begin
        dac_data_reg <= 8'd128;  // Default to center when no chirp
    end
end

// Clock forwarding to the DAC pin is board-specific (PLL output pin on Intel,
// clock-output primitive on Xilinx).  The core exports the clock as a plain
// signal; the board wrapper owns the output primitive.
assign dac_clk  = clk_120m;
assign dac_data = dac_data_reg;
```

- [ ] **Step 5: Strip memory attributes and fix the CFAR header**

```bash
perl -pi -e 's/\(\* ram_style = "block" \*\) //g' cfar_ca.v doppler_processor.v plfm_chirp_controller.v matched_filter_multi_segment.v matched_filter_processing_chain.v chirp_memory_loader_param.v latency_buffer.v
```

In `cfar_ca.v` replace lines 52–58 (the `Resources:` block of the header) with:

```verilog
 * Resources (measured, Vivado Build 25 on XC7A200T — see docs/reports.html):
 *   - 1 block RAM for the magnitude buffer (2048 x 17 bits = 34.8 kbit)
 *   - 1 multiplier for alpha * noise_sum (8 x 21 bits); the GO/SO modes add
 *     two small cross-multiplies (21 x 5 bits) that may map to logic
 *   - ~2 200 LUTs for FSM + sliding window + comparators (the earlier
 *     "~300 LUTs" estimate was wrong by 7x)
```

In `frequency_matched_filter.v` change the four comments `// Sync reset: enables DSP48E1 absorption (fixes DPOR-1/DPIP-1 DRC)` (lines 44, 62, 81, 118) to `// Sync reset: lets the multiplier pipeline registers be absorbed by either vendor`.

- [ ] **Step 6: Nothing to delete yet**

`cdc_adc_to_processing`, its testbench section and `formal/fv_cdc_adc.*` stay until Task 6 removes `ddc_400m.v`, their last user. After this task the only Gray-code CDC instances left are the two inside `ddc_400m.v`.

- [ ] **Step 7: Verify the grep gate on the touched files and run the regression**

Run: `grep -nE "DSP48E1|xpm_memory|IBUFDS|IDDR|ODDR|MMCME2|PLLE2|BUFG|BUFIO|ASYNC_REG|ram_style|DONT_TOUCH" cdc_modules.v radar_system_top.v usb_data_interface.v usb_data_interface_ft2232h.v edge_detector.v dac_interface_single.v cfar_ca.v doppler_processor.v plfm_chirp_controller.v frequency_matched_filter.v`
Expected: no output. (If a comment still mentions one of the words, reword it.)

Run: `./run_regression.sh 2>&1 | tail -5`
Expected: `Tests: 27 passed, 0 failed` (E2E/system tests still pass because `tx_current_chirp_sync` is still a level).

- [ ] **Step 8: Commit**

```bash
git add -A 9_Firmware/9_2_FPGA
git commit -m "fpga: remove Xilinx primitives/attributes from control, USB, DAC, CFAR paths; replace Gray CDC with handshake

Co-Authored-By: Claude Fable 5.1 <noreply@anthropic.com>"
```

---

### Task 3: `adc_cmos_interface.v` — 12-bit single-edge ADC capture

**Files:**
- Create: `adc_cmos_interface.v`
- Create: `tb/tb_adc_cmos_interface.v`
- Modify: `run_regression.sh` (Phase 4, add test)

**Interfaces:**
- Produces: `module adc_cmos_interface #(parameter DATA_W = 12) (input adc_clk, input reset_n, input [DATA_W-1:0] adc_data, input adc_ovr, input overrange_clear, output reg [DATA_W-1:0] sample, output reg sample_valid, output reg overrange)`. Latency: `sample` = `adc_data` captured two `adc_clk` edges earlier. `sample_valid` is 1 from the second edge after reset release. `overrange` is sticky; `overrange_clear` clears it unless `adc_ovr` is asserted in the same cycle. Task 6 instantiates it inside `radar_receiver_final.v`.

- [ ] **Step 1: Write the failing testbench**

Create `tb/tb_adc_cmos_interface.v`:

```verilog
`timescale 1ns / 1ps
// tb_adc_cmos_interface.v — unit test for the 12-bit CMOS ADC capture
module tb_adc_cmos_interface;
    localparam CLK_PERIOD = 10.0;   // 100 MHz ADC DCO
    reg         clk;
    reg         reset_n;
    reg  [11:0] adc_data;
    reg         adc_ovr;
    reg         overrange_clear;
    wire [11:0] sample;
    wire        sample_valid;
    wire        overrange;

    integer pass_count, fail_count, test_num, i;

    always #(CLK_PERIOD/2) clk = ~clk;

    adc_cmos_interface #(.DATA_W(12)) uut (
        .adc_clk(clk), .reset_n(reset_n),
        .adc_data(adc_data), .adc_ovr(adc_ovr), .overrange_clear(overrange_clear),
        .sample(sample), .sample_valid(sample_valid), .overrange(overrange)
    );

    task check;
        input cond;
        input [511:0] label;
        begin
            test_num = test_num + 1;
            if (cond) begin $display("[PASS] Test %0d: %0s", test_num, label); pass_count = pass_count + 1; end
            else       begin $display("[FAIL] Test %0d: %0s", test_num, label); fail_count = fail_count + 1; end
        end
    endtask

    initial begin
        clk = 0; reset_n = 0; adc_data = 12'h800; adc_ovr = 0; overrange_clear = 0;
        pass_count = 0; fail_count = 0; test_num = 0;

        // --- Group 1: reset ---
        repeat (3) @(posedge clk); #1;
        check(sample_valid === 1'b0, "sample_valid = 0 in reset");
        check(overrange === 1'b0,    "overrange = 0 in reset");
        reset_n = 1;

        // --- Group 2: ramp with 2-cycle latency ---
        for (i = 0; i < 20; i = i + 1) begin
            adc_data = 12'd100 + i;
            @(posedge clk); #1;
        end
        // After the loop: adc_data was 119 at the last edge; sample shows 118 (presented 2 edges before)
        check(sample === 12'd118, "sample lags adc_data by exactly 2 edges");
        check(sample_valid === 1'b1, "sample_valid = 1 after reset release");

        // --- Group 3: overrange sticky + clear ---
        adc_ovr = 1; @(posedge clk); #1; adc_ovr = 0;
        @(posedge clk); #1;
        check(overrange === 1'b1, "overrange sets 2 edges after adc_ovr pulse");
        repeat (5) @(posedge clk); #1;
        check(overrange === 1'b1, "overrange is sticky");
        overrange_clear = 1; @(posedge clk); #1; overrange_clear = 0; @(posedge clk); #1;
        check(overrange === 1'b0, "overrange_clear clears the flag");

        // --- Group 4: clear and set in the same cycle -> set wins ---
        adc_ovr = 1; @(posedge clk); #1; adc_ovr = 0;      // captured into adc_ovr_q1
        overrange_clear = 1; @(posedge clk); #1; overrange_clear = 0;  // same edge as the set
        check(overrange === 1'b1, "set has priority over clear");

        // --- Group 5: reset clears everything ---
        reset_n = 0; @(posedge clk); #1;
        check(overrange === 1'b0 && sample_valid === 1'b0, "async reset clears flags");

        $display("\nResults: %0d/%0d passed", pass_count, pass_count + fail_count);
        $finish;
    end
endmodule
```

- [ ] **Step 2: Run it to see it fail**

Run: `iverilog -g2001 -DSIMULATION -o /tmp/tb_adc.vvp tb/tb_adc_cmos_interface.v adc_cmos_interface.v`
Expected: compile error `Unable to open input file "adc_cmos_interface.v"`.

- [ ] **Step 3: Write the module**

Create `adc_cmos_interface.v`:

```verilog
`timescale 1ns / 1ps
// ============================================================================
// adc_cmos_interface.v — single-edge CMOS parallel ADC capture
//
// Replaces ad9484_interface_400m.v (LVDS, DDR, 400 MHz) for the 12-bit
// 100 MSPS CMOS ADC.  adc_clk is the ADC data-clock output and is ALSO the
// processing clock of the whole receiver (clk_proc = adc_clk), so no clock
// domain crossing exists between the ADC and the DDC.
//
//   adc_data[DATA_W-1:0]  offset-binary samples, change on adc_clk edges
//   adc_ovr               over-range pin (active high)
//   sample / sample_valid registered sample stream, 2-edge latency
//   overrange             sticky flag, cleared by overrange_clear (set wins)
//
// Resources: 2*DATA_W + 4 flip-flops, no multipliers, no RAM.
// Board-specific: input delay constraints for the data pins; any I/O
// register packing is left to the board wrapper/tool.
// ============================================================================
module adc_cmos_interface #(
    parameter DATA_W = 12
)(
    input  wire              adc_clk,          // ADC DCO, <= 125 MHz
    input  wire              reset_n,
    input  wire [DATA_W-1:0] adc_data,
    input  wire              adc_ovr,
    input  wire              overrange_clear,
    output reg  [DATA_W-1:0] sample,
    output reg               sample_valid,
    output reg               overrange
);

// First capture stage: no reset so the tool may place it in the I/O cell.
reg [DATA_W-1:0] adc_data_q1;
reg              adc_ovr_q1;

always @(posedge adc_clk) begin
    adc_data_q1 <= adc_data;
    adc_ovr_q1  <= adc_ovr;
end

always @(posedge adc_clk or negedge reset_n) begin
    if (!reset_n) begin
        sample       <= {DATA_W{1'b0}};
        sample_valid <= 1'b0;
        overrange    <= 1'b0;
    end else begin
        sample       <= adc_data_q1;
        sample_valid <= 1'b1;
        if (adc_ovr_q1)
            overrange <= 1'b1;
        else if (overrange_clear)
            overrange <= 1'b0;
    end
end

endmodule
```

- [ ] **Step 4: Run the test**

Run: `iverilog -g2001 -DSIMULATION -o /tmp/tb_adc.vvp tb/tb_adc_cmos_interface.v adc_cmos_interface.v && vvp /tmp/tb_adc.vvp | grep -c "^\[PASS"`
Expected: `8` and no `[FAIL` lines. (In Group 2 the loop presents 100..119; the value `119` is on the bus at the last edge, `118` was presented one edge earlier and is in `adc_data_q1`, so `sample` = `118`. If you get `117`, the latency is 3 — fix the module, not the test.)

- [ ] **Step 5: Add the test to the regression (Phase 4)**

In `run_regression.sh` after the `"Radar Mode Controller"` test (line 513-515) add:

```bash
run_test "ADC CMOS Interface" \
    tb/tb_adc_reg.vvp \
    tb/tb_adc_cmos_interface.v adc_cmos_interface.v
```

Run: `./run_regression.sh 2>&1 | tail -4` → `Tests: 28 passed, 0 failed`.

- [ ] **Step 6: Commit**

```bash
git add adc_cmos_interface.v tb/tb_adc_cmos_interface.v run_regression.sh
git commit -m "fpga: add adc_cmos_interface (12-bit single-edge CMOS ADC capture)

Co-Authored-By: Claude Fable 5.1 <noreply@anthropic.com>"
```

---

### Task 4: `nco.v` — 20 MHz NCO with an inferred accumulator

**Files:**
- Create: `nco.v` (replaces `nco_400m_enhanced.v`)
- Create: `tb/tb_nco.v` (replaces `tb/tb_nco_400m.v`)
- Delete: `nco_400m_enhanced.v`, `tb/tb_nco_400m.v`, `tb/tb_nco_xsim.v` — **only after Task 6** removes the last user (`ddc_400m.v`). In this task create the new files side by side and switch the regression entry.
- Modify: `run_regression.sh:477-479`

**Interfaces:**
- Produces: `module nco #(parameter PHASE_INC = 32'h3333_3333, parameter DITHER_EN = 1) (input clk, input reset_n, input phase_valid, input [15:0] phase_offset, output reg signed [15:0] sin_out, output reg signed [15:0] cos_out, output reg dds_ready)`. Timing contract used by Task 6: with `phase_valid` asserted on consecutive edges e, e+1, … (starting from reset with the accumulator at 0), the `sin_out/cos_out` pair for phase `PHASE_INC·n` (n-th valid edge) is readable at edge e+n+4. Module `lfsr_dither #(WIDTH)` lives in the same file.

- [ ] **Step 1: Write the failing testbench**

Create `tb/tb_nco.v`:

```verilog
`timescale 1ns / 1ps
// tb_nco.v — frequency, quadrature and phase-offset checks for nco.v
module tb_nco;
    localparam CLK_PERIOD = 10.0;                 // 100 MHz
    localparam [31:0] FTW_20MHZ = 32'h3333_3333;  // 0.2 * 2^32
    localparam [31:0] FTW_1MHZ  = 32'h028F_5C29;  // 0.01 * 2^32

    reg clk, reset_n, phase_valid;
    wire signed [15:0] sin20, cos20, sin1, cos1, sin20o, cos20o;
    wire ready20, ready1, ready20o;
    integer pass_count, fail_count, test_num, i;
    integer zc20, zc1, mismatch;
    reg signed [15:0] prev20, prev1;
    integer mag_sq, mag_min, mag_max;

    always #(CLK_PERIOD/2) clk = ~clk;

    nco #(.PHASE_INC(FTW_20MHZ), .DITHER_EN(0)) u20 (
        .clk(clk), .reset_n(reset_n), .phase_valid(phase_valid), .phase_offset(16'h0000),
        .sin_out(sin20), .cos_out(cos20), .dds_ready(ready20));
    nco #(.PHASE_INC(FTW_1MHZ), .DITHER_EN(0)) u1 (
        .clk(clk), .reset_n(reset_n), .phase_valid(phase_valid), .phase_offset(16'h0000),
        .sin_out(sin1), .cos_out(cos1), .dds_ready(ready1));
    nco #(.PHASE_INC(FTW_20MHZ), .DITHER_EN(0)) u20o (          // +90 degrees
        .clk(clk), .reset_n(reset_n), .phase_valid(phase_valid), .phase_offset(16'h4000),
        .sin_out(sin20o), .cos_out(cos20o), .dds_ready(ready20o));

    task check;
        input cond; input [511:0] label;
        begin
            test_num = test_num + 1;
            if (cond) begin $display("[PASS] Test %0d: %0s", test_num, label); pass_count = pass_count + 1; end
            else       begin $display("[FAIL] Test %0d: %0s", test_num, label); fail_count = fail_count + 1; end
        end
    endtask

    initial begin
        clk = 0; reset_n = 0; phase_valid = 0;
        pass_count = 0; fail_count = 0; test_num = 0;
        zc20 = 0; zc1 = 0; mismatch = 0; mag_min = 32'h7FFFFFFF; mag_max = 0;
        repeat (3) @(posedge clk); #1;
        check(ready20 === 1'b0, "dds_ready = 0 in reset");
        check(cos20 === 16'sh7FFF && sin20 === 16'sh0000, "reset phase 0: cos=0x7FFF sin=0");
        reset_n = 1; phase_valid = 1;
        repeat (8) @(posedge clk); #1;
        check(ready20 === 1'b1, "dds_ready asserts after pipeline fill");

        // 1000 cycles: 20 MHz -> 200 periods, 1 MHz -> 10 periods
        prev20 = sin20; prev1 = sin1;
        for (i = 0; i < 1000; i = i + 1) begin
            @(posedge clk); #1;
            if (prev20 < 0 && sin20 >= 0) zc20 = zc20 + 1;
            if (prev1  < 0 && sin1  >= 0) zc1  = zc1  + 1;
            prev20 = sin20; prev1 = sin1;
            mag_sq = sin20 * sin20 + cos20 * cos20;
            if (mag_sq < mag_min) mag_min = mag_sq;
            if (mag_sq > mag_max) mag_max = mag_sq;
            if (sin20o !== cos20) mismatch = mismatch + 1;
        end
        $display("zero crossings: 20MHz=%0d (exp 200), 1MHz=%0d (exp 10)", zc20, zc1);
        $display("sin^2+cos^2 range: %0d .. %0d (32767^2 = %0d)", mag_min, mag_max, 32767*32767);
        check(zc20 >= 199 && zc20 <= 201, "20 MHz: 200 +/-1 rising zero crossings in 1000 cycles");
        check(zc1 >= 9 && zc1 <= 11,      "1 MHz: 10 +/-1 rising zero crossings in 1000 cycles");
        check(mag_min > 32767*32767*96/100 && mag_max < 32767*32767*104/100,
              "quadrature: sin^2+cos^2 within 4% of full scale");
        check(mismatch == 0, "phase_offset 0x4000 (+90deg): sin == cos of unshifted NCO, exactly");

        // phase_valid gating freezes the output
        phase_valid = 0; repeat (6) @(posedge clk); #1;
        prev20 = sin20; repeat (10) @(posedge clk); #1;
        check(sin20 === prev20, "phase_valid=0 freezes the output");

        $display("\nResults: %0d/%0d passed", pass_count, pass_count + fail_count);
        $finish;
    end
endmodule
```

- [ ] **Step 2: Run it to see it fail**

Run: `iverilog -g2001 -DSIMULATION -o /tmp/tb_nco.vvp tb/tb_nco.v nco.v`
Expected: `Unable to open input file "nco.v"`.

- [ ] **Step 3: Write `nco.v`**

```verilog
`timescale 1ns / 1ps
// ============================================================================
// nco.v — numerically controlled oscillator for the DDC (vendor-neutral)
//
// f_out = PHASE_INC * f_clk / 2^32.  Default PHASE_INC = 0x3333_3333 gives
// 20 MHz at f_clk = 100 MHz (0x3333_3333 / 2^32 = 0.19999999995).
//
// Pipeline (every stage advances only while phase_valid = 1):
//   S1  phase_acc <= phase_acc + (PHASE_INC + dither)          32-bit '+'
//   S2  phase_off <= phase_acc(old) + {phase_offset, 16'b0}
//   S3  lut_idx / quadrant registered from phase_off[31:24]
//   S4  sin_abs / cos_abs <= quarter-wave LUT (64 x 16)
//   S5  sin_out / cos_out <= quadrant sign mux
// Timing contract: the accumulator is 0 at the first valid edge e0, so the
// n-th valid edge uses phase PHASE_INC*n (DITHER_EN = 0) and the matching
// sin/cos pair is readable at edge e0+n+4.  ddc.v delays the ADC sample by
// NCO_LAT = 4 registers to meet it.
//
// LUT[k] = round(32767 * sin(pi/2 * k / 64)), cos uses LUT[63-k].  Phase is
// truncated to 8 bits (2 quadrant + 6 index) before the LUT.
//
// Dither: an 8-bit LFSR (x^8+x^6+x^5+x^4+1) adds 0..255 to the tuning word
// each valid cycle when DITHER_EN = 1 to spread phase-truncation spurs.
// Golden tests instantiate DITHER_EN = 0 for bit-exact comparison.
//
// Resources: ~110 flip-flops, one 32-bit adder, 1 kbit LUT ROM, 0 multipliers.
// ============================================================================
module nco #(
    parameter PHASE_INC = 32'h3333_3333,
    parameter DITHER_EN = 1
)(
    input  wire               clk,
    input  wire               reset_n,
    input  wire               phase_valid,
    input  wire [15:0]        phase_offset,
    output reg  signed [15:0] sin_out,
    output reg  signed [15:0] cos_out,
    output reg                dds_ready
);

// Quarter-wave sine LUT (0..90 degrees)
reg [15:0] sin_lut [0:63];
initial begin
    sin_lut[ 0] = 16'h0000; sin_lut[ 1] = 16'h0324; sin_lut[ 2] = 16'h0648; sin_lut[ 3] = 16'h096A;
    sin_lut[ 4] = 16'h0C8C; sin_lut[ 5] = 16'h0FAB; sin_lut[ 6] = 16'h12C8; sin_lut[ 7] = 16'h15E2;
    sin_lut[ 8] = 16'h18F9; sin_lut[ 9] = 16'h1C0B; sin_lut[10] = 16'h1F1A; sin_lut[11] = 16'h2223;
    sin_lut[12] = 16'h2528; sin_lut[13] = 16'h2826; sin_lut[14] = 16'h2B1F; sin_lut[15] = 16'h2E11;
    sin_lut[16] = 16'h30FB; sin_lut[17] = 16'h33DF; sin_lut[18] = 16'h36BA; sin_lut[19] = 16'h398C;
    sin_lut[20] = 16'h3C56; sin_lut[21] = 16'h3F17; sin_lut[22] = 16'h41CE; sin_lut[23] = 16'h447A;
    sin_lut[24] = 16'h471C; sin_lut[25] = 16'h49B4; sin_lut[26] = 16'h4C3F; sin_lut[27] = 16'h4EBF;
    sin_lut[28] = 16'h5133; sin_lut[29] = 16'h539B; sin_lut[30] = 16'h55F5; sin_lut[31] = 16'h5842;
    sin_lut[32] = 16'h5A82; sin_lut[33] = 16'h5CB3; sin_lut[34] = 16'h5ED7; sin_lut[35] = 16'h60EB;
    sin_lut[36] = 16'h62F1; sin_lut[37] = 16'h64E8; sin_lut[38] = 16'h66CF; sin_lut[39] = 16'h68A6;
    sin_lut[40] = 16'h6A6D; sin_lut[41] = 16'h6C23; sin_lut[42] = 16'h6DC9; sin_lut[43] = 16'h6F5E;
    sin_lut[44] = 16'h70E2; sin_lut[45] = 16'h7254; sin_lut[46] = 16'h73B5; sin_lut[47] = 16'h7504;
    sin_lut[48] = 16'h7641; sin_lut[49] = 16'h776B; sin_lut[50] = 16'h7884; sin_lut[51] = 16'h7989;
    sin_lut[52] = 16'h7A7C; sin_lut[53] = 16'h7B5C; sin_lut[54] = 16'h7C29; sin_lut[55] = 16'h7CE3;
    sin_lut[56] = 16'h7D89; sin_lut[57] = 16'h7E1D; sin_lut[58] = 16'h7E9C; sin_lut[59] = 16'h7F09;
    sin_lut[60] = 16'h7F61; sin_lut[61] = 16'h7FA6; sin_lut[62] = 16'h7FD8; sin_lut[63] = 16'h7FF5;
end

// ---- Dither ----
wire [7:0]  dither_bits;
wire        dither_on = (DITHER_EN != 0);

lfsr_dither #(.WIDTH(8)) u_dither (
    .clk(clk),
    .reset_n(reset_n),
    .enable(phase_valid & dither_on),
    .dither_out(dither_bits)
);

wire [31:0] ftw = PHASE_INC + (dither_on ? {24'b0, dither_bits} : 32'b0);

// ---- S1/S2: accumulator and offset ----
reg [31:0] phase_acc;
reg [31:0] phase_off;

always @(posedge clk or negedge reset_n) begin
    if (!reset_n) begin
        phase_acc <= 32'h0000_0000;
        phase_off <= 32'h0000_0000;
    end else if (phase_valid) begin
        phase_acc <= phase_acc + ftw;
        phase_off <= phase_acc + {phase_offset, 16'b0};
    end
end

// ---- S3: quadrant / index decode ----
wire [7:0] lut_address = phase_off[31:24];
wire [1:0] quadrant_w  = lut_address[7:6];
wire [5:0] lut_idx_w   = (quadrant_w[0] ^ quadrant_w[1]) ? ~lut_address[5:0] : lut_address[5:0];

reg [5:0]  lut_idx_r;
reg [1:0]  quadrant_r;
reg [15:0] sin_abs_r, cos_abs_r;
reg [1:0]  quadrant_r2;
reg [4:0]  valid_pipe;

always @(posedge clk or negedge reset_n) begin
    if (!reset_n) begin
        lut_idx_r   <= 6'd0;
        quadrant_r  <= 2'b00;
        sin_abs_r   <= 16'h0000;
        cos_abs_r   <= 16'h7FFF;
        quadrant_r2 <= 2'b00;
        sin_out     <= 16'sh0000;
        cos_out     <= 16'sh7FFF;
    end else begin
        if (valid_pipe[0]) begin                      // S3
            lut_idx_r  <= lut_idx_w;
            quadrant_r <= quadrant_w;
        end
        if (valid_pipe[1]) begin                      // S4: LUT read
            sin_abs_r   <= sin_lut[lut_idx_r];
            cos_abs_r   <= sin_lut[6'd63 - lut_idx_r];
            quadrant_r2 <= quadrant_r;
        end
        if (valid_pipe[2]) begin                      // S5: sign mux
            case (quadrant_r2)
                2'b00: begin sin_out <=  $signed(sin_abs_r); cos_out <=  $signed(cos_abs_r); end
                2'b01: begin sin_out <=  $signed(sin_abs_r); cos_out <= -$signed(cos_abs_r); end
                2'b10: begin sin_out <= -$signed(sin_abs_r); cos_out <= -$signed(cos_abs_r); end
                default: begin sin_out <= -$signed(sin_abs_r); cos_out <=  $signed(cos_abs_r); end
            endcase
        end
    end
end

always @(posedge clk or negedge reset_n) begin
    if (!reset_n) begin
        valid_pipe <= 5'b00000;
        dds_ready  <= 1'b0;
    end else begin
        valid_pipe <= {valid_pipe[3:0], phase_valid};
        dds_ready  <= valid_pipe[3];
    end
end

endmodule

// ============================================================================
// lfsr_dither — Fibonacci LFSR, polynomial x^8+x^6+x^5+x^4+1 for WIDTH = 8
// ============================================================================
`timescale 1ns / 1ps
module lfsr_dither #(
    parameter WIDTH = 8
)(
    input  wire             clk,
    input  wire             reset_n,
    input  wire             enable,
    output wire [WIDTH-1:0] dither_out
);

reg  [WIDTH-1:0] lfsr_reg;
wire             feedback;

generate
    if (WIDTH == 8) begin : g_poly8
        assign feedback = lfsr_reg[7] ^ lfsr_reg[5] ^ lfsr_reg[4] ^ lfsr_reg[3];
    end else begin : g_poly_generic
        assign feedback = lfsr_reg[WIDTH-1] ^ lfsr_reg[WIDTH-2];
    end
endgenerate

always @(posedge clk or negedge reset_n) begin
    if (!reset_n)
        lfsr_reg <= {WIDTH{1'b1}};
    else if (enable)
        lfsr_reg <= {lfsr_reg[WIDTH-2:0], feedback};
end

assign dither_out = lfsr_reg;

endmodule
```

- [ ] **Step 4: Run the test**

Run: `iverilog -g2001 -DSIMULATION -o /tmp/tb_nco.vvp tb/tb_nco.v nco.v && vvp /tmp/tb_nco.vvp | grep -E "^\[|zero|range"`
Expected: 8 `[PASS` lines, 0 `[FAIL`. The 0x4000 offset check is exact because adding 0x4000<<16 to the phase moves `lut_address` by exactly 64 (one quadrant).

- [ ] **Step 5: Switch the regression entry**

In `run_regression.sh` lines 477–479 replace the NCO test with:

```bash
run_test "NCO (20 MHz IF, inferred accumulator)" \
    tb/tb_nco_reg.vvp \
    tb/tb_nco.v nco.v
```

Delete `tb/tb_nco_400m.v` and `tb/tb_nco_xsim.v` now (`git rm`); keep `nco_400m_enhanced.v` until Task 6.

Run: `./run_regression.sh 2>&1 | tail -4` → `Tests: 28 passed, 0 failed`.

- [ ] **Step 6: Commit**

```bash
git add nco.v tb/tb_nco.v run_regression.sh
git rm -q tb/tb_nco_400m.v tb/tb_nco_xsim.v
git commit -m "fpga: add vendor-neutral nco.v (20 MHz default, inferred accumulator, internal dither)

Co-Authored-By: Claude Fable 5.1 <noreply@anthropic.com>"
```

---

### Task 5: CIC with Hogenauer register widths and no monitors

**Files:**
- Rewrite: `cic_decimator_4x_enhanced.v` (module name kept)
- Rewrite: `tb/tb_cic_decimator.v`
- Modify: `ddc_400m.v:566-582` (drop the removed ports until Task 6 replaces the file)

**Interfaces:**
- Produces: `module cic_decimator_4x_enhanced #(parameter DATA_W = 16, parameter STAGES = 5, parameter LOG2_R = 2) (input clk, input reset_n, input signed [DATA_W-1:0] data_in, input data_valid, output reg signed [DATA_W-1:0] data_out, output reg data_out_valid)`. Arithmetic contract (used by `gen_ddc_golden.py` in Task 6): with x[n] the n-th valid input, S5 = five cumulative sums of x (mod 2^26), the j-th output is `Δ⁵ v[j] >> 10` with `v[j] = S5[4j−2]` (S5 of a negative index is 0) and Δ the first-order difference with zero history. One output per 4 valid inputs; output j appears 6 edges after the edge of input 4j+3.

- [ ] **Step 1: Write the failing testbench**

Replace `tb/tb_cic_decimator.v` with:

```verilog
`timescale 1ns / 1ps
// tb_cic_decimator.v — 5-stage CIC, R=4, Hogenauer widths (DATA_W=16)
module tb_cic_decimator;
    localparam CLK_PERIOD = 10.0;  // 100 MHz
    reg clk, reset_n;
    reg  signed [15:0] data_in;
    reg         data_valid;
    wire signed [15:0] data_out;
    wire        data_out_valid;
    integer pass_count, fail_count, test_num, i, out_count;
    reg signed [15:0] last_out;
    integer impulse_sum;
    integer imp_out [0:7];

    always #(CLK_PERIOD/2) clk = ~clk;

    cic_decimator_4x_enhanced #(.DATA_W(16), .STAGES(5), .LOG2_R(2)) uut (
        .clk(clk), .reset_n(reset_n), .data_in(data_in), .data_valid(data_valid),
        .data_out(data_out), .data_out_valid(data_out_valid));

    task check;
        input cond; input [511:0] label;
        begin
            test_num = test_num + 1;
            if (cond) begin $display("[PASS] Test %0d: %0s", test_num, label); pass_count = pass_count + 1; end
            else       begin $display("[FAIL] Test %0d: %0s", test_num, label); fail_count = fail_count + 1; end
        end
    endtask

    always @(posedge clk) if (data_out_valid) begin
        if (out_count < 8) imp_out[out_count] = data_out;
        out_count = out_count + 1; last_out = data_out; impulse_sum = impulse_sum + data_out;
    end

    task reset_dut;
        begin
            reset_n = 0; data_valid = 0; data_in = 0; out_count = 0; last_out = 0; impulse_sum = 0;
            for (i = 0; i < 8; i = i + 1) imp_out[i] = 0;
            repeat (3) @(posedge clk); #1; reset_n = 1; @(posedge clk); #1;
        end
    endtask

    initial begin
        clk = 0; pass_count = 0; fail_count = 0; test_num = 0;
        reset_dut;
        check(data_out_valid === 1'b0, "no output in reset");

        // --- DC: 400 samples of +12345 -> steady-state output == input exactly (gain 4^5 / 2^10 = 1) ---
        data_valid = 1;
        for (i = 0; i < 400; i = i + 1) begin data_in = 16'sd12345; @(posedge clk); #1; end
        data_valid = 0; repeat (10) @(posedge clk); #1;
        check(out_count == 100, "R=4: 400 inputs -> 100 outputs");
        check(last_out === 16'sd12345, "DC gain exactly 1 after >>10 (steady state)");

        // --- Negative full scale: no wrap ---
        reset_dut; data_valid = 1;
        for (i = 0; i < 400; i = i + 1) begin data_in = -16'sd32768; @(posedge clk); #1; end
        data_valid = 0; repeat (10) @(posedge clk); #1;
        check(last_out === -16'sd32768, "DC -32768 passes without wrap (26-bit integrators)");

        // --- Impulse 10000 at n=0: decimated impulse response, exact values from the
        //     integer model (Delta^5 S5[4j-2] >> 10): 0, 146, 1318, 986, 48, 0 ...
        reset_dut; data_valid = 1;
        data_in = 16'sd10000; @(posedge clk); #1; data_in = 0;
        for (i = 0; i < 199; i = i + 1) begin @(posedge clk); #1; end
        data_valid = 0; repeat (10) @(posedge clk); #1;
        check(out_count == 50, "impulse run: 200 inputs -> 50 outputs");
        check(impulse_sum == 2498, "impulse response sums to 2498 (0+146+1318+986+48, floor >>10)");
        check(imp_out[0] == 0 && imp_out[1] == 146 && imp_out[2] == 1318 &&
              imp_out[3] == 986 && imp_out[4] == 48 && imp_out[5] == 0,
              "impulse response taps exact: 0,146,1318,986,48,0");

        // --- data_valid gaps: outputs only count valid inputs ---
        reset_dut;
        for (i = 0; i < 80; i = i + 1) begin
            data_valid = (i % 2 == 0); data_in = 16'sd1000; @(posedge clk); #1;
        end
        data_valid = 0; repeat (10) @(posedge clk); #1;
        check(out_count == 10, "40 valid inputs with gaps -> 10 outputs");

        $display("\nResults: %0d/%0d passed", pass_count, pass_count + fail_count);
        $finish;
    end
endmodule
```

- [ ] **Step 2: Run it to see it fail**

Run: `iverilog -g2001 -DSIMULATION -o /tmp/tb_cic.vvp tb/tb_cic_decimator.v cic_decimator_4x_enhanced.v`
Expected: compile errors (`DATA_W`/`LOG2_R` are not parameters of the old module; ports `saturation_detected`… missing in the TB are OK, but the 48-bit old design does not have `DATA_W`).

- [ ] **Step 3: Rewrite `cic_decimator_4x_enhanced.v`**

```verilog
`timescale 1ns / 1ps
// ============================================================================
// cic_decimator_4x_enhanced.v — 5-stage CIC decimator, R = 4, M = 1
//
// Register width from Hogenauer:  B_max = N * log2(R*M) + B_in
//                                      = STAGES * LOG2_R + DATA_W = 26 bits
// All integrators and combs are ACC_W wide and wrap (modular arithmetic);
// the result is exact.  Gain = (R*M)^N = 2^(STAGES*LOG2_R) = 1024, removed
// by taking the top DATA_W bits of the last comb (an exact >>> 10), so DC
// gain is exactly 1 and no saturation logic is needed.
//
// Structure (one clock per stage, all enabled by valid):
//   integ[0] <= integ[0] + x ; integ[k] <= integ[k] + integ[k-1]      (N stages)
//   every R-th valid input:  sampled <= integ[N-1]                     (decimate)
//   comb[0] <= sampled - comb_d[0]; comb[k] <= comb[k-1] - comb_d[k]   (N stages)
//   data_out <= comb[N-1][ACC_W-1 : ACC_W-DATA_W]
// With x[n] the n-th valid input and S_N its N-fold cumulative sum, the j-th
// output is  Delta^N S_N[4j-2] >> 10  (the k-th integrator lags k samples and
// the decimator captures the integrator value BEFORE the 4j+3-rd update).
// Latency: output j is registered 6 edges after input 4j+3.
//
// Resources: 2*N*ACC_W + 2*DATA_W + ~10 flip-flops, 0 multipliers, 0 RAM.
// ============================================================================
module cic_decimator_4x_enhanced #(
    parameter DATA_W = 16,
    parameter STAGES = 5,
    parameter LOG2_R = 2
)(
    input  wire                     clk,
    input  wire                     reset_n,
    input  wire signed [DATA_W-1:0] data_in,
    input  wire                     data_valid,
    output reg  signed [DATA_W-1:0] data_out,
    output reg                      data_out_valid
);

localparam R     = (1 << LOG2_R);
localparam ACC_W = DATA_W + STAGES * LOG2_R;

reg signed [ACC_W-1:0] integ  [0:STAGES-1];
reg signed [ACC_W-1:0] comb   [0:STAGES-1];
reg signed [ACC_W-1:0] comb_d [0:STAGES-1];
reg        [STAGES-1:0] comb_valid;
reg        [LOG2_R-1:0] decim_cnt;
reg signed [ACC_W-1:0] sampled;
reg                    sampled_valid;

integer k;

// ---- Integrator cascade ----
always @(posedge clk) begin
    if (!reset_n) begin
        for (k = 0; k < STAGES; k = k + 1) integ[k] <= {ACC_W{1'b0}};
    end else if (data_valid) begin
        integ[0] <= integ[0] + {{(ACC_W-DATA_W){data_in[DATA_W-1]}}, data_in};
        for (k = 1; k < STAGES; k = k + 1) integ[k] <= integ[k] + integ[k-1];
    end
end

// ---- Decimation ----
always @(posedge clk) begin
    if (!reset_n) begin
        decim_cnt     <= {LOG2_R{1'b0}};
        sampled       <= {ACC_W{1'b0}};
        sampled_valid <= 1'b0;
    end else begin
        sampled_valid <= 1'b0;
        if (data_valid) begin
            if (decim_cnt == R - 1) begin
                decim_cnt     <= {LOG2_R{1'b0}};
                sampled       <= integ[STAGES-1];
                sampled_valid <= 1'b1;
            end else begin
                decim_cnt <= decim_cnt + 1'b1;
            end
        end
    end
end

// ---- Comb cascade + output ----
always @(posedge clk) begin
    if (!reset_n) begin
        for (k = 0; k < STAGES; k = k + 1) begin
            comb[k]   <= {ACC_W{1'b0}};
            comb_d[k] <= {ACC_W{1'b0}};
        end
        comb_valid     <= {STAGES{1'b0}};
        data_out       <= {DATA_W{1'b0}};
        data_out_valid <= 1'b0;
    end else begin
        comb_valid <= {comb_valid[STAGES-2:0], sampled_valid};
        if (sampled_valid) begin
            comb[0]   <= sampled - comb_d[0];
            comb_d[0] <= sampled;
        end
        for (k = 1; k < STAGES; k = k + 1) begin
            if (comb_valid[k-1]) begin
                comb[k]   <= comb[k-1] - comb_d[k];
                comb_d[k] <= comb[k-1];
            end
        end
        data_out_valid <= comb_valid[STAGES-1];
        if (comb_valid[STAGES-1])
            data_out <= comb[STAGES-1][ACC_W-1:ACC_W-DATA_W];
    end
end

endmodule
```

- [ ] **Step 4: Run the test**

Run: `iverilog -g2001 -DSIMULATION -o /tmp/tb_cic.vvp tb/tb_cic_decimator.v cic_decimator_4x_enhanced.v && vvp /tmp/tb_cic.vvp | grep -E "^\["`
Expected: 8 `[PASS`, 0 `[FAIL`. (The impulse taps 0,146,1318,986,48 were computed with the integer model `Delta^5 S5[4j-2] >> 10` — the same model `gen_ddc_golden.py` uses in Task 6; if the RTL differs by one output position, the decimation capture point is wrong, not the model.)

- [ ] **Step 5: Keep the old DDC compiling until Task 6**

In `ddc_400m.v` the two CIC instances (lines 566–582) pass `mixed_i[33:16]` (18 bits) and read 18-bit outputs. Edit them to the new 16-bit interface so the production lint and the receiver tests still compile and run:

```verilog
cic_decimator_4x_enhanced #(.DATA_W(16)) cic_i_inst (
    .clk(clk_400m),
    .reset_n(reset_n_400m),
    .data_in(mixed_i[33:18]),
    .data_valid(mixed_valid),
    .data_out(cic_i_out[15:0]),
    .data_out_valid(cic_valid_i)
);

cic_decimator_4x_enhanced #(.DATA_W(16)) cic_q_inst (
    .clk(clk_400m),
    .reset_n(reset_n_400m),
    .data_in(mixed_q[33:18]),
    .data_valid(mixed_valid),
    .data_out(cic_q_out[15:0]),
    .data_out_valid(cic_valid_q)
);
assign cic_i_out[17:16] = {2{cic_i_out[15]}};
assign cic_q_out[17:16] = {2{cic_q_out[15]}};
```

(The old `wire [17:0] cic_i_out, cic_q_out;` declarations stay. This is throwaway glue; `ddc_400m.v` is deleted in Task 6.)

Run: `./run_regression.sh 2>&1 | tail -6`
Expected: `Tests: 28 passed, 0 failed`. The "Receiver (golden generate)" run rewrites `tb/golden/golden_doppler.mem` and the compare run must match it; the DDC cosim test (`tb_ddc_cosim.v`) only checks output counts and still passes.

- [ ] **Step 6: Commit**

```bash
git add cic_decimator_4x_enhanced.v tb/tb_cic_decimator.v ddc_400m.v tb/golden/golden_doppler.mem
git commit -m "fpga: CIC with Hogenauer widths (26-bit), inferred adders, monitors removed

Co-Authored-By: Claude Fable 5.1 <noreply@anthropic.com>"
```

---

### Task 6: `ddc.v` (12-bit mixer, NCO, CIC) + ADC front-end integration + golden test (a)

**Files:**
- Create: `ddc.v`, `tb/golden/radar_params.py`, `tb/golden/gen_ddc_golden.py`, `tb/golden/tb_ddc_golden.v`
- Modify: `radar_receiver_final.v:1-12, 173-226`, `radar_system_top.v:12-16, 63-70, 511-516`
- Modify: `tb/tb_radar_receiver_final.v`, `tb/radar_system_tb.v`, `tb/tb_system_e2e.v`, `run_regression.sh`
- Modify: `cdc_modules.v` (delete `cdc_adc_to_processing`), `tb/tb_cdc_modules.v` (delete Section A / module 1)
- Delete: `ddc_400m.v`, `nco_400m_enhanced.v`, `ad9484_interface_400m.v`, `adc_clk_mmcm.v`, `adc_clk_mmcm_integration.md`, `radar_system_top_50t.v`, `tb/ad9484_interface_400m_stub.v`, `tb/tb_ad9484_xsim.v`, `tb/tb_ddc_400m.v`, `tb/tb_ddc_cosim.v`, `formal/fv_cdc_adc.v`, `formal/fv_cdc_adc.sby`

**Interfaces:**
- Consumes: `nco` (Task 4), `cic_decimator_4x_enhanced #(DATA_W=16)` (Task 5), `adc_cmos_interface` (Task 3), the existing `fir_lowpass_parallel_enhanced` (18-bit in/out, replaced in Task 7).
- Produces: `module ddc #(parameter ADC_W = 12, NCO_W = 16, OUT_W = 18, PHASE_INC = 32'h3333_3333, DITHER_EN = 1) (input clk, reset_n, mixers_enable, input [ADC_W-1:0] adc_data, input adc_valid, output reg signed [OUT_W-1:0] baseband_i, baseband_q, output reg baseband_valid)`. Mixer truncation: `mixed = (adc_signed * nco) >> 12`, 16 bits into the CIC. Task 7 changes `OUT_W` to 16.
- `radar_receiver_final` new ports: `input [11:0] adc_data, input adc_ovr, output adc_pwdn, output adc_overrange` (LVDS ports removed). `radar_system_top` same four ports.
- Python: `tb/golden/radar_params.py` exports `FS_ADC, FS_BB, F_IF, CHIRP_BW, T_LONG, T_SHORT, N_FFT, LOG2N, OVERLAP, ADVANCE, PHASE_INC, NCO_SINE_LUT, FIR_COEFFS, DDC_OUT_W, write_hex(path, values, width), read_hex(path, width), if_chirp_adc(n, amp, delay, seed), baseband_chirp(n_samples, t_chirp)`.

- [ ] **Step 1: Shared parameters module**

Create `tb/golden/radar_params.py`:

```python
#!/usr/bin/env python3
"""Shared radar/DDC parameters and helpers for the golden generators.

Every constant here mirrors a parameter in the RTL; the RTL header that owns
it is named in the comment.
"""
import math
import numpy as np

FS_ADC   = 100e6          # adc_cmos_interface: ADC DCO = clk_proc
FS_BB    = 25e6           # cic_decimator_4x_enhanced: R = 4
F_IF     = 20e6           # nco.v PHASE_INC default
CHIRP_BW = 20e6           # 10..30 MHz IF chirp
T_LONG   = 30e-6          # radar_mode_controller LONG_CHIRP_CYCLES = 3000 @ 100 MHz
T_SHORT  = 0.5e-6         # SHORT_CHIRP_CYCLES = 50
N_FFT    = 256            # matched_filter_processing_chain N_FFT
LOG2N    = 8
OVERLAP  = 32             # matched_filter_multi_segment OVERLAP
ADVANCE  = N_FFT - OVERLAP
LONG_CHIRP_SAMPLES  = int(round(T_LONG * FS_BB))        # 750
SHORT_CHIRP_SAMPLES = int(math.ceil(T_SHORT * FS_BB))   # 13
LONG_SEGMENTS = 4           # 256 + 224*3 = 928 >= 750 (3 segments cover only 704)
PHASE_INC = 0x33333333      # nco.v: round(0.2 * 2^32) truncated to 0x33333333
DDC_OUT_W = 18              # ddc.v OUT_W (Task 7 sets 16)

# nco.v quarter-wave LUT: round(32767*sin(pi/2*k/64))
NCO_SINE_LUT = [
    0x0000, 0x0324, 0x0648, 0x096A, 0x0C8C, 0x0FAB, 0x12C8, 0x15E2,
    0x18F9, 0x1C0B, 0x1F1A, 0x2223, 0x2528, 0x2826, 0x2B1F, 0x2E11,
    0x30FB, 0x33DF, 0x36BA, 0x398C, 0x3C56, 0x3F17, 0x41CE, 0x447A,
    0x471C, 0x49B4, 0x4C3F, 0x4EBF, 0x5133, 0x539B, 0x55F5, 0x5842,
    0x5A82, 0x5CB3, 0x5ED7, 0x60EB, 0x62F1, 0x64E8, 0x66CF, 0x68A6,
    0x6A6D, 0x6C23, 0x6DC9, 0x6F5E, 0x70E2, 0x7254, 0x73B5, 0x7504,
    0x7641, 0x776B, 0x7884, 0x7989, 0x7A7C, 0x7B5C, 0x7C29, 0x7CE3,
    0x7D89, 0x7E1D, 0x7E9C, 0x7F09, 0x7F61, 0x7FA6, 0x7FD8, 0x7FF5,
]

# fir_lowpass.v coefficients (18-bit two's complement, symmetric, Q1.17)
_FIR_HEX = [
    0x000AD, 0x000CE, 0x3FD87, 0x002A6, 0x000E0, 0x3F8C0, 0x00A45, 0x3FD82,
    0x3F0B5, 0x01CAD, 0x3EE59, 0x3E821, 0x04841, 0x3B340, 0x3E299, 0x1FFFF,
    0x1FFFF, 0x3E299, 0x3B340, 0x04841, 0x3E821, 0x3EE59, 0x01CAD, 0x3F0B5,
    0x3FD82, 0x00A45, 0x3F8C0, 0x000E0, 0x002A6, 0x3FD87, 0x000CE, 0x000AD,
]


def sext(value, bits):
    value &= (1 << bits) - 1
    return value - (1 << bits) if value & (1 << (bits - 1)) else value


FIR_COEFFS = [sext(c, 18) for c in _FIR_HEX]
assert FIR_COEFFS == FIR_COEFFS[::-1], "FIR coefficients must be symmetric"


def write_hex(path, values, width):
    """One value per line, two's complement, width bits, zero-padded hex."""
    digits = (width + 3) // 4
    mask = (1 << width) - 1
    with open(path, "w") as f:
        for v in values:
            f.write(f"{int(v) & mask:0{digits}X}\n")


def read_hex(path, width):
    out = []
    with open(path) as f:
        for line in f:
            line = line.strip()
            if not line or line.startswith("//"):
                continue
            out.append(sext(int(line, 16), width))
    return out


def if_chirp_adc(n_samples, amp, delay, seed=1):
    """12-bit offset-binary ADC samples: 10->30 MHz IF chirp (T_LONG) starting
    at sample `delay`, mid-scale elsewhere, plus +-2 LSB noise."""
    rng = np.random.default_rng(seed)
    n = np.arange(n_samples)
    t = (n - delay) / FS_ADC
    f0 = F_IF - CHIRP_BW / 2
    k = CHIRP_BW / T_LONG
    phase = 2 * np.pi * (f0 * t + 0.5 * k * t * t)
    active = (n >= delay) & (n < delay + int(T_LONG * FS_ADC))
    sig = np.where(active, amp * np.cos(phase), 0.0)
    noise = rng.normal(0.0, 1.0, n_samples)
    adc = np.rint(sig + noise).astype(np.int64) + 2048
    return np.clip(adc, 0, 4095)


def baseband_chirp(n_samples, t_chirp):
    """Complex baseband chirp at FS_BB as float arrays (i, q), unit amplitude,
    phase = pi * (BW/T) * t^2 (same formula as the old gen_chirp_mem.py)."""
    n = np.arange(n_samples)
    t = n / FS_BB
    rate = CHIRP_BW / t_chirp
    phase = np.pi * rate * t * t
    return np.cos(phase), np.sin(phase)
```

- [ ] **Step 2: Golden generator for the DDC (bit-exact integer model)**

Create `tb/golden/gen_ddc_golden.py`:

```python
#!/usr/bin/env python3
"""gen_ddc_golden.py — golden test (a): bit-exact DDC vectors.

Integer replica of ddc.v (NCO -> 12x16 mixer -> CIC R=4 -> FIR):
  nco     : phase[n] = PHASE_INC*n mod 2^32, 8-bit truncation, quarter-wave LUT
  mixer   : mixed = (adc_signed * nco) >> 12   (floor), 16 bits
  cic     : Delta^5 S5[4j-2] >> 10, 26-bit wrap  (see cic_decimator_4x_enhanced.v)
  fir     : direct form, acc = sum c[k]*u[j-k]; output per DDC_OUT_W
Writes ddc_adc_in.hex (12-bit), ddc_golden_i.hex, ddc_golden_q.hex.
Run from 9_Firmware/9_2_FPGA:  python3 tb/golden/gen_ddc_golden.py
"""
import os
import sys

import numpy as np

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from radar_params import (DDC_OUT_W, FIR_COEFFS, NCO_SINE_LUT, PHASE_INC,  # noqa: E402
                          if_chirp_adc, sext, write_hex)

N_IN   = 4096
ACC_W  = 26
MASK26 = (1 << ACC_W) - 1
HERE   = os.path.dirname(os.path.abspath(__file__))


def nco_sin_cos(n_samples):
    phase = (np.uint64(PHASE_INC) * np.arange(n_samples, dtype=np.uint64)) & np.uint64(0xFFFFFFFF)
    addr = (phase >> np.uint64(24)).astype(np.int64)
    quadrant = addr >> 6
    idx = addr & 63
    flip = ((quadrant & 1) ^ ((quadrant >> 1) & 1)) == 1
    idx = np.where(flip, (~idx) & 63, idx)
    lut = np.array(NCO_SINE_LUT, dtype=np.int64)
    sin_abs, cos_abs = lut[idx], lut[63 - idx]
    sin = np.where((quadrant == 0) | (quadrant == 1), sin_abs, -sin_abs)
    cos = np.where((quadrant == 0) | (quadrant == 3), cos_abs, -cos_abs)
    return sin, cos


def cic_r4_n5(x):
    s = np.asarray(x, dtype=np.int64)
    for _ in range(5):
        s = np.cumsum(s) & MASK26
    spad = np.concatenate([[0, 0], s])
    v = np.array([spad[4 * j] for j in range(len(x) // 4)], dtype=np.int64)  # S5[4j-2]
    for _ in range(5):
        v = (v - np.concatenate([[0], v[:-1]])) & MASK26
    return np.array([sext(int(a), ACC_W) >> 10 for a in v], dtype=np.int64)


def fir(u, out_w):
    c = np.array(FIR_COEFFS, dtype=np.int64)
    acc = np.convolve(np.asarray(u, dtype=np.int64), c)[:len(u)]   # |acc| < 2^34
    if out_w == 18:   # legacy fir_lowpass_parallel_enhanced: saturate at 2^34, take acc[34:17]
        out = []
        for a in acc:
            if a > (1 << 34) - 1:
                out.append((1 << 17) - 1)
            elif a < -(1 << 34):
                out.append(-(1 << 17))
            else:
                out.append(sext((int(a) >> 17) & 0x3FFFF, 18))
        return out
    # fir_lowpass (Task 7): acc >>> 17, saturate to 16 bits
    return [int(min(max(int(a) >> 17, -32768), 32767)) for a in acc]


def main():
    adc = if_chirp_adc(N_IN, amp=1000.0, delay=0, seed=1)
    adc_signed = adc - 2048
    sin, cos = nco_sin_cos(N_IN)
    mix_i = (adc_signed * cos) >> 12
    mix_q = (adc_signed * sin) >> 12
    cic_i, cic_q = cic_r4_n5(mix_i), cic_r4_n5(mix_q)
    out_i, out_q = fir(cic_i, DDC_OUT_W), fir(cic_q, DDC_OUT_W)
    write_hex(os.path.join(HERE, "ddc_adc_in.hex"), adc, 12)
    write_hex(os.path.join(HERE, "ddc_golden_i.hex"), out_i, DDC_OUT_W)
    write_hex(os.path.join(HERE, "ddc_golden_q.hex"), out_q, DDC_OUT_W)
    print(f"wrote {N_IN} ADC samples, {len(out_i)} baseband samples "
          f"(out width {DDC_OUT_W}), peak |I| = {max(abs(v) for v in out_i)}")


if __name__ == "__main__":
    main()
```

Run: `python3 tb/golden/gen_ddc_golden.py`
Expected: `wrote 4096 ADC samples, 1024 baseband samples (out width 18), peak |I| = <some value between 5000 and 20000>`.

- [ ] **Step 3: Write the failing golden testbench**

Create `tb/golden/tb_ddc_golden.v`:

```verilog
`timescale 1ns / 1ps
// ============================================================================
// tb_ddc_golden.v — golden test (a): ddc.v vs the integer model in
// tb/golden/gen_ddc_golden.py.  Exact match required (tolerance 0).
// DITHER_EN = 0 so the NCO phase of sample n is PHASE_INC*n.
// ============================================================================
module tb_ddc_golden;
    localparam CLK_PERIOD = 10.0;
    localparam N_IN   = 4096;
    localparam N_OUT  = N_IN / 4;
    localparam OUT_W  = 18;       // Task 7 -> 16

    reg clk, reset_n, adc_valid;
    reg  [11:0] adc_data;
    wire signed [OUT_W-1:0] bb_i, bb_q;
    wire bb_valid;

    reg [11:0]      adc_mem   [0:N_IN-1];
    reg [OUT_W-1:0] gold_i    [0:N_OUT-1];
    reg [OUT_W-1:0] gold_q    [0:N_OUT-1];
    integer out_count, mismatches, first_bad, i;
    integer pass_count, fail_count, test_num;

    always #(CLK_PERIOD/2) clk = ~clk;

    ddc #(.ADC_W(12), .OUT_W(OUT_W), .DITHER_EN(0)) uut (
        .clk(clk), .reset_n(reset_n), .mixers_enable(1'b1),
        .adc_data(adc_data), .adc_valid(adc_valid),
        .baseband_i(bb_i), .baseband_q(bb_q), .baseband_valid(bb_valid));

    task check;
        input cond; input [511:0] label;
        begin
            test_num = test_num + 1;
            if (cond) begin $display("[PASS] Test %0d: %0s", test_num, label); pass_count = pass_count + 1; end
            else       begin $display("[FAIL] Test %0d: %0s", test_num, label); fail_count = fail_count + 1; end
        end
    endtask

    always @(posedge clk) begin
        if (reset_n && bb_valid) begin
            if (out_count < N_OUT) begin
                if (bb_i !== $signed(gold_i[out_count]) || bb_q !== $signed(gold_q[out_count])) begin
                    if (mismatches < 10)
                        $display("  mismatch @%0d: rtl=(%0d,%0d) gold=(%0d,%0d)", out_count,
                                 bb_i, bb_q, $signed(gold_i[out_count]), $signed(gold_q[out_count]));
                    if (first_bad < 0) first_bad = out_count;
                    mismatches = mismatches + 1;
                end
            end
            out_count = out_count + 1;
        end
    end

    initial begin
        $readmemh("tb/golden/ddc_adc_in.hex", adc_mem);
        $readmemh("tb/golden/ddc_golden_i.hex", gold_i);
        $readmemh("tb/golden/ddc_golden_q.hex", gold_q);
        clk = 0; reset_n = 0; adc_valid = 0; adc_data = 12'h800;
        out_count = 0; mismatches = 0; first_bad = -1;
        pass_count = 0; fail_count = 0; test_num = 0;
        repeat (4) @(posedge clk); #1;
        reset_n = 1;
        repeat (4) @(posedge clk); #1;          // let the mixers_enable synchronizer settle
        for (i = 0; i < N_IN; i = i + 1) begin
            adc_data = adc_mem[i]; adc_valid = 1'b1;
            @(posedge clk); #1;
        end
        adc_valid = 1'b0; adc_data = 12'h800;
        repeat (60) @(posedge clk); #1;        // drain NCO(4)+mixer(3)+CIC(6)+FIR(9)+out(1)
        $display("outputs=%0d mismatches=%0d first_bad=%0d", out_count, mismatches, first_bad);
        check(out_count == N_OUT, "exactly N_IN/4 baseband samples");
        check(mismatches == 0, "all baseband samples match the integer model exactly");
        $display("\nResults: %0d/%0d passed", pass_count, pass_count + fail_count);
        $finish;
    end
endmodule
```

Run: `iverilog -g2001 -DSIMULATION -o /tmp/tb_ddcg.vvp tb/golden/tb_ddc_golden.v ddc.v nco.v cic_decimator_4x_enhanced.v fir_lowpass.v`
Expected: `Unable to open input file "ddc.v"`.

- [ ] **Step 4: Write `ddc.v`**

```verilog
`timescale 1ns / 1ps
// ============================================================================
// ddc.v — digital down-converter for the 12-bit, 100 MSPS, 20 MHz-IF front end
//
//   adc_data (12-bit offset binary, 100 MSPS)
//     -> 4-register delay (matches NCO latency)        12 bit
//     -> offset-binary to two's complement             12 bit signed
//     -> mixer  I = adc * cos, Q = adc * sin           12 x 16 -> 28 bit
//     -> truncate [27:12]                               16 bit  (|.| <= 2^14)
//     -> CIC N=5, R=4                                   16 bit @ 25 MSPS
//     -> FIR 32 taps (symmetric)                        OUT_W @ 25 MSPS
//
// Mixer truncation: adc_signed is an integer in [-2048, 2047]; the NCO is Q15
// (+-32767).  The 28-bit product has 15 fractional bits; keeping bits [27:12]
// drops 12 of them, so full-scale ADC x full-scale NCO = +-16384 (bit 14) and
// one bit of headroom is left for the CIC pass-band ripple.
//
// NCO phase alignment: the NCO advances only on accepted samples and starts
// at phase 0, so sample n is mixed with phase PHASE_INC*n (DITHER_EN = 0).
// The sample is delayed NCO_LAT = 4 registers to meet the NCO's latency
// (see nco.v "Timing contract").
//
// Latency (clk edges, sample presented -> baseband_valid):
//   NCO/delay 4 + mixer 3 + CIC 6 (per decimated output) + FIR + 1.
//
// Resources: 2 multipliers (12x16), 0 RAM.  Sub-blocks: nco (0 mult),
// 2 x cic_decimator_4x_enhanced (0 mult), 2 x FIR.
// ============================================================================
module ddc #(
    parameter ADC_W     = 12,
    parameter NCO_W     = 16,
    parameter OUT_W     = 18,
    parameter PHASE_INC = 32'h3333_3333,
    parameter DITHER_EN = 1
)(
    input  wire                    clk,
    input  wire                    reset_n,
    input  wire                    mixers_enable,
    input  wire [ADC_W-1:0]        adc_data,
    input  wire                    adc_valid,
    output reg  signed [OUT_W-1:0] baseband_i,
    output reg  signed [OUT_W-1:0] baseband_q,
    output reg                     baseband_valid
);

localparam PROD_W  = ADC_W + NCO_W;   // 28
localparam CIC_W   = 16;
localparam NCO_LAT = 4;

// ---- mixers_enable synchronizer (async GPIO from the STM32) ----
reg [1:0] en_sync;
always @(posedge clk or negedge reset_n) begin
    if (!reset_n) en_sync <= 2'b00;
    else          en_sync <= {en_sync[0], mixers_enable};
end
wire sample_en = adc_valid & en_sync[1];

// ---- NCO ----
wire signed [NCO_W-1:0] sin_w, cos_w;
nco #(
    .PHASE_INC(PHASE_INC),
    .DITHER_EN(DITHER_EN)
) nco_inst (
    .clk(clk),
    .reset_n(reset_n),
    .phase_valid(sample_en),
    .phase_offset(16'h0000),
    .sin_out(sin_w),
    .cos_out(cos_w),
    .dds_ready()
);

// ---- ADC delay line matching the NCO latency ----
reg [ADC_W-1:0]   adc_dly [0:NCO_LAT-1];
reg [NCO_LAT-1:0] valid_dly;
integer d;
always @(posedge clk or negedge reset_n) begin
    if (!reset_n) begin
        for (d = 0; d < NCO_LAT; d = d + 1) adc_dly[d] <= {ADC_W{1'b0}};
        valid_dly <= {NCO_LAT{1'b0}};
    end else begin
        adc_dly[0] <= adc_data;
        for (d = 1; d < NCO_LAT; d = d + 1) adc_dly[d] <= adc_dly[d-1];
        valid_dly <= {valid_dly[NCO_LAT-2:0], sample_en};
    end
end

// Offset binary -> two's complement (invert the MSB)
wire signed [ADC_W-1:0] adc_signed_w = {~adc_dly[NCO_LAT-1][ADC_W-1], adc_dly[NCO_LAT-1][ADC_W-2:0]};

// ---- Mixer: operand regs -> product regs -> truncated output regs ----
reg signed [ADC_W-1:0]  adc_r;
reg signed [NCO_W-1:0]  cos_r, sin_r;
reg signed [PROD_W-1:0] prod_i_r, prod_q_r;
reg signed [CIC_W-1:0]  mixed_i, mixed_q;
reg [2:0]               mix_valid;

always @(posedge clk or negedge reset_n) begin
    if (!reset_n) begin
        adc_r     <= {ADC_W{1'b0}};
        cos_r     <= {NCO_W{1'b0}};
        sin_r     <= {NCO_W{1'b0}};
        prod_i_r  <= {PROD_W{1'b0}};
        prod_q_r  <= {PROD_W{1'b0}};
        mixed_i   <= {CIC_W{1'b0}};
        mixed_q   <= {CIC_W{1'b0}};
        mix_valid <= 3'b000;
    end else begin
        adc_r     <= adc_signed_w;
        cos_r     <= cos_w;
        sin_r     <= sin_w;
        prod_i_r  <= adc_r * cos_r;
        prod_q_r  <= adc_r * sin_r;
        mixed_i   <= prod_i_r[PROD_W-1:PROD_W-CIC_W];
        mixed_q   <= prod_q_r[PROD_W-1:PROD_W-CIC_W];
        mix_valid <= {mix_valid[1:0], valid_dly[NCO_LAT-1]};
    end
end

// ---- CIC decimators (100 MSPS -> 25 MSPS) ----
wire signed [CIC_W-1:0] cic_i_out, cic_q_out;
wire cic_valid_i, cic_valid_q;

cic_decimator_4x_enhanced #(.DATA_W(CIC_W)) cic_i_inst (
    .clk(clk), .reset_n(reset_n),
    .data_in(mixed_i), .data_valid(mix_valid[2]),
    .data_out(cic_i_out), .data_out_valid(cic_valid_i));

cic_decimator_4x_enhanced #(.DATA_W(CIC_W)) cic_q_inst (
    .clk(clk), .reset_n(reset_n),
    .data_in(mixed_q), .data_valid(mix_valid[2]),
    .data_out(cic_q_out), .data_out_valid(cic_valid_q));

// ---- FIR low-pass (one valid input every 4 clocks) ----
wire signed [OUT_W-1:0] fir_i_out, fir_q_out;
wire fir_valid_i, fir_valid_q;

fir_lowpass_parallel_enhanced fir_i_inst (
    .clk(clk), .reset_n(reset_n),
    .data_in({{(OUT_W-CIC_W){cic_i_out[CIC_W-1]}}, cic_i_out}), .data_valid(cic_valid_i),
    .data_out(fir_i_out), .data_out_valid(fir_valid_i),
    .fir_ready(), .filter_overflow());

fir_lowpass_parallel_enhanced fir_q_inst (
    .clk(clk), .reset_n(reset_n),
    .data_in({{(OUT_W-CIC_W){cic_q_out[CIC_W-1]}}, cic_q_out}), .data_valid(cic_valid_q),
    .data_out(fir_q_out), .data_out_valid(fir_valid_q),
    .fir_ready(), .filter_overflow());

// ---- Output register ----
always @(posedge clk or negedge reset_n) begin
    if (!reset_n) begin
        baseband_i     <= {OUT_W{1'b0}};
        baseband_q     <= {OUT_W{1'b0}};
        baseband_valid <= 1'b0;
    end else begin
        baseband_valid <= fir_valid_i & fir_valid_q;
        if (fir_valid_i & fir_valid_q) begin
            baseband_i <= fir_i_out;
            baseband_q <= fir_q_out;
        end
    end
end

endmodule
```

- [ ] **Step 5: Run golden test (a)**

Run: `iverilog -g2001 -DSIMULATION -o /tmp/tb_ddcg.vvp tb/golden/tb_ddc_golden.v ddc.v nco.v cic_decimator_4x_enhanced.v fir_lowpass.v && vvp /tmp/tb_ddcg.vvp | tail -5`
Expected: `outputs=1024 mismatches=0 first_bad=-1`, 2 `[PASS`. Debug guide if it fails: a *rotating* error (RTL = golden × e^{jφ}) means the NCO/ADC alignment is off by one — check `NCO_LAT` (4) against the nco pipeline; an error that starts at output 0 and persists with constant magnitude means a CIC capture-phase or comb-history difference; an error only in the first 8 outputs means a FIR pipeline/valid issue.

- [ ] **Step 6: Wire the new front end into `radar_receiver_final.v`**

Replace lines 3–12 (module header through `adc_pwdn`) with:

```verilog
module radar_receiver_final (
    input wire clk,           // processing clock = ADC data clock (nominal 100 MHz)
    input wire reset_n,

    // ADC CMOS parallel interface (12-bit offset binary, one sample per clk)
    input wire [11:0] adc_data,
    input wire        adc_ovr,            // ADC over-range pin
    output wire       adc_pwdn,
    output wire       adc_overrange,      // sticky, cleared at every Doppler frame end
```

Replace lines 173–226 (`wire clk_400m;` through the `ddc_400m_enhanced ddc(...)` instance) with:

```verilog
// 1. ADC capture (single edge, clk = ADC DCO) and power-down (always on)
wire [11:0] adc_sample;
wire        adc_sample_valid;
assign adc_pwdn = 1'b0;

adc_cmos_interface #(.DATA_W(12)) adc_if (
    .adc_clk(clk),
    .reset_n(reset_n),
    .adc_data(adc_data),
    .adc_ovr(adc_ovr),
    .overrange_clear(doppler_frame_done),
    .sample(adc_sample),
    .sample_valid(adc_sample_valid),
    .overrange(adc_overrange)
);

// 2. DDC: NCO (20 MHz) + mixer + CIC (R=4) + FIR -> 25 MSPS complex baseband
wire signed [17:0] ddc_out_i;
wire signed [17:0] ddc_out_q;
wire ddc_valid;

ddc #(.ADC_W(12), .OUT_W(18)) ddc_inst (
    .clk(clk),
    .reset_n(reset_n),
    .mixers_enable(1'b1),
    .adc_data(adc_sample),
    .adc_valid(adc_sample_valid),
    .baseband_i(ddc_out_i),
    .baseband_q(ddc_out_q),
    .baseband_valid(ddc_valid)
);
```

In the `ddc_input_interface ddc_if (...)` instance that follows, change `.valid_i(ddc_valid_i), .valid_q(ddc_valid_q)` to `.valid_i(ddc_valid), .valid_q(ddc_valid)`. Delete the old `wire ddc_valid_i; wire ddc_valid_q;` lines. Line 70-72 comments: `// DDC output I (16-bit signed, 25 MSPS)` etc.

- [ ] **Step 7: Update `radar_system_top.v`**

Replace lines 65–70 (the LVDS ADC port block) with:

```verilog
    // ADC CMOS parallel interface (12-bit, sampled on clk_100m = ADC data clock)
    input wire [11:0] adc_data,
    input wire adc_ovr,                   // ADC over-range pin
    output wire adc_pwdn,                 // ADC power down
    output wire adc_overrange,            // sticky over-range flag (status)
```

Replace lines 511–516 in the `rx_inst` instance with:

```verilog
    // ADC interface
    .adc_data(adc_data),
    .adc_ovr(adc_ovr),
    .adc_pwdn(adc_pwdn),
    .adc_overrange(adc_overrange),
```

Edit the header comment (lines 12–16) to:

```verilog
 * Clock domains:
 * - clk_100m: processing clock. MUST be driven by the ADC data clock
 *             (adc_cmos_interface samples on it; nominal 100 MHz)
 * - clk_120m_dac: DAC clock (120MHz)
 * - ft601_clk: USB interface clock (100MHz FT601 or 60MHz FT2232H)
```

- [ ] **Step 8: Update the three testbenches that drive the ADC**

`tb/tb_radar_receiver_final.v`:
- delete `reg clk_400m;` (57) and its generator (65–66);
- replace the ADC stimulus block (74–89) with:

```verilog
// Feed a 20 MHz tone (IF) sampled at 100 MHz: phase step = 0.2 * 65536 = 13107.
// phase_acc[15:4] is a 12-bit sawtooth with strong energy at the IF.
reg [11:0] adc_data;
reg [15:0] phase_acc;
localparam [15:0] PHASE_INC = 16'd13107;

always @(posedge clk_100m or negedge reset_n) begin
    if (!reset_n) begin
        phase_acc <= 16'd0;
        adc_data  <= 12'd2048;
    end else begin
        phase_acc <= phase_acc + PHASE_INC;
        adc_data  <= phase_acc[15:4];
    end
end
```
- replace the DUT ADC port connections (139–144) with `.adc_data(adc_data), .adc_ovr(1'b0), .adc_pwdn(), .adc_overrange(),`;
- line 268: `dut.ddc_valid_i` → `dut.ddc_valid`;
- header comments 6, 34, 43–45: replace `ad9484_interface (stub) -> CDC -> DDC` with `adc_cmos_interface -> DDC`, drop the "behavioral stub" bullet, change "Feeds 120 MHz tone" to "Feeds 20 MHz tone".

`tb/radar_system_tb.v`:
- delete `parameter ADC_DCO_PERIOD = 2.5;` (24), `reg adc_dco_p; reg adc_dco_n;` (45–46), `reg [7:0] adc_d_p; reg [7:0] adc_d_n;` (50–51) and the DCO generator (142–150); add `reg [11:0] adc_data;` next to `reg [7:0] adc_data_pattern;`;
- in the ADC stimulus loop (287–311): `adc_d_p = 8'h00; adc_d_n = ~8'h00;` → `adc_data = 12'h800;`, `@(posedge adc_dco_p);` → `@(posedge clk_100m);`, and the two LVDS output lines → `adc_data = {adc_data_pattern, 4'h0};`;
- DUT ports (486–490) → `.adc_data(adc_data), .adc_ovr(1'b0), .adc_pwdn(adc_pwdn), .adc_overrange(),`.

`tb/tb_system_e2e.v`:
- delete `parameter ADC_DCO_PERIOD` (52) and the `adc_dco_p/n` regs + generator (86–94); replace `reg [7:0] adc_d_p;` (103, and `adc_d_n` if declared) with `reg [11:0] adc_data;`;
- DUT ports (426–430) → `.adc_data(adc_data), .adc_ovr(1'b0), .adc_pwdn(adc_pwdn), .adc_overrange(),`;
- stimulus (498–515): `adc_d_p = 8'h80; adc_d_n = 8'h7F;` → `adc_data = 12'h800;`, `@(posedge adc_dco_p);` → `@(posedge clk_100m);`, the pattern lines → `adc_data = {8'h80 + ((adc_phase * 7) & 8'h3F) - 8'h20, 4'h0};` and the else branch → `adc_data = 12'h800;`;
- line 32 comment: drop `tb/ad9484_interface_400m_stub.v`.

Check: `grep -n "adc_d_p\|adc_dco\|clk_400m\|ad9484" tb/tb_radar_receiver_final.v tb/radar_system_tb.v tb/tb_system_e2e.v` → no output.

- [ ] **Step 9: Delete the 400 MHz front end, the Gray-code CDC, and switch the regression lists**

```bash
git rm -q ddc_400m.v nco_400m_enhanced.v ad9484_interface_400m.v adc_clk_mmcm.v adc_clk_mmcm_integration.md \
       radar_system_top_50t.v tb/ad9484_interface_400m_stub.v tb/tb_ad9484_xsim.v tb/tb_ddc_400m.v tb/tb_ddc_cosim.v \
       formal/fv_cdc_adc.v formal/fv_cdc_adc.sby
```

In `cdc_modules.v` delete the `cdc_adc_to_processing` module (from its header comment block through its `endmodule`; it is the first module in the file — verify with `grep -n "^module\|^endmodule" cdc_modules.v`). Check: `grep -rn "cdc_adc_to_processing" *.v tb/*.v formal/` → no output except `tb/tb_cdc_modules.v`.

In `tb/tb_cdc_modules.v`:
1. Delete the block from the decorative line above `// MODULE 1: cdc_adc_to_processing (Gray-code multi-bit CDC)` (line 34) through the `);` that closes `uut_m1` (line 61). Verify the range first with `grep -n "MODULE 1\|MODULE 2" tb/tb_cdc_modules.v`.
2. In the `initial` init block delete the lines that assign `m1_src_clk`, `m1_dst_clk`, `m1_src_reset_n`, `m1_dst_reset_n`, `m1_src_data`, `m1_src_valid` (`grep -n "m1_" tb/tb_cdc_modules.v` lists them).
3. Delete from the decorative line above `// SECTION A: cdc_adc_to_processing tests` (line 130) up to but not including the decorative line above `// SECTION B: cdc_single_bit tests` (line 459).
4. Check: `grep -c "m1_\|cdc_adc_to_processing" tb/tb_cdc_modules.v` → `0`.

In `run_regression.sh`:
- line 497: change `"CDC Modules (3 variants)"` to `"CDC Modules (single-bit + handshake)"`.
- `PROD_RTL` (lines 51–80): replace `tb/ad9484_interface_400m_stub.v`, `ddc_400m.v`, `nco_400m_enhanced.v` with `adc_cmos_interface.v`, `ddc.v`, `nco.v` (keep the rest, order: `radar_receiver_final.v adc_cmos_interface.v ddc.v nco.v cic_decimator_4x_enhanced.v cdc_modules.v fir_lowpass.v ddc_input_interface.v ...`); delete the comment lines 83–85 about the stub.
- `RECEIVER_RTL` (96–106): same substitution.
- Replace the "DDC Chain (NCO→CIC→FIR)" test (412–415) with:

```bash
run_test "DDC golden (a): NCO+mixer+CIC+FIR bit-exact" \
    tb/tb_ddc_golden_reg.vvp \
    tb/golden/tb_ddc_golden.v ddc.v nco.v cic_decimator_4x_enhanced.v fir_lowpass.v
```

Run: `./run_regression.sh 2>&1 | tail -8`
Expected: `Tests: 28 passed, 0 failed`. The receiver golden is regenerated (new DDC scale); `tb/golden/golden_doppler.mem` changes and is committed. The E2E test's 46 checks still pass (none depend on ADC bit width).

- [ ] **Step 10: Commit**

```bash
git add -A .
git commit -m "fpga: 12-bit CMOS ADC front end, ddc.v at 100 MSPS / 20 MHz IF, delete 400 MHz LVDS path; add DDC golden test (a)

Co-Authored-By: Claude Fable 5.1 <noreply@anthropic.com>"
```

---

### Task 7: Folded, time-multiplexed FIR + golden test (b)

**Files:**
- Rewrite: `fir_lowpass.v` (module `fir_lowpass`)
- Rewrite: `tb/tb_fir_lowpass.v`
- Create: `tb/golden/gen_fir_golden.py`, `tb/golden/tb_fir_golden.v`
- Modify: `ddc.v` (OUT_W 16, instantiate `fir_lowpass`), `radar_receiver_final.v` (drop `ddc_input_interface`), `tb/golden/radar_params.py:DDC_OUT_W`, `tb/golden/tb_ddc_golden.v:OUT_W`, `run_regression.sh`
- Delete: `ddc_input_interface.v`, `tb/tb_ddc_input_interface.v`

**Interfaces:**
- Produces: `module fir_lowpass #(parameter DATA_W = 16, COEFF_W = 18, TAPS = 32) (input clk, reset_n, input signed [DATA_W-1:0] data_in, input data_valid, output reg signed [DATA_W-1:0] data_out, output reg data_out_valid, output wire fir_ready, output reg filter_overflow)`. Contract: at most one `data_valid` per 4 clocks (`fir_ready` = 1 when a sample may be accepted this cycle); `data_out = sat16(Σ_k c[k]·x[n−k] >>> 17)`; latency 7 edges from the accepted sample to `data_out_valid`.
- `ddc` `OUT_W` becomes 16; `radar_receiver_final` wires `adc_i_scaled/adc_q_scaled/adc_valid_sync` straight from `ddc`.

- [ ] **Step 1: Golden generator (direct form vs folded — exact)**

Create `tb/golden/gen_fir_golden.py`:

```python
#!/usr/bin/env python3
"""gen_fir_golden.py — golden test (b): direct-form 32-tap FIR, exact integers.

The folded RTL computes sum_{k<16} c[k]*(x[n-k] + x[n-31+k]); because the
coefficients are symmetric this equals the direct form exactly, so the
expected output is  clip(acc >> 17, -32768, 32767)  with acc from np.convolve.
Writes fir_in.hex (16-bit) and fir_golden.hex (16-bit).
"""
import os
import sys

import numpy as np

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from radar_params import FIR_COEFFS, write_hex  # noqa: E402

N = 2048
HERE = os.path.dirname(os.path.abspath(__file__))


def main():
    rng = np.random.default_rng(7)
    n = np.arange(N)
    tone = 12000 * np.cos(2 * np.pi * 0.03 * n)            # in-band
    tone2 = 9000 * np.cos(2 * np.pi * 0.37 * n)            # stop-band
    noise = rng.normal(0, 3000, N)
    x = np.clip(np.rint(tone + tone2 + noise), -32768, 32767).astype(np.int64)
    x[100:110] = 32767                                      # full-scale burst
    x[300:310] = -32768
    acc = np.convolve(x, np.array(FIR_COEFFS, dtype=np.int64))[:N]
    y = np.clip(acc >> 17, -32768, 32767)
    write_hex(os.path.join(HERE, "fir_in.hex"), x, 16)
    write_hex(os.path.join(HERE, "fir_golden.hex"), y, 16)
    sat = int(np.sum((acc >> 17) != y))
    print(f"wrote {N} samples; {sat} outputs saturated; max|y| = {int(np.max(np.abs(y)))}")


if __name__ == "__main__":
    main()
```

Run: `python3 tb/golden/gen_fir_golden.py` → `wrote 2048 samples; ... max|y| = ...` (saturation count may be 0 or small; both fine).

- [ ] **Step 2: Write the failing golden testbench**

Create `tb/golden/tb_fir_golden.v`:

```verilog
`timescale 1ns / 1ps
// tb_fir_golden.v — golden test (b): folded/time-multiplexed fir_lowpass vs
// direct-form integer model (gen_fir_golden.py).  Exact match, one input per
// 4 clocks (the CIC cadence).
module tb_fir_golden;
    localparam CLK_PERIOD = 10.0;
    localparam N = 2048;
    reg clk, reset_n, data_valid;
    reg  signed [15:0] data_in;
    wire signed [15:0] data_out;
    wire data_out_valid, fir_ready, filter_overflow;
    reg [15:0] in_mem [0:N-1];
    reg [15:0] gold   [0:N-1];
    integer out_count, mismatches, i, pass_count, fail_count, test_num;

    always #(CLK_PERIOD/2) clk = ~clk;

    fir_lowpass #(.DATA_W(16), .COEFF_W(18), .TAPS(32)) uut (
        .clk(clk), .reset_n(reset_n), .data_in(data_in), .data_valid(data_valid),
        .data_out(data_out), .data_out_valid(data_out_valid),
        .fir_ready(fir_ready), .filter_overflow(filter_overflow));

    task check;
        input cond; input [511:0] label;
        begin
            test_num = test_num + 1;
            if (cond) begin $display("[PASS] Test %0d: %0s", test_num, label); pass_count = pass_count + 1; end
            else       begin $display("[FAIL] Test %0d: %0s", test_num, label); fail_count = fail_count + 1; end
        end
    endtask

    always @(posedge clk) if (reset_n && data_out_valid) begin
        if (out_count < N && data_out !== $signed(gold[out_count])) begin
            if (mismatches < 10) $display("  mismatch @%0d: rtl=%0d gold=%0d", out_count, data_out, $signed(gold[out_count]));
            mismatches = mismatches + 1;
        end
        out_count = out_count + 1;
    end

    initial begin
        $readmemh("tb/golden/fir_in.hex", in_mem);
        $readmemh("tb/golden/fir_golden.hex", gold);
        clk = 0; reset_n = 0; data_valid = 0; data_in = 0;
        out_count = 0; mismatches = 0; pass_count = 0; fail_count = 0; test_num = 0;
        repeat (3) @(posedge clk); #1; reset_n = 1; @(posedge clk); #1;
        check(fir_ready === 1'b1, "fir_ready = 1 when idle");
        for (i = 0; i < N; i = i + 1) begin
            data_in = $signed(in_mem[i]); data_valid = 1'b1;
            @(posedge clk); #1;
            data_valid = 1'b0;
            @(posedge clk); #1;
            if (i == 0) check(fir_ready === 1'b0, "fir_ready = 0 while the 4 phases run");
            @(posedge clk); #1;
            @(posedge clk); #1;
        end
        repeat (12) @(posedge clk); #1;
        $display("outputs=%0d mismatches=%0d", out_count, mismatches);
        check(out_count == N, "one output per input");
        check(mismatches == 0, "folded FIR == direct-form FIR, bit-exact");
        $display("\nResults: %0d/%0d passed", pass_count, pass_count + fail_count);
        $finish;
    end
endmodule
```

Run: `iverilog -g2001 -DSIMULATION -o /tmp/tb_firg.vvp tb/golden/tb_fir_golden.v fir_lowpass.v`
Expected: error `Unknown module type: fir_lowpass`.

- [ ] **Step 3: Rewrite `fir_lowpass.v`**

```verilog
`timescale 1ns / 1ps
// ============================================================================
// fir_lowpass.v — 32-tap symmetric low-pass FIR, folded and time-multiplexed
//
// Input rate: 25 MSPS on a 100 MHz clock = one valid sample every 4 clocks.
// Symmetry (c[k] == c[31-k]) folds the 32 taps into 16 pre-added pairs, and
// the 4 idle clocks between samples let 4 multipliers compute the 16 pairs
// in 4 phases.  Total: 4 multipliers (17 x 18) per channel.
//
//   y[n] = sum_{k=0}^{15} c[k] * (x[n-k] + x[n-31+k])      (exact == direct form)
//   data_out = saturate16( acc >>> 17 )                     (coefficients are Q1.17)
//
// Pipeline for a sample accepted at edge e (data_valid = 1):
//   e      : delay line shift; run <= 1, phase <= 0
//   e+1..4 : stage A  pre-adders for pairs 4p..4p+3 (p = phase), coeff regs
//   e+2..5 : stage B  4 products (registered)
//   e+3..6 : stage C  acc <= (p == 0 ? 0 : acc) + p0 + p1 + p2 + p3
//   e+7    : stage D  data_out <= saturate(acc >>> 17), data_out_valid
// Widths: pre-add 17, product 35, accumulator 35 + log2(32) = 40 bits
// (no overflow possible: |acc| <= 2^16 * sum|c| < 2^36).
//
// Contract: data_valid must not be asserted more than once per 4 clocks
// (fir_ready reports when a new sample may be accepted on the next edge).
// The CIC in ddc.v guarantees this cadence.
//
// Resources: 4 multipliers, ~32*16 + 2*4*17 + 4*35 + 40 + ~40 flip-flops, 0 RAM.
// ============================================================================
module fir_lowpass #(
    parameter DATA_W  = 16,
    parameter COEFF_W = 18,
    parameter TAPS    = 32
)(
    input  wire                     clk,
    input  wire                     reset_n,
    input  wire signed [DATA_W-1:0] data_in,
    input  wire                     data_valid,
    output reg  signed [DATA_W-1:0] data_out,
    output reg                      data_out_valid,
    output wire                     fir_ready,
    output reg                      filter_overflow
);

localparam HALF   = TAPS / 2;            // 16 pairs
localparam SUM_W  = DATA_W + 1;          // 17
localparam PROD_W = SUM_W + COEFF_W;     // 35
localparam ACC_W  = PROD_W + 5;          // 40 (+log2(TAPS) guard bits)
localparam SHIFT  = COEFF_W - 1;         // 17

localparam signed [ACC_W-1:0] OUT_MAX =  (1 << (DATA_W - 1)) - 1;
localparam signed [ACC_W-1:0] OUT_MIN = -(1 << (DATA_W - 1));

// Coefficients, first half (c[k] == c[31-k])
reg signed [COEFF_W-1:0] coeff [0:HALF-1];
initial begin
    coeff[ 0] = 18'sh00AD; coeff[ 1] = 18'sh00CE; coeff[ 2] = 18'sh3FD87; coeff[ 3] = 18'sh02A6;
    coeff[ 4] = 18'sh00E0; coeff[ 5] = 18'sh3F8C0; coeff[ 6] = 18'sh0A45; coeff[ 7] = 18'sh3FD82;
    coeff[ 8] = 18'sh3F0B5; coeff[ 9] = 18'sh1CAD; coeff[10] = 18'sh3EE59; coeff[11] = 18'sh3E821;
    coeff[12] = 18'sh4841; coeff[13] = 18'sh3B340; coeff[14] = 18'sh3E299; coeff[15] = 18'sh1FFFF;
end

// Delay line (dline[0] = newest sample)
reg signed [DATA_W-1:0] dline [0:TAPS-1];
reg       run;
reg [1:0] phase;

// Stage A/B/C registers
reg signed [SUM_W-1:0]   pre0, pre1, pre2, pre3;
reg signed [COEFF_W-1:0] c0, c1, c2, c3;
reg                      vA; reg [1:0] phA;
reg signed [PROD_W-1:0]  p0, p1, p2, p3;
reg                      vB; reg [1:0] phB;
reg signed [ACC_W-1:0]   acc;
reg                      vC;

integer i;

wire [4:0] base = {phase, 2'b00};   // 4 * phase

assign fir_ready = (!run) || (phase == 2'd3);

// ---- Delay line + phase scheduler ----
always @(posedge clk) begin
    if (!reset_n) begin
        for (i = 0; i < TAPS; i = i + 1) dline[i] <= {DATA_W{1'b0}};
        run   <= 1'b0;
        phase <= 2'd0;
    end else begin
        if (data_valid) begin
            for (i = TAPS-1; i > 0; i = i - 1) dline[i] <= dline[i-1];
            dline[0] <= data_in;
            run   <= 1'b1;
            phase <= 2'd0;
        end else if (run) begin
            phase <= phase + 2'd1;
            if (phase == 2'd3) run <= 1'b0;
        end
    end
end

// ---- Stage A: pre-adders (folding) ----
always @(posedge clk) begin
    if (!reset_n) begin
        vA <= 1'b0; phA <= 2'd0;
        pre0 <= 0; pre1 <= 0; pre2 <= 0; pre3 <= 0;
        c0 <= 0; c1 <= 0; c2 <= 0; c3 <= 0;
    end else begin
        vA  <= run;
        phA <= phase;
        pre0 <= dline[base + 0] + dline[TAPS - 1 - base - 0];
        pre1 <= dline[base + 1] + dline[TAPS - 1 - base - 1];
        pre2 <= dline[base + 2] + dline[TAPS - 1 - base - 2];
        pre3 <= dline[base + 3] + dline[TAPS - 1 - base - 3];
        c0 <= coeff[base + 0];
        c1 <= coeff[base + 1];
        c2 <= coeff[base + 2];
        c3 <= coeff[base + 3];
    end
end

// ---- Stage B: 4 multipliers ----
always @(posedge clk) begin
    if (!reset_n) begin
        vB <= 1'b0; phB <= 2'd0;
        p0 <= 0; p1 <= 0; p2 <= 0; p3 <= 0;
    end else begin
        vB  <= vA;
        phB <= phA;
        p0  <= pre0 * c0;
        p1  <= pre1 * c1;
        p2  <= pre2 * c2;
        p3  <= pre3 * c3;
    end
end

// ---- Stage C: accumulate the 4 phases ----
always @(posedge clk) begin
    if (!reset_n) begin
        acc <= {ACC_W{1'b0}};
        vC  <= 1'b0;
    end else begin
        vC <= vB & (phB == 2'd3);
        if (vB) begin
            if (phB == 2'd0)
                acc <= p0 + p1 + p2 + p3;
            else
                acc <= acc + p0 + p1 + p2 + p3;
        end
    end
end

// ---- Stage D: scale + saturate ----
wire signed [ACC_W-1:0] acc_sh = acc >>> SHIFT;

always @(posedge clk) begin
    if (!reset_n) begin
        data_out        <= {DATA_W{1'b0}};
        data_out_valid  <= 1'b0;
        filter_overflow <= 1'b0;
    end else begin
        data_out_valid <= vC;
        if (vC) begin
            if (acc_sh > OUT_MAX) begin
                data_out <= OUT_MAX[DATA_W-1:0];
                filter_overflow <= 1'b1;
            end else if (acc_sh < OUT_MIN) begin
                data_out <= OUT_MIN[DATA_W-1:0];
                filter_overflow <= 1'b1;
            end else begin
                data_out <= acc_sh[DATA_W-1:0];
                filter_overflow <= 1'b0;
            end
        end
    end
end

endmodule
```

- [ ] **Step 4: Run golden test (b)**

Run: `iverilog -g2001 -DSIMULATION -o /tmp/tb_firg.vvp tb/golden/tb_fir_golden.v fir_lowpass.v && vvp /tmp/tb_firg.vvp | tail -4`
Expected: `outputs=2048 mismatches=0`, 4 `[PASS`. If outputs are shifted by one sample, the delay-line shift happens on the wrong edge relative to stage A.

- [ ] **Step 5: Replace the functional FIR test**

Rewrite `tb/tb_fir_lowpass.v` (keeps the regression entry "FIR Lowpass"):

```verilog
`timescale 1ns / 1ps
// tb_fir_lowpass.v — functional checks for the folded FIR (DC gain, cadence, saturation)
module tb_fir_lowpass;
    localparam CLK_PERIOD = 10.0;
    reg clk, reset_n, data_valid;
    reg  signed [15:0] data_in;
    wire signed [15:0] data_out;
    wire data_out_valid, fir_ready, filter_overflow;
    integer pass_count, fail_count, test_num, i, out_count;
    reg signed [15:0] last_out;
    integer coef_sum;

    always #(CLK_PERIOD/2) clk = ~clk;

    fir_lowpass uut (.clk(clk), .reset_n(reset_n), .data_in(data_in), .data_valid(data_valid),
        .data_out(data_out), .data_out_valid(data_out_valid), .fir_ready(fir_ready), .filter_overflow(filter_overflow));

    task check;
        input cond; input [511:0] label;
        begin
            test_num = test_num + 1;
            if (cond) begin $display("[PASS] Test %0d: %0s", test_num, label); pass_count = pass_count + 1; end
            else       begin $display("[FAIL] Test %0d: %0s", test_num, label); fail_count = fail_count + 1; end
        end
    endtask

    always @(posedge clk) if (reset_n && data_out_valid) begin out_count = out_count + 1; last_out = data_out; end

    // feed one sample, then 3 idle clocks (25 MSPS cadence)
    task feed; input signed [15:0] v;
        begin data_in = v; data_valid = 1; @(posedge clk); #1; data_valid = 0; repeat (3) @(posedge clk); #1; end
    endtask

    initial begin
        clk = 0; reset_n = 0; data_valid = 0; data_in = 0;
        pass_count = 0; fail_count = 0; test_num = 0; out_count = 0; last_out = 0;
        // sum of the 32 coefficients (18-bit two's complement) = DC gain * 2^17
        coef_sum = 2 * (173 + 206 - 633 + 678 + 224 - 1856 + 2629 - 638 - 3915 + 7341 - 4519 - 6111
                        + 18497 - 19648 - 7527 + 131071);
        repeat (3) @(posedge clk); #1;
        check(data_out_valid === 1'b0, "no output in reset");
        reset_n = 1; @(posedge clk); #1;

        // DC: 64 samples of 10000 -> steady-state = (10000 * coef_sum) >>> 17
        for (i = 0; i < 64; i = i + 1) feed(16'sd10000);
        repeat (10) @(posedge clk); #1;
        $display("DC: last_out=%0d expected=%0d (coef_sum=%0d)", last_out, (10000 * coef_sum) >>> 17, coef_sum);
        check(out_count == 64, "one output per input (64)");
        check(last_out == ((10000 * coef_sum) >>> 17), "DC gain = sum(c) / 2^17 exactly");

        // Saturation: full-scale DC must saturate, flag asserted
        out_count = 0;
        for (i = 0; i < 64; i = i + 1) feed(16'sd32767);
        repeat (10) @(posedge clk); #1;
        check(last_out == 16'sd32767 || (filter_overflow === 1'b0 && last_out == ((32767 * coef_sum) >>> 17)),
              "full-scale DC saturates to 32767 (or fits if gain <= 1)");

        // Cadence: fir_ready pattern 0,0,0,1 after a sample
        data_in = 0; data_valid = 1; @(posedge clk); #1; data_valid = 0;
        check(fir_ready === 1'b0, "fir_ready low on phase 0");
        @(posedge clk); #1; @(posedge clk); #1; @(posedge clk); #1;
        check(fir_ready === 1'b1, "fir_ready high on phase 3 (next sample may be accepted)");

        $display("\nResults: %0d/%0d passed", pass_count, pass_count + fail_count);
        $finish;
    end
endmodule
```

Run: `iverilog -g2001 -DSIMULATION -o /tmp/tb_fir.vvp tb/tb_fir_lowpass.v fir_lowpass.v && vvp /tmp/tb_fir.vvp | grep -E "^\[|DC:"`
Expected: 6 `[PASS`, with `DC: last_out=17695 expected=17695 (coef_sum=231944)` (sum of the 32 coefficients = 231944, DC gain 1.77, so full-scale DC saturates to 32767).

- [ ] **Step 6: Switch `ddc.v` to the new FIR (16-bit) and drop `ddc_input_interface`**

In `ddc.v`: `parameter OUT_W = 16`; replace the two `fir_lowpass_parallel_enhanced` instances with:

```verilog
fir_lowpass #(.DATA_W(OUT_W)) fir_i_inst (
    .clk(clk), .reset_n(reset_n),
    .data_in(cic_i_out), .data_valid(cic_valid_i),
    .data_out(fir_i_out), .data_out_valid(fir_valid_i),
    .fir_ready(), .filter_overflow());

fir_lowpass #(.DATA_W(OUT_W)) fir_q_inst (
    .clk(clk), .reset_n(reset_n),
    .data_in(cic_q_out), .data_valid(cic_valid_q),
    .data_out(fir_q_out), .data_out_valid(fir_valid_q),
    .fir_ready(), .filter_overflow());
```

and update the header lines `-> FIR 32 taps (symmetric)   OUT_W @ 25 MSPS` → `16 bit @ 25 MSPS`, `Resources: 2 multipliers` → `Resources: 2 (mixer) + 2 x 4 (FIR) = 10 multipliers, 0 RAM`.

In `radar_receiver_final.v`: `wire signed [17:0] ddc_out_i/q` → `[15:0]`; `ddc #(.ADC_W(12), .OUT_W(18))` → `ddc #(.ADC_W(12), .OUT_W(16))`; delete the `ddc_input_interface ddc_if (...)` instance and replace it with:

```verilog
// DDC output is already 16-bit at 25 MSPS — no rescaling stage
assign adc_i_scaled   = ddc_out_i;
assign adc_q_scaled   = ddc_out_q;
assign adc_valid_sync = ddc_valid;
```

`git rm -q ddc_input_interface.v tb/tb_ddc_input_interface.v`; remove `ddc_input_interface.v` from `PROD_RTL` and `RECEIVER_RTL` in `run_regression.sh`.

In `tb/golden/radar_params.py` set `DDC_OUT_W = 16` (update the comment); in `tb/golden/tb_ddc_golden.v` set `localparam OUT_W = 16;` and the drain comment `FIR 9` → `FIR 7`. Regenerate: `python3 tb/golden/gen_ddc_golden.py`.

- [ ] **Step 7: Regression entries and run**

In `run_regression.sh` Phase 3 replace the FIR entry with:

```bash
run_test "FIR Lowpass (folded, 4-phase)" \
    tb/tb_fir_reg.vvp \
    tb/tb_fir_lowpass.v fir_lowpass.v

run_test "FIR golden (b): folded == direct form" \
    tb/tb_fir_golden_reg.vvp \
    tb/golden/tb_fir_golden.v fir_lowpass.v
```

Run: `./run_regression.sh 2>&1 | tail -8` → `Tests: 29 passed, 0 failed` (DDC golden (a) passes with the 16-bit vectors; receiver golden regenerated).

- [ ] **Step 8: Commit**

```bash
git add -A .
git commit -m "fpga: folded 4-phase FIR (4 multipliers/channel, 40-bit accumulator); golden test (b); drop ddc_input_interface

Co-Authored-By: Claude Fable 5.1 <noreply@anthropic.com>"
```

---

### Task 8: `fft_engine.v` — 24-bit internal width, inferred RAM, 256-point default + golden test (c)

**Files:**
- Modify: `fft_engine.v:1-37, 105, 224-227, 311-512, 636-650`
- Modify: `xfft_16.v:84-91`, `tb/tb_fft_engine.v:26`
- Modify: `tb/cosim/fpga_model.py:761-864` (add `internal_w` wrap to `FFTEngine`)
- Create: `tb/golden/gen_twiddle_rom.py`, `fft_twiddle_256.mem`, `tb/golden/gen_fft_golden.py`, `tb/golden/tb_fft256_golden.v`
- Modify: `run_regression.sh` (Phase 3)

**Interfaces:**
- Produces: `fft_engine #(N = 256, LOG2N = 8, DATA_W = 16, INTERNAL_W = 24, TWIDDLE_W = 16, TWIDDLE_FILE = "fft_twiddle_256.mem")` — ports unchanged (`clk, reset_n, start, inverse, din_re, din_im, din_valid, dout_re, dout_im, dout_valid, busy, done`). Arithmetic unchanged (4 real multiplies per butterfly, `>>> 15`, saturate on output, `>>> LOG2N` for inverse). Python: `FFTEngine(n, twiddle_file, internal_w=None)`; `internal_w=24` wraps the butterfly sums to 24 bits exactly like the RTL.
- `fft_twiddle_<N>.mem`: N/4 lines, `round(32767*cos(2πk/N))` as 4-digit two's-complement hex, 3 leading `//` comment lines.

- [ ] **Step 1: Twiddle generator and the 256-point ROM**

Create `tb/golden/gen_twiddle_rom.py`:

```python
#!/usr/bin/env python3
"""gen_twiddle_rom.py N — quarter-wave cosine ROM for fft_engine.v.

Writes ../../fft_twiddle_<N>.mem with N/4 entries, round(32767*cos(2*pi*k/N)),
16-bit two's complement, matching the existing fft_twiddle_1024.mem/16.mem.
"""
import math
import os
import sys


def main():
    n = int(sys.argv[1]) if len(sys.argv) > 1 else 256
    assert n & (n - 1) == 0 and n >= 16, "N must be a power of two >= 16"
    path = os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "..", f"fft_twiddle_{n}.mem")
    with open(path, "w") as f:
        f.write(f"// Quarter-wave cosine ROM for {n}-point FFT\n")
        f.write(f"// {n // 4} entries, 16-bit signed Q15 ($readmemh format)\n")
        f.write(f"// cos(2*pi*k/{n}) for k = 0..{n // 4 - 1}\n")
        for k in range(n // 4):
            f.write(f"{round(32767 * math.cos(2 * math.pi * k / n)) & 0xFFFF:04X}\n")
    print(f"wrote {os.path.normpath(path)}")


if __name__ == "__main__":
    main()
```

Run: `python3 tb/golden/gen_twiddle_rom.py 256 && python3 tb/golden/gen_twiddle_rom.py 1024 && git diff --stat fft_twiddle_1024.mem`
Expected: `fft_twiddle_256.mem` created (67 lines); `fft_twiddle_1024.mem` unchanged (the diff is empty — proves the rounding convention matches).

- [ ] **Step 2: Python model: optional internal-width wrap**

In `tb/cosim/fpga_model.py` change `FFTEngine.__init__` and the butterfly store:

```python
    def __init__(self, n=1024, twiddle_file=None, internal_w=None):
        self.N = n
        self.LOG2N = n.bit_length() - 1
        self.internal_w = internal_w          # None = unbounded (legacy), 24 = fft_engine default
        self.cos_rom = load_twiddle_rom(twiddle_file)
        # Working memory (INTERNAL_W-bit signed I/Q pairs)
        self.mem_re = [0] * n
        self.mem_im = [0] * n

    def _wrap(self, v):
        if self.internal_w is None:
            return v
        return sign_extend(v & ((1 << self.internal_w) - 1), self.internal_w)
```

and in `compute()` replace the four stores with:

```python
                self.mem_re[even] = self._wrap(a_re + t_re)
                self.mem_im[even] = self._wrap(a_im + t_im)
                self.mem_re[odd] = self._wrap(a_re - t_re)
                self.mem_im[odd] = self._wrap(a_im - t_im)
```

`load_twiddle_rom()` must keep accepting an explicit path (it does). Run `python3 tb/cosim/fpga_model.py` → no output, exit 0.

- [ ] **Step 3: Golden generator for the 256-point FFT**

Create `tb/golden/gen_fft_golden.py`:

```python
#!/usr/bin/env python3
"""gen_fft_golden.py — golden test (c): 256-point fft_engine vs numpy.

Forward: input = two tones + noise, amplitude chosen so |X| < 2^15 (the engine
saturates its 16-bit output without scaling).  Expected = numpy FFT rounded.
Tolerance (documented in tb_fft256_golden.v): the engine truncates (>>> 15)
after every stage, which accumulates to |err| <= 256 per component and
RMS <= 32 for this vector; the script asserts the bit-accurate model
(fpga_model.FFTEngine, internal_w=24) satisfies the same bound.
Inverse: input = the rounded numpy spectrum, expected = original samples,
tolerance +-4 per component (1/N scaling truncation).
Writes fft256_in_i/q.hex, fft256_fwd_i/q.hex, fft256_inv_i/q.hex (16-bit).
"""
import os
import sys

import numpy as np

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
sys.path.insert(0, os.path.join(HERE, "..", "cosim"))
from fpga_model import FFTEngine  # noqa: E402
from radar_params import write_hex  # noqa: E402

N = 256
MAX_ABS_ERR = 256
MAX_RMS_ERR = 32


def main():
    rng = np.random.default_rng(3)
    n = np.arange(N)
    xi = np.rint(100 * np.cos(2 * np.pi * 17 * n / N) + 60 * np.sin(2 * np.pi * 41.3 * n / N)
                 + rng.normal(0, 40, N)).astype(int)
    xq = np.rint(rng.normal(0, 40, N)).astype(int)
    X = np.fft.fft(xi + 1j * xq)
    assert np.abs(X).max() < 30000, "spectrum must not saturate the 16-bit output"
    Xi, Xq = np.rint(X.real).astype(int), np.rint(X.imag).astype(int)

    model = FFTEngine(n=N, twiddle_file=os.path.join(HERE, "..", "..", "fft_twiddle_256.mem"), internal_w=24)
    mi, mq = model.compute(list(xi), list(xq), inverse=False)
    err = np.hypot(np.array(mi) - Xi, np.array(mq) - Xq)
    print(f"model vs numpy (forward): max {err.max():.1f}  rms {np.sqrt((err**2).mean()):.1f}")
    assert err.max() <= MAX_ABS_ERR and np.sqrt((err ** 2).mean()) <= MAX_RMS_ERR, "raise the documented tolerance"
    ri, rq = model.compute(list(Xi), list(Xq), inverse=True)
    inv_err = max(max(abs(np.array(ri) - xi)), max(abs(np.array(rq) - xq)))
    print(f"model inverse round-trip max err: {inv_err}")
    assert inv_err <= 4

    write_hex(os.path.join(HERE, "fft256_in_i.hex"), xi, 16)
    write_hex(os.path.join(HERE, "fft256_in_q.hex"), xq, 16)
    write_hex(os.path.join(HERE, "fft256_fwd_i.hex"), Xi, 16)
    write_hex(os.path.join(HERE, "fft256_fwd_q.hex"), Xq, 16)
    print("wrote fft256_*.hex")


if __name__ == "__main__":
    main()
```

Run: `python3 tb/golden/gen_fft_golden.py`
Expected: `model vs numpy (forward): max ~187  rms ~21`, `model inverse round-trip max err: 2` (or less), `wrote fft256_*.hex`.

- [ ] **Step 4: Write the failing golden testbench**

Create `tb/golden/tb_fft256_golden.v`:

```verilog
`timescale 1ns / 1ps
// ============================================================================
// tb_fft256_golden.v — golden test (c): 256-point fft_engine vs numpy FFT.
// Tolerance (see gen_fft_golden.py): forward |err| <= 256 per component and
// sum of squared errors <= N * 32^2 (RMS <= 32); inverse +-4 per component.
// ============================================================================
module tb_fft256_golden;
    localparam N = 256, LOG2N = 8, CLK_PERIOD = 10;
    localparam MAX_ABS = 256;
    localparam MAX_SQ_SUM = N * 32 * 32;
    reg clk, reset_n, start, inverse, din_valid;
    reg  signed [15:0] din_re, din_im;
    wire signed [15:0] dout_re, dout_im;
    wire dout_valid, busy, done;
    reg [15:0] in_i [0:N-1], in_q [0:N-1], fwd_i [0:N-1], fwd_q [0:N-1];
    reg signed [15:0] cap_re [0:N-1], cap_im [0:N-1];
    integer cap_count, i, err_re, err_im, max_err, sq_sum, pass_count, fail_count, test_num;

    always #(CLK_PERIOD/2) clk = ~clk;

    fft_engine #(.N(N), .LOG2N(LOG2N), .DATA_W(16), .INTERNAL_W(24), .TWIDDLE_W(16),
                 .TWIDDLE_FILE("fft_twiddle_256.mem")) dut (
        .clk(clk), .reset_n(reset_n), .start(start), .inverse(inverse),
        .din_re(din_re), .din_im(din_im), .din_valid(din_valid),
        .dout_re(dout_re), .dout_im(dout_im), .dout_valid(dout_valid), .busy(busy), .done(done));

    task check;
        input cond; input [511:0] label;
        begin
            test_num = test_num + 1;
            if (cond) begin $display("[PASS] Test %0d: %0s", test_num, label); pass_count = pass_count + 1; end
            else       begin $display("[FAIL] Test %0d: %0s", test_num, label); fail_count = fail_count + 1; end
        end
    endtask

    always @(posedge clk) if (dout_valid && cap_count < N) begin
        cap_re[cap_count] <= dout_re; cap_im[cap_count] <= dout_im; cap_count <= cap_count + 1;
    end

    task run_fft;
        input inv;
        integer k;
        begin
            cap_count = 0;
            @(posedge clk); #1; start = 1; inverse = inv; @(posedge clk); #1; start = 0;
            for (k = 0; k < N; k = k + 1) begin
                din_re = inv ? $signed(fwd_i[k]) : $signed(in_i[k]);
                din_im = inv ? $signed(fwd_q[k]) : $signed(in_q[k]);
                din_valid = 1; @(posedge clk); #1;
            end
            din_valid = 0;
            @(posedge done);            // done is a 1-cycle pulse after the last output
            @(posedge clk); #1;
        end
    endtask

    initial begin
        $readmemh("tb/golden/fft256_in_i.hex", in_i);  $readmemh("tb/golden/fft256_in_q.hex", in_q);
        $readmemh("tb/golden/fft256_fwd_i.hex", fwd_i); $readmemh("tb/golden/fft256_fwd_q.hex", fwd_q);
        clk = 0; reset_n = 0; start = 0; inverse = 0; din_valid = 0; din_re = 0; din_im = 0;
        cap_count = 0; pass_count = 0; fail_count = 0; test_num = 0;
        repeat (3) @(posedge clk); #1; reset_n = 1;

        // ---- forward ----
        run_fft(0);
        max_err = 0; sq_sum = 0;
        for (i = 0; i < N; i = i + 1) begin
            err_re = cap_re[i] - $signed(fwd_i[i]); if (err_re < 0) err_re = -err_re;
            err_im = cap_im[i] - $signed(fwd_q[i]); if (err_im < 0) err_im = -err_im;
            if (err_re > max_err) max_err = err_re;
            if (err_im > max_err) max_err = err_im;
            sq_sum = sq_sum + err_re * err_re + err_im * err_im;
        end
        $display("forward: outputs=%0d max_err=%0d sq_sum=%0d (limits %0d, %0d)", cap_count, max_err, sq_sum, MAX_ABS, MAX_SQ_SUM);
        check(cap_count == N, "forward: 256 outputs");
        check(max_err <= MAX_ABS, "forward: max |err| <= 256 vs numpy");
        check(sq_sum <= MAX_SQ_SUM, "forward: RMS err <= 32 vs numpy");

        // ---- inverse (from the rounded numpy spectrum back to the input) ----
        run_fft(1);
        max_err = 0;
        for (i = 0; i < N; i = i + 1) begin
            err_re = cap_re[i] - $signed(in_i[i]); if (err_re < 0) err_re = -err_re;
            err_im = cap_im[i] - $signed(in_q[i]); if (err_im < 0) err_im = -err_im;
            if (err_re > max_err) max_err = err_re;
            if (err_im > max_err) max_err = err_im;
        end
        $display("inverse: outputs=%0d max_err=%0d", cap_count, max_err);
        check(cap_count == N, "inverse: 256 outputs");
        check(max_err <= 4, "inverse: round trip within +-4");

        $display("\nResults: %0d/%0d passed", pass_count, pass_count + fail_count);
        $finish;
    end
endmodule
```

Run: `iverilog -g2001 -DSIMULATION -o /tmp/tb_fft256.vvp tb/golden/tb_fft256_golden.v fft_engine.v && vvp /tmp/tb_fft256.vvp | tail -8`
Expected: compiles (the engine is parameterizable today) and probably **passes** already with `INTERNAL_W(24)` passed explicitly — that is fine: the RTL change in this task is structural (inferred RAM, defaults). Keep going.

- [ ] **Step 5: Modify `fft_engine.v`**

Replace lines 1–37 (header + parameter list) with:

```verilog
`timescale 1ns / 1ps

/**
 * fft_engine.v
 *
 * Synthesizable parameterized radix-2 DIT FFT/IFFT engine, vendor-neutral.
 * Iterative single-butterfly architecture with quarter-wave twiddle ROM.
 *
 * Architecture:
 *   - LOAD:    Accept N input samples, store bit-reversed in the data RAM
 *   - COMPUTE: LOG2N stages x N/2 butterflies, 4-cycle pipeline:
 *              BF_READ:  Present RAM addresses; register twiddle index
 *              BF_TW:    RAM data valid -> capture; twiddle ROM lookup from
 *                        registered index -> capture cos/sin
 *              BF_MULT2: 4 real multiplies (INTERNAL_W x TWIDDLE_W) -> registered
 *              BF_WRITE: >>> (TWIDDLE_W-1), add/subtract, RAM writeback
 *   - OUTPUT:  Stream N results (1/N scaling for IFFT), saturated to DATA_W
 *
 * Widths: INTERNAL_W = 24 holds DATA_W = 16 plus 8 bits of growth for N = 256
 * (|X[k]| <= N * 2^15 = 2^23).  Larger N or full-scale coherent input can
 * wrap by 1 LSB at the top; the receiver's gain control keeps the input below
 * 0.9 full scale.  Products are INTERNAL_W + TWIDDLE_W = 40 bits.
 *
 * Multipliers: 4 (INTERNAL_W x TWIDDLE_W) — a 24 x 16 product maps to one
 * 27 x 27 DSP or two 18 x 18 multipliers depending on the device.
 *
 * Data memory: two inferable true-dual-port RAMs (re/im), N x INTERNAL_W,
 * synchronous read, one write port per always block.  Twiddle ROM: N/4 x 16
 * from TWIDDLE_FILE via $readmemh (Quartus and Vivado both infer ROM/LUT).
 *
 * Clock domain: single clock (clk), active-low async reset (reset_n) on the
 * FSM only; datapath registers use synchronous reset.
 */

module fft_engine #(
    parameter N            = 256,
    parameter LOG2N        = 8,
    parameter DATA_W       = 16,
    parameter INTERNAL_W   = 24,
    parameter TWIDDLE_W    = 16,
    parameter TWIDDLE_FILE = "fft_twiddle_256.mem"
)(
```

Line 105: `(* rom_style = "block" *) reg signed [TWIDDLE_W-1:0] cos_rom [0:TW_QUARTER-1];` → `reg signed [TWIDDLE_W-1:0] cos_rom [0:TW_QUARTER-1];`.

Lines 224–227: replace the comment with `// Raw products — full precision, registered to break the multiplier -> adder path\n// Width: INTERNAL_W + TWIDDLE_W per multiply, +1 for the sum of two`.

Replace lines 311–512 (from `// DATA MEMORY — True Dual-Port BRAM` through the `` `endif `` that closes `` `ifndef FFT_XPM_BRAM ``) with:

```verilog
// ============================================================================
// DATA MEMORY — two inferable true-dual-port RAMs (re / im)
// ============================================================================
// Port A and port B each live in their own always block with one write and
// one synchronous read, which both Quartus and Vivado map to block RAM.
// Read-during-write on the same address never happens: BF_WRITE writes two
// distinct addresses and no port reads during BF_WRITE.
// ============================================================================
reg [INTERNAL_W-1:0] mem_re [0:N-1];
reg [INTERNAL_W-1:0] mem_im [0:N-1];

reg [INTERNAL_W-1:0] rdata_a_re, rdata_a_im;
reg [INTERNAL_W-1:0] rdata_b_re, rdata_b_im;

// Port A
always @(posedge clk) begin
    if (bram_we_a) begin
        mem_re[bram_addr_a] <= bram_wdata_a_re;
        mem_im[bram_addr_a] <= bram_wdata_a_im;
    end
    rdata_a_re <= mem_re[bram_addr_a];
    rdata_a_im <= mem_im[bram_addr_a];
end

// Port B
always @(posedge clk) begin
    if (bram_we_b) begin
        mem_re[bram_addr_b] <= bram_wdata_b_re;
        mem_im[bram_addr_b] <= bram_wdata_b_im;
    end
    rdata_b_re <= mem_re[bram_addr_b];
    rdata_b_im <= mem_im[bram_addr_b];
end

always @(*) begin
    mem_rdata_a_re = $signed(rdata_a_re);
    mem_rdata_a_im = $signed(rdata_a_im);
    mem_rdata_b_re = $signed(rdata_b_re);
    mem_rdata_b_im = $signed(rdata_b_im);
end

`ifdef SIMULATION
integer init_i;
initial begin
    for (init_i = 0; init_i < N; init_i = init_i + 1) begin
        mem_re[init_i] = 0;
        mem_im[init_i] = 0;
    end
end
`endif
```

(The replaced range includes the old `xpm_douta_*` wires and the `always @(*)` that drove `mem_rdata_*` from them — the block above is now the only `mem_rdata_*` driver.) Replace lines 636–650 (the Block 2 comment) with:

```verilog
// ============================================================================
// MAIN FSM — Block 2: datapath pipeline (sync reset)
// ============================================================================
// Synchronous reset keeps these registers eligible for absorption into the
// multiplier / RAM output registers of either vendor:
//   - rd_b_re/im, rd_tw_cos/sin -> multiplier input registers
//   - bf_prod_re/im             -> multiplier output registers
//   - rd_a_re/im                -> RAM output registers
// They are only meaningful during COMPUTE states and are always rewritten
// before use, so sync reset is functionally equivalent to async reset.
// ============================================================================
```

Replace the `ST_BF_MULT2` branch of Block 2 (was lines 693–705, the `if (!rd_inverse) ... else ...` with eight `*`) so that exactly four products exist and forward/inverse only differ in the add/subtract — the integer results are identical to before:

```verilog
        ST_BF_MULT2: begin
            // Four shared products (INTERNAL_W x TWIDDLE_W); forward and inverse
            // twiddles differ only in the sign of the sin terms.
            if (!rd_inverse) begin
                bf_prod_re <= p_rc + p_is;
                bf_prod_im <= p_ic - p_rs;
            end else begin
                bf_prod_re <= p_rc - p_is;
                bf_prod_im <= p_ic + p_rs;
            end
        end
```

and add, just above the Block 2 `always` (after the Block 2 comment):

```verilog
// Butterfly products — the only multipliers in the engine (4)
wire signed [PROD_W-1:0] p_rc = rd_b_re * rd_tw_cos;
wire signed [PROD_W-1:0] p_is = rd_b_im * rd_tw_sin;
wire signed [PROD_W-1:0] p_ic = rd_b_im * rd_tw_cos;
wire signed [PROD_W-1:0] p_rs = rd_b_re * rd_tw_sin;
```

Check: `grep -nE "xpm|DSP48|BRAM|rom_style|FFT_XPM" fft_engine.v` → no output (reword any remaining comment that mentions BRAM to "RAM"); `grep -c "\*" fft_engine.v` lists exactly four product lines in code (comments aside).

- [ ] **Step 6: Doppler engine at 24 bits, tests, regression**

In `xfft_16.v` line 88: `.INTERNAL_W(32),` → `.INTERNAL_W(24),`. In `tb/tb_fft_engine.v` line 26: `localparam INT_W  = 32;` → `24`. In `run_regression.sh` Phase 3 add after the FFT Engine test:

```bash
run_test "FFT golden (c): 256-pt engine vs numpy" \
    tb/tb_fft256_reg.vvp \
    tb/golden/tb_fft256_golden.v fft_engine.v
```

Run: `./run_regression.sh 2>&1 | tail -8`
Expected: `Tests: 30 passed, 0 failed`. In particular "Doppler Real-Data (exact match)" and "Full-Chain Real-Data (exact match)" must still report PASS — 16-point data never exceeds 20 bits so 24-bit internal width is bit-identical to 32.

- [ ] **Step 7: Commit**

```bash
git add -A .
git commit -m "fpga: fft_engine with inferred dual-port RAM, 24-bit internal width, 256-pt default; golden test (c)

Co-Authored-By: Claude Fable 5.1 <noreply@anthropic.com>"
```

---

### Task 9: Reference-spectrum ROM

**Files:**
- Create: `tb/golden/gen_ref_spectrum.py`, `ref_spectrum_i.mem`, `ref_spectrum_q.mem`, `ref_spectrum_rom.v`, `tb/tb_ref_spectrum_rom.v`
- Modify: `run_regression.sh` (Phase 3)

**Interfaces:**
- Produces: `module ref_spectrum_rom #(parameter N_FFT = 256, LOG2N = 8, N_SEG = 5, DATA_W = 16, I_FILE = "ref_spectrum_i.mem", Q_FILE = "ref_spectrum_q.mem") (input clk, input [LOG2N+2:0] addr, output reg signed [DATA_W-1:0] dout_i, dout_q)`. `addr = {segment[2:0], k[LOG2N-1:0]}`, 1-cycle read latency. Segment s ∈ 0..3 = FFT of long-chirp samples `[224·s, 224·s+256)`; segment 4 = FFT of the 13-sample short chirp zero-padded to 256. Spectra are scaled so the peak magnitude is 0.9·32767 (one scale for the four long segments, one for the short).
- Python: `gen_ref_spectrum.py` also exposes `ref_segment_spectra()` returning the five complex spectra as int arrays (used by `gen_mf_chain_golden.py`).

- [ ] **Step 1: Generator**

Create `tb/golden/gen_ref_spectrum.py`:

```python
#!/usr/bin/env python3
"""gen_ref_spectrum.py — precomputed reference spectra for ref_spectrum_rom.v.

ROM layout (addr = {seg[2:0], k[7:0]}):
  seg 0..3 : FFT_256( long_chirp[224*seg : 224*seg + 256] )   (750-sample 30 us chirp
             at 25 MSPS, zero-padded; segment windows match the overlap-save
             advance of matched_filter_multi_segment.v so a target at delay d
             peaks at bin d in every segment)
  seg 4    : FFT_256( short_chirp[0:13] zero-padded )
Scaling: long segments share one factor (peak = 0.9*32767 over all four),
the short segment has its own factor.  Values are rounded, 16-bit two's
complement, written with numpy's forward FFT (e^{-j...}) convention, which is
the convention of fft_engine.v.
Writes ../../ref_spectrum_i.mem and ../../ref_spectrum_q.mem (1280 lines each).
"""
import os
import sys

import numpy as np

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
from radar_params import (ADVANCE, LONG_CHIRP_SAMPLES, LONG_SEGMENTS, N_FFT,  # noqa: E402
                          SHORT_CHIRP_SAMPLES, T_LONG, T_SHORT, baseband_chirp, write_hex)

N_SEG = LONG_SEGMENTS + 1
Q15_PEAK = 0.9 * 32767


def long_chirp_padded():
    ci, cq = baseband_chirp(LONG_CHIRP_SAMPLES, T_LONG)
    total = ADVANCE * (LONG_SEGMENTS - 1) + N_FFT          # 928
    c = np.zeros(total, dtype=complex)
    c[:LONG_CHIRP_SAMPLES] = ci + 1j * cq
    return c


def short_chirp_padded():
    ci, cq = baseband_chirp(SHORT_CHIRP_SAMPLES, T_SHORT)
    c = np.zeros(N_FFT, dtype=complex)
    c[:SHORT_CHIRP_SAMPLES] = ci + 1j * cq
    return c


def ref_segment_spectra():
    """Return list of N_SEG arrays of complex ints (the ROM contents)."""
    lc = long_chirp_padded()
    long_specs = [np.fft.fft(lc[ADVANCE * s: ADVANCE * s + N_FFT]) for s in range(LONG_SEGMENTS)]
    short_spec = np.fft.fft(short_chirp_padded())
    scale_long = Q15_PEAK / max(np.abs(S).max() for S in long_specs)
    scale_short = Q15_PEAK / np.abs(short_spec).max()
    out = [np.rint(S * scale_long) for S in long_specs] + [np.rint(short_spec * scale_short)]
    return [np.clip(S.real, -32768, 32767).astype(int) + 1j * np.clip(S.imag, -32768, 32767).astype(int)
            for S in out]


def main():
    specs = ref_segment_spectra()
    rom_i = np.concatenate([S.real.astype(int) for S in specs])
    rom_q = np.concatenate([S.imag.astype(int) for S in specs])
    assert len(rom_i) == N_SEG * N_FFT
    root = os.path.join(HERE, "..", "..")
    write_hex(os.path.join(root, "ref_spectrum_i.mem"), rom_i, 16)
    write_hex(os.path.join(root, "ref_spectrum_q.mem"), rom_q, 16)
    for s, S in enumerate(specs):
        print(f"seg {s}: peak |S| = {int(np.abs(S).max())}, nonzero bins = {int(np.sum(np.abs(S) > 0))}")
    print(f"wrote {N_SEG * N_FFT} entries to ref_spectrum_i.mem / ref_spectrum_q.mem")


if __name__ == "__main__":
    main()
```

Run: `python3 tb/golden/gen_ref_spectrum.py`
Expected: five `seg s: peak |S| = ~29490 ...` lines (seg 3 has a lower peak since it holds only 78 chirp samples), `wrote 1280 entries ...`.

- [ ] **Step 2: Write the failing testbench**

Create `tb/tb_ref_spectrum_rom.v`:

```verilog
`timescale 1ns / 1ps
// tb_ref_spectrum_rom.v — every ROM word reads back the .mem contents with 1-cycle latency
module tb_ref_spectrum_rom;
    localparam N_FFT = 256, LOG2N = 8, N_SEG = 5, DEPTH = N_SEG * N_FFT;
    reg clk;
    reg  [LOG2N+2:0] addr;
    wire signed [15:0] dout_i, dout_q;
    reg [15:0] exp_i [0:DEPTH-1];
    reg [15:0] exp_q [0:DEPTH-1];
    integer a, mismatches, nonzero_long, nonzero_short, pass_count, fail_count, test_num;

    always #5 clk = ~clk;

    ref_spectrum_rom #(.N_FFT(N_FFT), .LOG2N(LOG2N), .N_SEG(N_SEG), .DATA_W(16)) uut (
        .clk(clk), .addr(addr), .dout_i(dout_i), .dout_q(dout_q));

    task check;
        input cond; input [511:0] label;
        begin
            test_num = test_num + 1;
            if (cond) begin $display("[PASS] Test %0d: %0s", test_num, label); pass_count = pass_count + 1; end
            else       begin $display("[FAIL] Test %0d: %0s", test_num, label); fail_count = fail_count + 1; end
        end
    endtask

    initial begin
        $readmemh("ref_spectrum_i.mem", exp_i);
        $readmemh("ref_spectrum_q.mem", exp_q);
        clk = 0; addr = 0; mismatches = 0; nonzero_long = 0; nonzero_short = 0;
        pass_count = 0; fail_count = 0; test_num = 0;
        for (a = 0; a < DEPTH; a = a + 1) begin
            addr = a; @(posedge clk); #1;                 // registered read: data valid now
            if (dout_i !== $signed(exp_i[a]) || dout_q !== $signed(exp_q[a])) mismatches = mismatches + 1;
            if (exp_i[a] != 0 || exp_q[a] != 0) begin
                if (a < 4 * N_FFT) nonzero_long = nonzero_long + 1; else nonzero_short = nonzero_short + 1;
            end
        end
        check(mismatches == 0, "all 1280 words match the .mem files (1-cycle latency)");
        check(nonzero_long > 900, "long segments are dense (> 900 non-zero bins of 1024)");
        check(nonzero_short > 200, "short segment is dense (> 200 non-zero bins of 256)");
        $display("\nResults: %0d/%0d passed", pass_count, pass_count + fail_count);
        $finish;
    end
endmodule
```

Run: `iverilog -g2001 -DSIMULATION -o /tmp/tb_rom.vvp tb/tb_ref_spectrum_rom.v ref_spectrum_rom.v` → `Unable to open input file "ref_spectrum_rom.v"`.

- [ ] **Step 3: Write the ROM**

Create `ref_spectrum_rom.v`:

```verilog
`timescale 1ns / 1ps
// ============================================================================
// ref_spectrum_rom.v — precomputed reference-chirp spectra for the matched
// filter.  Replaces the per-segment "reference FFT" pass, the time-domain
// chirp_memory_loader_param ROMs and the latency_buffer.
//
//   addr = {segment[2:0], k[LOG2N-1:0]}
//   segment 0..3 : long chirp, FFT_256 of samples [224*seg, 224*seg + 256)
//   segment 4    : short chirp (13 samples, zero-padded)
//   dout = ROM[addr] one clock after addr (synchronous read -> block RAM/ROM)
//
// Contents are generated by tb/golden/gen_ref_spectrum.py (scaling and
// conventions documented there).  Size: 2 x 1280 x 16 = 40 kbit.
// ============================================================================
module ref_spectrum_rom #(
    parameter N_FFT  = 256,
    parameter LOG2N  = 8,
    parameter N_SEG  = 5,
    parameter DATA_W = 16,
    parameter I_FILE = "ref_spectrum_i.mem",
    parameter Q_FILE = "ref_spectrum_q.mem"
)(
    input  wire                     clk,
    input  wire [LOG2N+2:0]         addr,
    output reg  signed [DATA_W-1:0] dout_i,
    output reg  signed [DATA_W-1:0] dout_q
);

reg [DATA_W-1:0] rom_i [0:N_SEG*N_FFT-1];
reg [DATA_W-1:0] rom_q [0:N_SEG*N_FFT-1];

initial begin
    $readmemh(I_FILE, rom_i);
    $readmemh(Q_FILE, rom_q);
end

always @(posedge clk) begin
    dout_i <= rom_i[addr];
    dout_q <= rom_q[addr];
end

endmodule
```

- [ ] **Step 4: Run, add to regression, commit**

Run: `iverilog -g2001 -DSIMULATION -o /tmp/tb_rom.vvp tb/tb_ref_spectrum_rom.v ref_spectrum_rom.v && vvp /tmp/tb_rom.vvp | grep "^\["` → 3 `[PASS`.

In `run_regression.sh` Phase 3 add:

```bash
run_test "Reference spectrum ROM" \
    tb/tb_ref_rom_reg.vvp \
    tb/tb_ref_spectrum_rom.v ref_spectrum_rom.v
```

Run: `./run_regression.sh 2>&1 | tail -4` → `Tests: 31 passed, 0 failed`.

```bash
git add ref_spectrum_rom.v ref_spectrum_i.mem ref_spectrum_q.mem tb/golden/gen_ref_spectrum.py tb/tb_ref_spectrum_rom.v run_regression.sh
git commit -m "fpga: add ref_spectrum_rom (precomputed 256-pt reference spectra, 5 segments)

Co-Authored-By: Claude Fable 5.1 <noreply@anthropic.com>"
```

---

### Task 10: 256-point matched-filter chain with ROM reference, receiver integration, golden test (d)

**Files:**
- Rewrite: `matched_filter_processing_chain.v`, `matched_filter_multi_segment.v`
- Modify: `radar_receiver_final.v:87-95, 107-109, 269-373`, `tb/tb_radar_receiver_final.v:183, 500-505, 520-535`
- Create: `tb/golden/gen_mf_chain_golden.py`, `tb/tb_mf_chain.v`, `tb/golden/gen_fullchain_golden.py`, `tb/golden/tb_fullchain_golden.v`
- Modify: `run_regression.sh` (helper `run_test_nosim`, file lists, Phase 2/3 entries)
- Delete: `latency_buffer.v`, `chirp_memory_loader_param.v`, `long_chirp_seg{0,1,2,3}_{i,q}.mem`, `short_chirp_{i,q}.mem`, `tb/tb_latency_buffer.v`, `tb/tb_matched_filter_processing_chain.v`, `tb/tb_mf_chain_synth.v`, `tb/tb_mf_cosim.v`, `tb/tb_multiseg_cosim.v`, `tb/cosim/compare.py`, `tb/cosim/compare_mf.py`, `tb/cosim/gen_mf_cosim_golden.py`, `tb/cosim/gen_multiseg_golden.py`, `tb/cosim/gen_chirp_mem.py`, `tb/cosim/validate_mem_files.py`

**Interfaces:**
- Consumes: `fft_engine` (Task 8, defaults N=256/INTERNAL_W=24), `ref_spectrum_rom` (Task 9), `frequency_matched_filter` (unchanged), `fpga_model.FFTEngine(n, twiddle_file, internal_w)` and `FreqMatchedFilter.process_block`.
- Produces: `module matched_filter_processing_chain #(N_FFT = 256, LOG2N = 8) (input clk, reset_n, input [15:0] adc_data_i, adc_data_q, input adc_valid, input [5:0] chirp_counter, input [2:0] ref_segment, output signed [15:0] range_profile_i, range_profile_q, output range_profile_valid, output [3:0] chain_state)`. `chain_state == 0` means idle in both branches. Collection counter is named `fwd_in_count` in both branches (the receiver TB references it).
- `module matched_filter_multi_segment #(N_FFT = 256, LOG2N = 8, OVERLAP = 32, LONG_CHIRP_SAMPLES = 750, SHORT_CHIRP_SAMPLES = 13, LONG_SEGMENTS = 4) (input clk, reset_n, input signed [15:0] ddc_i, ddc_q, input ddc_valid, input use_long_chirp, input [5:0] chirp_counter, input mc_new_chirp, mc_new_elevation, mc_new_azimuth, output signed [15:0] pc_i_w, pc_q_w, output pc_valid_w, output reg [3:0] status)`. State encoding: IDLE 0, COLLECT 1, ZERO_PAD 2, PRIME 3, PROCESSING 4, WAIT_FFT 5, OUTPUT 6, NEXT_SEGMENT 7, OVERLAP_COPY 8.

- [ ] **Step 1: Golden generator for the chain (bit-exact for the synthesizable branch)**

Create `tb/golden/gen_mf_chain_golden.py`:

```python
#!/usr/bin/env python3
"""gen_mf_chain_golden.py — vectors for tb/tb_mf_chain.v.

Signal = long-chirp segment 0 (first 256 baseband samples), amplitude 400
(no forward-FFT saturation), delayed by 0 / 20 / 100 samples (zero-filled).
Expected output = IFFT( FFT(sig) * conj(ROM seg 0) ) computed with the
bit-accurate model (fpga_model.FFTEngine internal_w=24 + FreqMatchedFilter),
which the synthesizable branch must match exactly.  The peak bin equals the
delay; tb_mf_chain.v checks that for the behavioral branch too.
Writes mf_sig_d{D}_i/q.hex (256 x 16-bit) and mf_gold_d{D}_i/q.hex.
"""
import os
import sys

import numpy as np

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
sys.path.insert(0, os.path.join(HERE, "..", "cosim"))
from fpga_model import FFTEngine, FreqMatchedFilter  # noqa: E402
from gen_ref_spectrum import long_chirp_padded, ref_segment_spectra  # noqa: E402
from radar_params import N_FFT, write_hex  # noqa: E402

AMP = 400
DELAYS = (0, 20, 100)


def main():
    rom0 = ref_segment_spectra()[0]
    rom_i = [int(v) for v in rom0.real]
    rom_q = [int(v) for v in rom0.imag]
    chirp = long_chirp_padded()
    fft = FFTEngine(n=N_FFT, twiddle_file=os.path.join(HERE, "..", "..", "fft_twiddle_256.mem"), internal_w=24)
    for d in DELAYS:
        sig = np.zeros(N_FFT, dtype=complex)
        sig[d:] = chirp[:N_FFT - d]
        si = [int(round(AMP * v.real)) for v in sig]
        sq = [int(round(AMP * v.imag)) for v in sig]
        fi, fq = fft.compute(si, sq, inverse=False)
        assert max(max(map(abs, fi)), max(map(abs, fq))) < 32767, "forward FFT saturates; lower AMP"
        pi, pq = FreqMatchedFilter.process_block(fi, fq, rom_i, rom_q)
        oi, oq = fft.compute(pi, pq, inverse=True)
        mag = np.hypot(oi, oq)
        peak = int(np.argmax(mag))
        print(f"delay {d:3d}: peak bin {peak}, peak {mag.max():.0f}, mean {mag.mean():.0f}")
        assert peak == d
        write_hex(os.path.join(HERE, f"mf_sig_d{d}_i.hex"), si, 16)
        write_hex(os.path.join(HERE, f"mf_sig_d{d}_q.hex"), sq, 16)
        write_hex(os.path.join(HERE, f"mf_gold_d{d}_i.hex"), oi, 16)
        write_hex(os.path.join(HERE, f"mf_gold_d{d}_q.hex"), oq, 16)
    print("wrote mf_sig_*/mf_gold_* vectors")


if __name__ == "__main__":
    main()
```

Run: `python3 tb/golden/gen_mf_chain_golden.py`
Expected: `delay 0: peak bin 0, peak ~2389, mean ~96`, `delay 20: peak bin 20 ...`, `delay 100: peak bin 100 ...`.

- [ ] **Step 2: Write the failing chain testbench (two compile modes)**

Create `tb/tb_mf_chain.v`:

```verilog
`timescale 1ns / 1ps
// ============================================================================
// tb_mf_chain.v — matched_filter_processing_chain (256-pt, ROM reference)
//
// Compiled twice by run_regression.sh:
//   with    -DSIMULATION : behavioral branch (float twiddles) — checks peak
//                          bin == delay and peak within 25% of the golden
//   without -DSIMULATION : synthesizable branch (fft_engine + ref ROM +
//                          frequency_matched_filter) — EXACT match to the
//                          bit-accurate golden (gen_mf_chain_golden.py)
// ============================================================================
module tb_mf_chain;
    localparam N = 256, CLK_PERIOD = 10;
    localparam FRAME_TIMEOUT = 60000;   // 2 FFT passes ~ 4.6k cycles each + multiply + output
    reg clk, reset_n, adc_valid;
    reg  [15:0] adc_data_i, adc_data_q;
    reg  [2:0]  ref_segment;
    wire signed [15:0] rp_i, rp_q;
    wire rp_valid;
    wire [3:0] chain_state;
    reg [15:0] sig_i [0:N-1], sig_q [0:N-1], gold_i [0:N-1], gold_q [0:N-1];
    reg signed [15:0] cap_i [0:N-1], cap_q [0:N-1];
    integer cap_count, i, k, pass_count, fail_count, test_num;
    integer mismatches, peak_bin, peak_mag, gold_peak_bin, gold_peak_mag, mag, mean_mag;
`ifdef SIMULATION
    localparam EXACT = 0;
`else
    localparam EXACT = 1;
`endif

    always #(CLK_PERIOD/2) clk = ~clk;

    matched_filter_processing_chain #(.N_FFT(N), .LOG2N(8)) uut (
        .clk(clk), .reset_n(reset_n),
        .adc_data_i(adc_data_i), .adc_data_q(adc_data_q), .adc_valid(adc_valid),
        .chirp_counter(6'd0), .ref_segment(ref_segment),
        .range_profile_i(rp_i), .range_profile_q(rp_q), .range_profile_valid(rp_valid),
        .chain_state(chain_state));

    task check;
        input cond; input [511:0] label;
        begin
            test_num = test_num + 1;
            if (cond) begin $display("[PASS] Test %0d: %0s", test_num, label); pass_count = pass_count + 1; end
            else       begin $display("[FAIL] Test %0d: %0s", test_num, label); fail_count = fail_count + 1; end
        end
    endtask

    always @(posedge clk) if (reset_n && rp_valid && cap_count < N) begin
        cap_i[cap_count] <= rp_i; cap_q[cap_count] <= rp_q; cap_count <= cap_count + 1;
    end

    task load_vectors;
        input integer d;
        begin
            case (d)
            0:   begin $readmemh("tb/golden/mf_sig_d0_i.hex", sig_i);   $readmemh("tb/golden/mf_sig_d0_q.hex", sig_q);
                       $readmemh("tb/golden/mf_gold_d0_i.hex", gold_i); $readmemh("tb/golden/mf_gold_d0_q.hex", gold_q); end
            20:  begin $readmemh("tb/golden/mf_sig_d20_i.hex", sig_i);  $readmemh("tb/golden/mf_sig_d20_q.hex", sig_q);
                       $readmemh("tb/golden/mf_gold_d20_i.hex", gold_i); $readmemh("tb/golden/mf_gold_d20_q.hex", gold_q); end
            default: begin $readmemh("tb/golden/mf_sig_d100_i.hex", sig_i); $readmemh("tb/golden/mf_sig_d100_q.hex", sig_q);
                       $readmemh("tb/golden/mf_gold_d100_i.hex", gold_i); $readmemh("tb/golden/mf_gold_d100_q.hex", gold_q); end
            endcase
        end
    endtask

    task run_frame;
        input integer d;
        integer wait_count;
        begin
            load_vectors(d);
            reset_n = 0; adc_valid = 0; cap_count = 0; ref_segment = 3'd0;
            repeat (4) @(posedge clk); #1; reset_n = 1; @(posedge clk); #1;
            for (k = 0; k < N; k = k + 1) begin
                adc_data_i = sig_i[k]; adc_data_q = sig_q[k]; adc_valid = 1; @(posedge clk); #1;
            end
            adc_valid = 0;
            wait_count = 0;
            while (!(chain_state == 4'd0 && cap_count == N) && wait_count < FRAME_TIMEOUT) begin
                @(posedge clk); wait_count = wait_count + 1;
            end
            #1;
            // analyse
            mismatches = 0; peak_bin = -1; peak_mag = 0; gold_peak_bin = -1; gold_peak_mag = 0; mean_mag = 0;
            for (i = 0; i < N; i = i + 1) begin
                if (cap_i[i] !== $signed(gold_i[i]) || cap_q[i] !== $signed(gold_q[i])) mismatches = mismatches + 1;
                mag = (cap_i[i] < 0 ? -cap_i[i] : cap_i[i]) + (cap_q[i] < 0 ? -cap_q[i] : cap_q[i]);
                mean_mag = mean_mag + mag;
                if (mag > peak_mag) begin peak_mag = mag; peak_bin = i; end
                mag = ($signed(gold_i[i]) < 0 ? -$signed(gold_i[i]) : $signed(gold_i[i]))
                    + ($signed(gold_q[i]) < 0 ? -$signed(gold_q[i]) : $signed(gold_q[i]));
                if (mag > gold_peak_mag) begin gold_peak_mag = mag; gold_peak_bin = i; end
            end
            mean_mag = mean_mag / N;
            $display("delay %0d: outputs=%0d peak_bin=%0d (gold %0d) peak=%0d (gold %0d) mean=%0d mismatches=%0d cycles=%0d",
                     d, cap_count, peak_bin, gold_peak_bin, peak_mag, gold_peak_mag, mean_mag, mismatches, wait_count);
            check(cap_count == N, "256 range-profile outputs");
            check(chain_state == 4'd0, "chain returns to IDLE");
            check(peak_bin == d, "peak bin == target delay");
            check(peak_mag > 8 * mean_mag, "peak at least 8x the mean magnitude");
            if (EXACT) check(mismatches == 0, "synth branch: bit-exact vs golden");
            else       check(peak_mag * 4 > gold_peak_mag * 3 && peak_mag * 4 < gold_peak_mag * 5,
                             "behavioral branch: peak within 25% of golden");
        end
    endtask

    initial begin
        clk = 0; reset_n = 0; adc_valid = 0; adc_data_i = 0; adc_data_q = 0; ref_segment = 0;
        pass_count = 0; fail_count = 0; test_num = 0; cap_count = 0;
        $display("tb_mf_chain: %s branch", EXACT ? "SYNTHESIZABLE (exact)" : "BEHAVIORAL (tolerant)");
        run_frame(0);
        run_frame(20);
        run_frame(100);
        // back-to-back frames without reset: second frame right after the first
        load_vectors(20);
        reset_n = 0; repeat (2) @(posedge clk); #1; reset_n = 1; @(posedge clk); #1;
        cap_count = 0;
        for (k = 0; k < N; k = k + 1) begin adc_data_i = sig_i[k]; adc_data_q = sig_q[k]; adc_valid = 1; @(posedge clk); #1; end
        adc_valid = 0;
        wait (chain_state == 4'd0 && cap_count == N); #1;
        cap_count = 0;
        for (k = 0; k < N; k = k + 1) begin adc_data_i = sig_i[k]; adc_data_q = sig_q[k]; adc_valid = 1; @(posedge clk); #1; end
        adc_valid = 0;
        wait (chain_state == 4'd0 && cap_count == N); #1;
        check(cap_count == N, "back-to-back frames: second frame produces 256 outputs");
        $display("\nResults: %0d/%0d passed", pass_count, pass_count + fail_count);
        $finish;
    end
endmodule
```

Run (both modes):
`iverilog -g2001 -DSIMULATION -o /tmp/mf_beh.vvp tb/tb_mf_chain.v matched_filter_processing_chain.v fft_engine.v ref_spectrum_rom.v frequency_matched_filter.v`
Expected: compile error (`ref_segment` port does not exist yet).

- [ ] **Step 3: Rewrite `matched_filter_processing_chain.v`**

```verilog
`timescale 1ns / 1ps

/**
 * matched_filter_processing_chain.v
 *
 * Pulse compression for one N_FFT-point segment:
 *   FFT(signal) -> x conj(REF[ref_segment]) -> IFFT -> range profile
 *
 * The reference spectrum comes precomputed from ref_spectrum_rom.v
 * (generated by tb/golden/gen_ref_spectrum.py), so the engine runs only two
 * passes per segment (forward + inverse).
 *
 * Two implementations, selected at compile time:
 *   `ifdef SIMULATION : behavioral in-place FFT with float twiddles (fast for
 *                       the integration testbenches; NOT bit-exact)
 *   else              : synthesizable — one fft_engine (N_FFT, 24-bit internal,
 *                       4 multipliers), frequency_matched_filter (4 multipliers),
 *                       two inferable RAM buffers (sig, prod) 2 x 256 x 16 each
 *                       and the reference ROM.
 * tb/tb_mf_chain.v is compiled both ways by run_regression.sh.
 *
 * Interface:
 *   adc_data_i/q, adc_valid   N_FFT samples per frame (16-bit, two's complement)
 *   ref_segment               0..3 long-chirp segment, 4 short chirp
 *   range_profile_i/q/valid   N_FFT outputs streamed one per clock
 *   chain_state               0 = IDLE (segmenter waits for this)
 *
 * Multipliers: synthesizable branch 4 (engine) + 4 (conj-multiply) = 8.
 * RAM: engine 2 x 256 x 24, sig/prod 4 x 256 x 16, ROM 2 x 1280 x 16.
 */

module matched_filter_processing_chain #(
    parameter N_FFT = 256,
    parameter LOG2N = 8
)(
    input wire clk,
    input wire reset_n,

    input wire [15:0] adc_data_i,
    input wire [15:0] adc_data_q,
    input wire adc_valid,

    input wire [5:0] chirp_counter,
    input wire [2:0] ref_segment,

    output wire signed [15:0] range_profile_i,
    output wire signed [15:0] range_profile_q,
    output wire range_profile_valid,

    output wire [3:0] chain_state
);

`ifdef SIMULATION
// ============================================================================
// BEHAVIORAL IMPLEMENTATION (simulation only, float twiddles)
// ============================================================================
localparam [3:0] ST_IDLE          = 4'd0;
localparam [3:0] ST_FWD_FFT       = 4'd1;   // collect samples + bit-reverse
localparam [3:0] ST_FWD_BUTTERFLY = 4'd2;
localparam [3:0] ST_MULTIPLY      = 4'd3;
localparam [3:0] ST_INV_BITREV    = 4'd4;
localparam [3:0] ST_INV_BUTTERFLY = 4'd5;
localparam [3:0] ST_OUTPUT        = 4'd6;
localparam [3:0] ST_DONE          = 4'd7;

reg [3:0] state;
reg [LOG2N:0] fwd_in_count;
reg fwd_frame_done;

reg signed [15:0] fwd_buf_i  [0:N_FFT-1];
reg signed [15:0] fwd_buf_q  [0:N_FFT-1];
reg signed [15:0] fwd_out_i  [0:N_FFT-1];
reg signed [15:0] fwd_out_q  [0:N_FFT-1];
reg signed [15:0] mult_out_i [0:N_FFT-1];
reg signed [15:0] mult_out_q [0:N_FFT-1];
reg signed [15:0] ifft_out_i [0:N_FFT-1];
reg signed [15:0] ifft_out_q [0:N_FFT-1];
reg signed [31:0] work_re    [0:N_FFT-1];
reg signed [31:0] work_im    [0:N_FFT-1];

// Reference spectra (same .mem files as ref_spectrum_rom.v)
reg [15:0] ref_rom_i [0:5*N_FFT-1];
reg [15:0] ref_rom_q [0:5*N_FFT-1];
initial begin
    $readmemh("ref_spectrum_i.mem", ref_rom_i);
    $readmemh("ref_spectrum_q.mem", ref_rom_q);
end

reg [LOG2N:0] out_count;
reg out_valid_reg;
reg signed [15:0] out_i_reg, out_q_reg;

function [LOG2N-1:0] bit_reverse;
    input [LOG2N-1:0] val;
    integer b;
    begin
        bit_reverse = 0;
        for (b = 0; b < LOG2N; b = b + 1)
            bit_reverse[LOG2N-1-b] = val[b];
    end
endfunction

integer fft_stage, fft_k, fft_j, fft_half, fft_span;
integer fft_idx_even, fft_idx_odd;
reg signed [31:0] tw_re, tw_im, t_re, t_im, u_re, u_im;
real tw_angle;
integer i;

task butterflies;
    input integer sign;   // -1 forward, +1 inverse
    begin
        for (fft_stage = 0; fft_stage < LOG2N; fft_stage = fft_stage + 1) begin
            fft_half = 1 << fft_stage;
            fft_span = fft_half << 1;
            for (fft_k = 0; fft_k < N_FFT; fft_k = fft_k + fft_span) begin
                for (fft_j = 0; fft_j < fft_half; fft_j = fft_j + 1) begin
                    fft_idx_even = fft_k + fft_j;
                    fft_idx_odd  = fft_idx_even + fft_half;
                    tw_angle = sign * 2.0 * 3.14159265358979 * fft_j / (fft_span * 1.0);
                    tw_re = $rtoi($cos(tw_angle) * 32767.0);
                    tw_im = $rtoi($sin(tw_angle) * 32767.0);
                    t_re = (work_re[fft_idx_odd] * tw_re - work_im[fft_idx_odd] * tw_im) >>> 15;
                    t_im = (work_re[fft_idx_odd] * tw_im + work_im[fft_idx_odd] * tw_re) >>> 15;
                    u_re = work_re[fft_idx_even];
                    u_im = work_im[fft_idx_even];
                    work_re[fft_idx_even] = u_re + t_re;
                    work_im[fft_idx_even] = u_im + t_im;
                    work_re[fft_idx_odd]  = u_re - t_re;
                    work_im[fft_idx_odd]  = u_im - t_im;
                end
            end
        end
    end
endtask

function signed [15:0] sat16;
    input signed [31:0] v;
    begin
        if (v > 32767)       sat16 = 16'sh7FFF;
        else if (v < -32768) sat16 = 16'sh8000;
        else                 sat16 = v[15:0];
    end
endfunction

always @(posedge clk or negedge reset_n) begin
    if (!reset_n) begin
        state <= ST_IDLE; fwd_in_count <= 0; fwd_frame_done <= 0;
        out_count <= 0; out_valid_reg <= 0; out_i_reg <= 0; out_q_reg <= 0;
    end else begin
        out_valid_reg <= 1'b0;
        case (state)
        ST_IDLE: begin
            fwd_in_count <= 0; fwd_frame_done <= 0; out_count <= 0;
            if (adc_valid) begin
                fwd_buf_i[0] <= $signed(adc_data_i);
                fwd_buf_q[0] <= $signed(adc_data_q);
                fwd_in_count <= 1;
                state <= ST_FWD_FFT;
            end
        end
        ST_FWD_FFT: begin
            if (!fwd_frame_done) begin
                if (adc_valid && fwd_in_count < N_FFT) begin
                    fwd_buf_i[fwd_in_count] <= $signed(adc_data_i);
                    fwd_buf_q[fwd_in_count] <= $signed(adc_data_q);
                    fwd_in_count <= fwd_in_count + 1;
                end
                if (fwd_in_count == N_FFT) begin
                    fwd_frame_done <= 1;
                    for (i = 0; i < N_FFT; i = i + 1) begin
                        work_re[bit_reverse(i[LOG2N-1:0])] <= {{16{fwd_buf_i[i][15]}}, fwd_buf_i[i]};
                        work_im[bit_reverse(i[LOG2N-1:0])] <= {{16{fwd_buf_q[i][15]}}, fwd_buf_q[i]};
                    end
                end
            end else begin
                state <= ST_FWD_BUTTERFLY;
            end
        end
        ST_FWD_BUTTERFLY: begin
            butterflies(-1);
            for (i = 0; i < N_FFT; i = i + 1) begin
                fwd_out_i[i] <= sat16(work_re[i]);
                fwd_out_q[i] <= sat16(work_im[i]);
            end
            state <= ST_MULTIPLY;
        end
        ST_MULTIPLY: begin
            // (a+jb)(c-jd) = (ac+bd) + j(bc-ad), Q15 x Q15 -> Q15 (same as frequency_matched_filter)
            for (i = 0; i < N_FFT; i = i + 1) begin : mult_loop
                reg signed [31:0] a, b, c, d, re_sum, im_sum;
                a = {{16{fwd_out_i[i][15]}}, fwd_out_i[i]};
                b = {{16{fwd_out_q[i][15]}}, fwd_out_q[i]};
                c = {{16{ref_rom_i[{ref_segment, i[LOG2N-1:0]}][15]}}, ref_rom_i[{ref_segment, i[LOG2N-1:0]}]};
                d = {{16{ref_rom_q[{ref_segment, i[LOG2N-1:0]}][15]}}, ref_rom_q[{ref_segment, i[LOG2N-1:0]}]};
                re_sum = (a * c + b * d + 32'sh4000) >>> 15;
                im_sum = (b * c - a * d + 32'sh4000) >>> 15;
                mult_out_i[i] <= sat16(re_sum);
                mult_out_q[i] <= sat16(im_sum);
            end
            state <= ST_INV_BITREV;
        end
        ST_INV_BITREV: begin
            for (i = 0; i < N_FFT; i = i + 1) begin
                work_re[bit_reverse(i[LOG2N-1:0])] <= {{16{mult_out_i[i][15]}}, mult_out_i[i]};
                work_im[bit_reverse(i[LOG2N-1:0])] <= {{16{mult_out_q[i][15]}}, mult_out_q[i]};
            end
            state <= ST_INV_BUTTERFLY;
        end
        ST_INV_BUTTERFLY: begin
            butterflies(1);
            for (i = 0; i < N_FFT; i = i + 1) begin
                ifft_out_i[i] <= sat16(work_re[i] >>> LOG2N);
                ifft_out_q[i] <= sat16(work_im[i] >>> LOG2N);
            end
            state <= ST_OUTPUT;
        end
        ST_OUTPUT: begin
            if (out_count < N_FFT) begin
                out_i_reg <= ifft_out_i[out_count];
                out_q_reg <= ifft_out_q[out_count];
                out_valid_reg <= 1'b1;
                out_count <= out_count + 1;
            end else begin
                state <= ST_DONE;
            end
        end
        ST_DONE: state <= ST_IDLE;
        default: state <= ST_IDLE;
        endcase
    end
end

assign range_profile_i     = out_i_reg;
assign range_profile_q     = out_q_reg;
assign range_profile_valid = out_valid_reg;
assign chain_state         = state;

integer init_idx;
initial begin
    for (init_idx = 0; init_idx < N_FFT; init_idx = init_idx + 1) begin
        fwd_buf_i[init_idx] = 0; fwd_buf_q[init_idx] = 0; fwd_out_i[init_idx] = 0; fwd_out_q[init_idx] = 0;
        mult_out_i[init_idx] = 0; mult_out_q[init_idx] = 0; ifft_out_i[init_idx] = 0; ifft_out_q[init_idx] = 0;
        work_re[init_idx] = 0; work_im[init_idx] = 0;
    end
end

`else
// ============================================================================
// SYNTHESIZABLE IMPLEMENTATION — fft_engine (FFT + IFFT) + ref ROM
// ============================================================================
localparam [3:0] ST_IDLE     = 4'd0,
                 ST_COLLECT  = 4'd1,   // collect N_FFT samples into sig_buf
                 ST_SIG_FFT  = 4'd2,   // feed sig_buf -> engine, capture FFT into sig_buf
                 ST_SIG_CAP  = 4'd3,
                 ST_MULTIPLY = 4'd4,   // sig_buf x conj(ROM) -> prod_buf
                 ST_INV_FFT  = 4'd5,   // feed prod_buf -> engine (inverse), capture into prod_buf
                 ST_INV_CAP  = 4'd6,
                 ST_OUTPUT   = 4'd7,   // stream prod_buf
                 ST_DONE     = 4'd8;

reg [3:0] state;

// Buffers (inferable RAM: one write + one sync read per always block)
reg signed [15:0] sig_buf_i  [0:N_FFT-1];
reg signed [15:0] sig_buf_q  [0:N_FFT-1];
reg signed [15:0] prod_buf_i [0:N_FFT-1];
reg signed [15:0] prod_buf_q [0:N_FFT-1];
reg signed [15:0] sig_rdata_i, sig_rdata_q;
reg signed [15:0] prod_rdata_i, prod_rdata_q;

reg [LOG2N:0] fwd_in_count;   // collection counter (0..N_FFT)
reg [LOG2N:0] feed_count;
reg [LOG2N:0] cap_count;
reg [LOG2N:0] mult_count;
reg [LOG2N:0] out_count;
reg feed_primed, mult_primed, out_primed;

// FFT engine
reg fft_start, fft_inverse, fft_din_valid;
reg signed [15:0] fft_din_re, fft_din_im;
wire signed [15:0] fft_dout_re, fft_dout_im;
wire fft_dout_valid, fft_busy, fft_done;

fft_engine #(
    .N(N_FFT), .LOG2N(LOG2N), .DATA_W(16), .INTERNAL_W(24), .TWIDDLE_W(16),
    .TWIDDLE_FILE("fft_twiddle_256.mem")
) fft_inst (
    .clk(clk), .reset_n(reset_n), .start(fft_start), .inverse(fft_inverse),
    .din_re(fft_din_re), .din_im(fft_din_im), .din_valid(fft_din_valid),
    .dout_re(fft_dout_re), .dout_im(fft_dout_im), .dout_valid(fft_dout_valid),
    .busy(fft_busy), .done(fft_done));

// Reference ROM — addressed in lock-step with sig_buf during MULTIPLY
wire [LOG2N-1:0] mult_addr = (mult_count < N_FFT) ? mult_count[LOG2N-1:0] : {LOG2N{1'b0}};
wire signed [15:0] rom_i, rom_q;

ref_spectrum_rom #(.N_FFT(N_FFT), .LOG2N(LOG2N)) rom_inst (
    .clk(clk), .addr({ref_segment, mult_addr}), .dout_i(rom_i), .dout_q(rom_q));

// Conjugate multiply
reg signed [15:0] mf_sig_re, mf_sig_im, mf_ref_re, mf_ref_im;
reg mf_valid_in;
wire signed [15:0] mf_out_re, mf_out_im;
wire mf_valid_out;

frequency_matched_filter mf_inst (
    .clk(clk), .reset_n(reset_n),
    .fft_real_in(mf_sig_re), .fft_imag_in(mf_sig_im), .fft_valid_in(mf_valid_in),
    .ref_chirp_real(mf_ref_re), .ref_chirp_imag(mf_ref_im),
    .filtered_real(mf_out_re), .filtered_imag(mf_out_im), .filtered_valid(mf_valid_out),
    .state());

reg out_valid_reg;
reg signed [15:0] out_i_reg, out_q_reg;

// ---- sig_buf port ----
always @(posedge clk) begin : sig_bram_port
    reg we; reg [LOG2N-1:0] addr; reg signed [15:0] wdata_i, wdata_q;
    we = 1'b0; addr = 0; wdata_i = 0; wdata_q = 0;
    case (state)
    ST_IDLE:    if (adc_valid) begin we = 1'b1; addr = 0; wdata_i = $signed(adc_data_i); wdata_q = $signed(adc_data_q); end
    ST_COLLECT: if (adc_valid && fwd_in_count < N_FFT) begin
                    we = 1'b1; addr = fwd_in_count[LOG2N-1:0]; wdata_i = $signed(adc_data_i); wdata_q = $signed(adc_data_q);
                end
    ST_SIG_FFT: begin
        if (feed_count < N_FFT) addr = feed_count[LOG2N-1:0];
        if (fft_dout_valid && cap_count < N_FFT) begin
            we = 1'b1; addr = cap_count[LOG2N-1:0]; wdata_i = fft_dout_re; wdata_q = fft_dout_im;
        end
    end
    ST_SIG_CAP: if (fft_dout_valid && cap_count < N_FFT) begin
                    we = 1'b1; addr = cap_count[LOG2N-1:0]; wdata_i = fft_dout_re; wdata_q = fft_dout_im;
                end
    ST_MULTIPLY: addr = mult_addr;
    default: ;
    endcase
    if (we) begin
        sig_buf_i[addr] <= wdata_i;
        sig_buf_q[addr] <= wdata_q;
    end
    sig_rdata_i <= sig_buf_i[addr];
    sig_rdata_q <= sig_buf_q[addr];
end

// ---- prod_buf port ----
always @(posedge clk) begin : prod_bram_port
    reg we; reg [LOG2N-1:0] addr; reg signed [15:0] wdata_i, wdata_q;
    we = 1'b0; addr = 0; wdata_i = 0; wdata_q = 0;
    case (state)
    ST_MULTIPLY: if (mf_valid_out && cap_count < N_FFT) begin
                     we = 1'b1; addr = cap_count[LOG2N-1:0]; wdata_i = mf_out_re; wdata_q = mf_out_im;
                 end
    ST_INV_FFT: begin
        if (feed_count < N_FFT) addr = feed_count[LOG2N-1:0];
        if (fft_dout_valid && cap_count < N_FFT) begin
            we = 1'b1; addr = cap_count[LOG2N-1:0]; wdata_i = fft_dout_re; wdata_q = fft_dout_im;
        end
    end
    ST_INV_CAP: if (fft_dout_valid && cap_count < N_FFT) begin
                    we = 1'b1; addr = cap_count[LOG2N-1:0]; wdata_i = fft_dout_re; wdata_q = fft_dout_im;
                end
    ST_OUTPUT:  if (out_count < N_FFT) addr = out_count[LOG2N-1:0];
    default: ;
    endcase
    if (we) begin
        prod_buf_i[addr] <= wdata_i;
        prod_buf_q[addr] <= wdata_q;
    end
    prod_rdata_i <= prod_buf_i[addr];
    prod_rdata_q <= prod_buf_q[addr];
end

// ---- main FSM ----
always @(posedge clk or negedge reset_n) begin
    if (!reset_n) begin
        state <= ST_IDLE; fwd_in_count <= 0; feed_count <= 0; cap_count <= 0; mult_count <= 0; out_count <= 0;
        feed_primed <= 0; mult_primed <= 0; out_primed <= 0;
        fft_start <= 0; fft_inverse <= 0; fft_din_re <= 0; fft_din_im <= 0; fft_din_valid <= 0;
        mf_sig_re <= 0; mf_sig_im <= 0; mf_ref_re <= 0; mf_ref_im <= 0; mf_valid_in <= 0;
        out_valid_reg <= 0; out_i_reg <= 0; out_q_reg <= 0;
    end else begin
        fft_start <= 1'b0; fft_din_valid <= 1'b0; mf_valid_in <= 1'b0; out_valid_reg <= 1'b0;
        case (state)
        ST_IDLE: begin
            fwd_in_count <= 0; feed_primed <= 0; mult_primed <= 0; out_primed <= 0;
            if (adc_valid) begin fwd_in_count <= 1; state <= ST_COLLECT; end
        end
        ST_COLLECT: begin
            if (adc_valid && fwd_in_count < N_FFT) fwd_in_count <= fwd_in_count + 1;
            if (fwd_in_count == N_FFT) begin
                state <= ST_SIG_FFT; fft_start <= 1'b1; fft_inverse <= 1'b0;
                feed_count <= 0; cap_count <= 0; feed_primed <= 0;
            end
        end
        ST_SIG_FFT: begin
            if (feed_count < N_FFT) begin
                if (!feed_primed) begin feed_primed <= 1'b1; feed_count <= feed_count + 1; end
                else begin fft_din_re <= sig_rdata_i; fft_din_im <= sig_rdata_q; fft_din_valid <= 1'b1; feed_count <= feed_count + 1; end
            end else if (feed_count == N_FFT && feed_primed) begin
                fft_din_re <= sig_rdata_i; fft_din_im <= sig_rdata_q; fft_din_valid <= 1'b1; feed_count <= feed_count + 1;
            end
            if (fft_dout_valid && cap_count < N_FFT) cap_count <= cap_count + 1;
            if (fft_done) state <= ST_SIG_CAP;
        end
        ST_SIG_CAP: begin
            if (fft_dout_valid && cap_count < N_FFT) cap_count <= cap_count + 1;
            state <= ST_MULTIPLY; mult_count <= 0; cap_count <= 0; mult_primed <= 0;
        end
        ST_MULTIPLY: begin
            if (mult_count < N_FFT) begin
                if (!mult_primed) begin mult_primed <= 1'b1; mult_count <= mult_count + 1; end
                else begin
                    mf_sig_re <= sig_rdata_i; mf_sig_im <= sig_rdata_q; mf_ref_re <= rom_i; mf_ref_im <= rom_q;
                    mf_valid_in <= 1'b1; mult_count <= mult_count + 1;
                end
            end else if (mult_count == N_FFT && mult_primed) begin
                mf_sig_re <= sig_rdata_i; mf_sig_im <= sig_rdata_q; mf_ref_re <= rom_i; mf_ref_im <= rom_q;
                mf_valid_in <= 1'b1; mult_count <= mult_count + 1;
            end
            if (mf_valid_out && cap_count < N_FFT) cap_count <= cap_count + 1;
            if (cap_count == N_FFT) begin
                state <= ST_INV_FFT; fft_start <= 1'b1; fft_inverse <= 1'b1;
                feed_count <= 0; cap_count <= 0; feed_primed <= 0;
            end
        end
        ST_INV_FFT: begin
            if (feed_count < N_FFT) begin
                if (!feed_primed) begin feed_primed <= 1'b1; feed_count <= feed_count + 1; end
                else begin fft_din_re <= prod_rdata_i; fft_din_im <= prod_rdata_q; fft_din_valid <= 1'b1; feed_count <= feed_count + 1; end
            end else if (feed_count == N_FFT && feed_primed) begin
                fft_din_re <= prod_rdata_i; fft_din_im <= prod_rdata_q; fft_din_valid <= 1'b1; feed_count <= feed_count + 1;
            end
            if (fft_dout_valid && cap_count < N_FFT) cap_count <= cap_count + 1;
            if (fft_done) state <= ST_INV_CAP;
        end
        ST_INV_CAP: begin
            if (fft_dout_valid && cap_count < N_FFT) cap_count <= cap_count + 1;
            state <= ST_OUTPUT; out_count <= 0; out_primed <= 0;
        end
        ST_OUTPUT: begin
            if (out_count < N_FFT) begin
                if (!out_primed) begin out_primed <= 1'b1; out_count <= out_count + 1; end
                else begin out_i_reg <= prod_rdata_i; out_q_reg <= prod_rdata_q; out_valid_reg <= 1'b1; out_count <= out_count + 1; end
            end else if (out_count == N_FFT && out_primed) begin
                out_i_reg <= prod_rdata_i; out_q_reg <= prod_rdata_q; out_valid_reg <= 1'b1; out_count <= out_count + 1;
            end else begin
                state <= ST_DONE;
            end
        end
        ST_DONE: state <= ST_IDLE;
        default: state <= ST_IDLE;
        endcase
    end
end

assign range_profile_i     = out_i_reg;
assign range_profile_q     = out_q_reg;
assign range_profile_valid = out_valid_reg;
assign chain_state         = state;

`endif

endmodule
```

Note on the ROM read in MULTIPLY: `sig_rdata_*` and `rom_*` are both registered reads of the same `mult_addr`, so the pair presented to `frequency_matched_filter` is always bin-aligned. The behavioral branch rounds the Q30 product (`+ 0x4000`) exactly like `frequency_matched_filter.v:102` so the two branches differ only by twiddle quantization.

- [ ] **Step 4: Run the chain test in both modes**

```bash
iverilog -g2001 -DSIMULATION -o /tmp/mf_beh.vvp tb/tb_mf_chain.v matched_filter_processing_chain.v fft_engine.v ref_spectrum_rom.v frequency_matched_filter.v && vvp /tmp/mf_beh.vvp | grep -E "^\[|delay|branch"
iverilog -g2001 -o /tmp/mf_syn.vvp tb/tb_mf_chain.v matched_filter_processing_chain.v fft_engine.v ref_spectrum_rom.v frequency_matched_filter.v && vvp /tmp/mf_syn.vvp | grep -E "^\[|delay|branch"
```
Expected: behavioral: 16 `[PASS` (peak bins 0/20/100, within 25%); synth: 16 `[PASS` with `mismatches=0` on all three frames and `cycles` ≈ 9 500 per frame. If the synth branch mismatches everywhere by a constant offset, `ref_segment` addressing or the ROM/sig read alignment (both must be presented with the same `mult_addr`) is off; if only a few bins differ, the 24-bit wrap differs from the model (`internal_w=24` must be passed in the generator).

- [ ] **Step 5: Rewrite `matched_filter_multi_segment.v`**

```verilog
`timescale 1ns / 1ps
// ============================================================================
// matched_filter_multi_segment.v — overlap-save segmenter for the pulse
// compression chain (256-point segments, reference spectra in ROM)
//
// Sample rates / sizes (25 MSPS baseband):
//   long chirp  30 us  -> LONG_CHIRP_SAMPLES  = 750
//   short chirp 0.5 us -> SHORT_CHIRP_SAMPLES = 13
//   N_FFT = 256, OVERLAP = 32, ADVANCE = 224
//   samples covered after k segments = 256 + 224*(k-1):
//     k = 3 -> 704 < 750, k = 4 -> 928 >= 750   => LONG_SEGMENTS = 4
//   (the 4th segment is zero-padded).  Short chirp: 1 segment, zero-padded.
//
// Per segment: collect N_FFT samples (segment 0 fresh; later segments start
// with the last OVERLAP samples of the previous one), run the chain
// (FFT -> conj-multiply with ref ROM segment -> IFFT), stream N_FFT bins.
// ref_segment passed to the chain: long = current_segment (0..3), short = 4.
// The reference ROM segment s holds the spectrum of chirp samples
// [224*s, 224*s+256), i.e. the same window the signal buffer holds when the
// input is contiguous, so a target at delay d peaks at bin d in each segment.
//
// Resources: input buffer 2 x 256 x 16 bit (RAM), overlap cache 2 x 32 x 16,
// no multipliers of its own (chain: 4 + 4).
// ============================================================================
module matched_filter_multi_segment #(
    parameter N_FFT               = 256,
    parameter LOG2N               = 8,
    parameter OVERLAP             = 32,
    parameter LONG_CHIRP_SAMPLES  = 750,
    parameter SHORT_CHIRP_SAMPLES = 13,
    parameter LONG_SEGMENTS       = 4
)(
    input wire clk,
    input wire reset_n,

    // Baseband from the DDC / gain control (25 MSPS)
    input wire signed [15:0] ddc_i,
    input wire signed [15:0] ddc_q,
    input wire ddc_valid,

    // Chirp control
    input wire use_long_chirp,
    input wire [5:0] chirp_counter,
    input wire mc_new_chirp,
    input wire mc_new_elevation,
    input wire mc_new_azimuth,

    // Pulse-compressed output
    output wire signed [15:0] pc_i_w,
    output wire signed [15:0] pc_q_w,
    output wire pc_valid_w,

    output reg [3:0] status
);

localparam ADVANCE = N_FFT - OVERLAP;

localparam [3:0] ST_IDLE         = 4'd0,
                 ST_COLLECT_DATA = 4'd1,
                 ST_ZERO_PAD     = 4'd2,
                 ST_PRIME        = 4'd3,   // present read address 0, wait for the RAM
                 ST_PROCESSING   = 4'd4,
                 ST_WAIT_FFT     = 4'd5,
                 ST_OUTPUT       = 4'd6,
                 ST_NEXT_SEGMENT = 4'd7,
                 ST_OVERLAP_COPY = 4'd8;

reg [3:0] state;

// Input buffer (inferable RAM)
reg signed [15:0] input_buffer_i [0:N_FFT-1];
reg signed [15:0] input_buffer_q [0:N_FFT-1];
reg        buf_we;
reg [LOG2N-1:0] buf_waddr;
reg signed [15:0] buf_wdata_i, buf_wdata_q;
reg [LOG2N-1:0] buf_raddr;
reg signed [15:0] buf_rdata_i, buf_rdata_q;

always @(posedge clk) begin
    if (buf_we) begin
        input_buffer_i[buf_waddr] <= buf_wdata_i;
        input_buffer_q[buf_waddr] <= buf_wdata_q;
    end
end
always @(posedge clk) begin
    buf_rdata_i <= input_buffer_i[buf_raddr];
    buf_rdata_q <= input_buffer_q[buf_raddr];
end

// Overlap cache (tail of the previous segment), written in PROCESSING
reg signed [15:0] overlap_cache_i [0:OVERLAP-1];
reg signed [15:0] overlap_cache_q [0:OVERLAP-1];
reg        ov_we;
reg [LOG2N-1:0] ov_waddr;
always @(posedge clk) begin
    if (ov_we) begin
        overlap_cache_i[ov_waddr] <= buf_rdata_i;
        overlap_cache_q[ov_waddr] <= buf_rdata_q;
    end
end

reg [LOG2N:0] buffer_write_ptr;
reg [LOG2N:0] buffer_read_ptr;
reg [15:0]    chirp_samples_collected;
reg [2:0]     current_segment;
reg [2:0]     total_segments;
reg           chirp_complete;
reg           saw_chain_output;
reg           primed;
reg [LOG2N-1:0] overlap_copy_count;

// mc_new_chirp is a TOGGLE (radar_mode_controller.v:9), so any edge starts a
// chirp.  (The previous rising-edge-only detection silently dropped every
// second chirp.)
reg mc_new_chirp_prev, mc_new_elevation_prev, mc_new_azimuth_prev;
wire chirp_start_pulse = mc_new_chirp ^ mc_new_chirp_prev;

always @(posedge clk or negedge reset_n) begin
    if (!reset_n) begin
        mc_new_chirp_prev <= 1'b0; mc_new_elevation_prev <= 1'b0; mc_new_azimuth_prev <= 1'b0;
    end else begin
        mc_new_chirp_prev <= mc_new_chirp; mc_new_elevation_prev <= mc_new_elevation; mc_new_azimuth_prev <= mc_new_azimuth;
    end
end

// Chain interface
wire [15:0] fft_pc_i, fft_pc_q;
wire        fft_pc_valid;
wire [3:0]  fft_chain_state;
reg signed [15:0] fft_input_i, fft_input_q;
reg         fft_input_valid;
wire [2:0]  ref_segment = use_long_chirp ? current_segment : 3'd4;

always @(posedge clk or negedge reset_n) begin
    if (!reset_n) begin
        state <= ST_IDLE;
        buffer_write_ptr <= 0; buffer_read_ptr <= 0; chirp_samples_collected <= 0;
        current_segment <= 0; total_segments <= 0; chirp_complete <= 0; saw_chain_output <= 0; primed <= 0;
        buf_we <= 0; buf_waddr <= 0; buf_wdata_i <= 0; buf_wdata_q <= 0; buf_raddr <= 0;
        ov_we <= 0; ov_waddr <= 0; overlap_copy_count <= 0;
        fft_input_i <= 0; fft_input_q <= 0; fft_input_valid <= 0;
        status <= 0;
    end else begin
        buf_we <= 1'b0;
        ov_we  <= 1'b0;
        fft_input_valid <= 1'b0;

        case (state)
        ST_IDLE: begin
            buffer_write_ptr <= 0; buffer_read_ptr <= 0; chirp_samples_collected <= 0;
            current_segment <= 0; chirp_complete <= 0; saw_chain_output <= 0;
            if (chirp_start_pulse) begin
                state <= ST_COLLECT_DATA;
                total_segments <= use_long_chirp ? LONG_SEGMENTS[2:0] : 3'd1;
            end
        end

        ST_COLLECT_DATA: begin
            if (ddc_valid && buffer_write_ptr < N_FFT) begin
                buf_we <= 1'b1;
                buf_waddr <= buffer_write_ptr[LOG2N-1:0];
                buf_wdata_i <= ddc_i;
                buf_wdata_q <= ddc_q;
                buffer_write_ptr <= buffer_write_ptr + 1;
                chirp_samples_collected <= chirp_samples_collected + 1;
                if (!use_long_chirp && chirp_samples_collected >= SHORT_CHIRP_SAMPLES - 1) begin
                    chirp_complete <= 1'b1;      // short chirp = one segment, then IDLE
                    state <= ST_ZERO_PAD;
                end
            end
            if (use_long_chirp) begin
                if (buffer_write_ptr >= N_FFT) begin
                    state <= ST_PRIME; primed <= 1'b0;
                end
                if (chirp_samples_collected >= LONG_CHIRP_SAMPLES && !chirp_complete) begin
                    chirp_complete <= 1'b1;
                    if (buffer_write_ptr < N_FFT) state <= ST_ZERO_PAD;
                end
            end
        end

        ST_ZERO_PAD: begin
            buf_we <= 1'b1;
            buf_waddr <= buffer_write_ptr[LOG2N-1:0];
            buf_wdata_i <= 16'sd0;
            buf_wdata_q <= 16'sd0;
            buffer_write_ptr <= buffer_write_ptr + 1;
            if (buffer_write_ptr >= N_FFT - 1) begin
                buffer_write_ptr <= 0;
                state <= ST_PRIME; primed <= 1'b0;
            end
        end

        ST_PRIME: begin
            // Two cycles: buf_raddr = 0 -> buf_rdata = buffer[0] -> first PROCESSING cycle
            buf_raddr <= {LOG2N{1'b0}};
            buffer_read_ptr <= 0;
            primed <= 1'b1;
            if (primed) state <= ST_PROCESSING;
        end

        ST_PROCESSING: begin
            if (buffer_read_ptr < N_FFT) begin
                fft_input_i <= buf_rdata_i;
                fft_input_q <= buf_rdata_q;
                fft_input_valid <= 1'b1;
                if (buffer_read_ptr >= ADVANCE) begin
                    ov_we <= 1'b1;
                    ov_waddr <= buffer_read_ptr[LOG2N-1:0] - ADVANCE[LOG2N-1:0];
                end
                buf_raddr <= buffer_read_ptr[LOG2N-1:0] + 1'b1;
                buffer_read_ptr <= buffer_read_ptr + 1;
            end else begin
                saw_chain_output <= 1'b0;
                state <= ST_WAIT_FFT;
            end
        end

        ST_WAIT_FFT: begin
            if (fft_pc_valid) saw_chain_output <= 1'b1;
            if (saw_chain_output && fft_chain_state == 4'd0) begin
                saw_chain_output <= 1'b0;
                state <= ST_OUTPUT;
            end
        end

        ST_OUTPUT: begin
            if (current_segment < total_segments - 1 || !chirp_complete)
                state <= ST_NEXT_SEGMENT;
            else
                state <= ST_IDLE;
        end

        ST_NEXT_SEGMENT: begin
            current_segment <= current_segment + 1;
            if (use_long_chirp) begin
                overlap_copy_count <= 0;
                state <= ST_OVERLAP_COPY;
            end else begin
                buffer_write_ptr <= 0;
                state <= chirp_complete ? ST_IDLE : ST_COLLECT_DATA;
            end
        end

        ST_OVERLAP_COPY: begin
            buf_we <= 1'b1;
            buf_waddr <= overlap_copy_count;
            buf_wdata_i <= overlap_cache_i[overlap_copy_count[4:0]];
            buf_wdata_q <= overlap_cache_q[overlap_copy_count[4:0]];
            if (overlap_copy_count < OVERLAP - 1) begin
                overlap_copy_count <= overlap_copy_count + 1;
            end else begin
                buffer_write_ptr <= OVERLAP;
                state <= chirp_complete ? ST_IDLE : ST_COLLECT_DATA;
            end
        end

        default: state <= ST_IDLE;
        endcase

        status <= {state[2:0], use_long_chirp};
    end
end

matched_filter_processing_chain #(.N_FFT(N_FFT), .LOG2N(LOG2N)) m_f_p_c (
    .clk(clk), .reset_n(reset_n),
    .adc_data_i(fft_input_i), .adc_data_q(fft_input_q), .adc_valid(fft_input_valid),
    .chirp_counter(chirp_counter), .ref_segment(ref_segment),
    .range_profile_i(fft_pc_i), .range_profile_q(fft_pc_q), .range_profile_valid(fft_pc_valid),
    .chain_state(fft_chain_state));

assign pc_i_w = fft_pc_i;
assign pc_q_w = fft_pc_q;
assign pc_valid_w = fft_pc_valid;

`ifdef SIMULATION
integer init_k;
initial begin
    for (init_k = 0; init_k < N_FFT; init_k = init_k + 1) begin input_buffer_i[init_k] = 0; input_buffer_q[init_k] = 0; end
    for (init_k = 0; init_k < OVERLAP; init_k = init_k + 1) begin overlap_cache_i[init_k] = 0; overlap_cache_q[init_k] = 0; end
end
`endif

endmodule
```

(`overlap_copy_count[4:0]` assumes OVERLAP = 32; if OVERLAP is changed, change the index width.)

- [ ] **Step 6: Integrate into `radar_receiver_final.v`**

1. Delete the wires `segment_request`, `mem_request`, `ref_i, ref_q`, `mem_ready` (lines 92–95) and `long_chirp_real/imag`, `short_chirp_real/imag` (107–109).
2. Delete lines 269–317 (sections "3. Dual Chirp Memory Loader", the sample address generator and "4. CRITICAL: Reference Chirp Latency Buffer") and the `wire [9:0] sample_addr_from_chain;` line.
3. Replace the `matched_filter_multi_segment mf_dual (...)` instance (330–352) with:

```verilog
// 3. Matched filter: 256-point overlap-save segments, reference spectra in ROM
matched_filter_multi_segment #(
    .N_FFT(256), .LOG2N(8), .OVERLAP(32),
    .LONG_CHIRP_SAMPLES(750), .SHORT_CHIRP_SAMPLES(13), .LONG_SEGMENTS(4)
) mf_dual (
    .clk(clk),
    .reset_n(reset_n),
    .ddc_i(gc_i),
    .ddc_q(gc_q),
    .ddc_valid(gc_valid),
    .use_long_chirp(use_long_chirp),
    .chirp_counter(chirp_counter),
    .mc_new_chirp(mc_new_chirp),
    .mc_new_elevation(mc_new_elevation),
    .mc_new_azimuth(mc_new_azimuth),
    .pc_i_w(range_profile_i),
    .pc_q_w(range_profile_q),
    .pc_valid_w(range_valid),
    .status()
);
```

4. Range-bin decimator (354–373): comment `// Convert 256 range bins to 64 bins for Doppler`, parameters `.INPUT_BINS(256), .OUTPUT_BINS(64), .DECIMATION_FACTOR(4)`.

- [ ] **Step 7: Golden test (d) — full receiver chain, peak displacement**

Create `tb/golden/gen_fullchain_golden.py`:

```python
#!/usr/bin/env python3
"""gen_fullchain_golden.py — golden test (d): ADC vectors for a target echo.

Two 4096-sample 12-bit ADC vectors: the 30 us IF chirp (10..30 MHz) starting
D1 = 100 and D2 = 260 ADC samples after the chirp-start pulse, amplitude
0.25 FS.  Through the DDC (x4 decimation) the delays differ by
(D2 - D1) / 4 = 40 range bins, independent of the pipeline latency, so the
testbench checks  peak_bin(D2) - peak_bin(D1) == 40 (+-1).
Writes fullchain_adc_d1.hex / fullchain_adc_d2.hex.
"""
import os
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
from radar_params import if_chirp_adc, write_hex  # noqa: E402

N_IN = 4096
D1, D2 = 100, 260
AMP = 512.0


def main():
    for name, d in (("d1", D1), ("d2", D2)):
        adc = if_chirp_adc(N_IN, AMP, d, seed=11)
        write_hex(os.path.join(HERE, f"fullchain_adc_{name}.hex"), adc, 12)
    print(f"wrote fullchain_adc_d1/d2.hex; expected peak displacement = {(D2 - D1) // 4} bins")


if __name__ == "__main__":
    main()
```

Create `tb/golden/tb_fullchain_golden.v`:

```verilog
`timescale 1ns / 1ps
// ============================================================================
// tb_fullchain_golden.v — golden test (d): ADC -> DDC -> gain -> matched
// filter range profile.  Single-chirp mode (host_mode = 2'b10, host_trigger):
// the echo starts D ADC samples after the chirp-start pulse; the range-profile
// peak of segment 0 must move by (D2 - D1)/4 = 40 bins between the two runs
// and stand at least 8x above the mean.  Gain shift 1100b (/16) keeps the
// FFT input below saturation.
// ============================================================================
module tb_fullchain_golden;
    localparam CLK_PERIOD = 10;
    localparam N_IN = 4096, N_FFT = 256;
    localparam EXPECTED_DELTA = 40;
    reg clk, reset_n, host_trigger;
    reg [11:0] adc_data;
    reg [11:0] adc_mem [0:N_IN-1];
    wire signed [15:0] rp_i, rp_q;
    wire rp_valid;
    wire doppler_frame_done;
    integer i, k, pass_count, fail_count, test_num;
    integer cap_count, peak_bin, peak_mag, mean_mag, mag, peak1, peak2;
    reg capturing;

    always #(CLK_PERIOD/2) clk = ~clk;

    radar_receiver_final dut (
        .clk(clk), .reset_n(reset_n),
        .adc_data(adc_data), .adc_ovr(1'b0), .adc_pwdn(), .adc_overrange(),
        .chirp_counter(6'd0), .tx_frame_start(1'b0),
        .doppler_output(), .doppler_valid(), .doppler_bin(), .range_bin(),
        .range_profile_i_out(rp_i), .range_profile_q_out(rp_q), .range_profile_valid_out(rp_valid),
        .host_mode(2'b10), .host_trigger(host_trigger),
        .host_long_chirp_cycles(16'd3000), .host_long_listen_cycles(16'd13700), .host_guard_cycles(16'd17540),
        .host_short_chirp_cycles(16'd50), .host_short_listen_cycles(16'd17450), .host_chirps_per_elev(6'd32),
        .host_gain_shift(4'b1100),
        .host_agc_enable(1'b0), .host_agc_target(8'd200), .host_agc_attack(4'd1), .host_agc_decay(4'd1), .host_agc_holdoff(4'd4),
        .stm32_new_chirp_rx(1'b0), .stm32_new_elevation_rx(1'b0), .stm32_new_azimuth_rx(1'b0),
        .doppler_frame_done_out(doppler_frame_done),
        .host_mti_enable(1'b0), .host_dc_notch_width(3'd0),
        .dbg_adc_i(), .dbg_adc_q(), .dbg_adc_valid(),
        .agc_saturation_count(), .agc_peak_magnitude(), .agc_current_gain());

    task check;
        input cond; input [511:0] label;
        begin
            test_num = test_num + 1;
            if (cond) begin $display("[PASS] Test %0d: %0s", test_num, label); pass_count = pass_count + 1; end
            else       begin $display("[FAIL] Test %0d: %0s", test_num, label); fail_count = fail_count + 1; end
        end
    endtask

    // Capture the first N_FFT range-profile outputs after a trigger (segment 0)
    always @(posedge clk) if (capturing && rp_valid) begin
        if (cap_count < N_FFT) begin
            mag = (rp_i < 0 ? -rp_i : rp_i) + (rp_q < 0 ? -rp_q : rp_q);
            mean_mag = mean_mag + mag;
            if (mag > peak_mag) begin peak_mag = mag; peak_bin = cap_count; end
        end
        cap_count = cap_count + 1;
    end

    task run_chirp;
        input [1023:0] hexfile;
        integer t;
        begin
            $readmemh(hexfile, adc_mem);
            cap_count = 0; peak_bin = -1; peak_mag = 0; mean_mag = 0; capturing = 1;
            // trigger one long chirp; the mode controller toggles mc_new_chirp a few cycles later
            host_trigger = 1; @(posedge clk); #1; host_trigger = 0;
            @(dut.mc_new_chirp);                         // toggle = chirp start (segmenter starts collecting)
            for (k = 0; k < N_IN; k = k + 1) begin
                @(posedge clk); #1; adc_data = adc_mem[k];
            end
            adc_data = 12'h800;
            t = 0;
            while (cap_count < N_FFT && t < 200000) begin @(posedge clk); t = t + 1; end
            capturing = 0;
            mean_mag = mean_mag / N_FFT;
            $display("  segment 0: outputs=%0d peak_bin=%0d peak=%0d mean=%0d", cap_count, peak_bin, peak_mag, mean_mag);
            // The mode controller only accepts a trigger in S_IDLE, which it re-enters
            // long_chirp + long_listen = 16700 cycles after the trigger.  Wait that out
            // (the 4-segment matched filter finishes well within it).
            repeat (17000) @(posedge clk); #1;
            check(dut.mf_dual.state == 4'd0, "segmenter back in IDLE after the chirp");
        end
    endtask

    initial begin
        clk = 0; reset_n = 0; host_trigger = 0; adc_data = 12'h800; capturing = 0;
        pass_count = 0; fail_count = 0; test_num = 0;
        repeat (10) @(posedge clk); #1; reset_n = 1;
        repeat (50) @(posedge clk); #1;

        run_chirp("tb/golden/fullchain_adc_d1.hex");
        check(cap_count >= N_FFT, "run 1: segment 0 produced 256 range bins");
        check(peak_mag > 8 * mean_mag, "run 1: peak >= 8x mean");
        peak1 = peak_bin;

        run_chirp("tb/golden/fullchain_adc_d2.hex");
        check(cap_count >= N_FFT, "run 2: segment 0 produced 256 range bins");
        check(peak_mag > 8 * mean_mag, "run 2: peak >= 8x mean");
        peak2 = peak_bin;

        $display("peak bins: run1=%0d run2=%0d delta=%0d (expected %0d)", peak1, peak2, peak2 - peak1, EXPECTED_DELTA);
        check(peak2 - peak1 >= EXPECTED_DELTA - 1 && peak2 - peak1 <= EXPECTED_DELTA + 1,
              "range-profile peak moves by (D2-D1)/4 = 40 bins (+-1)");
        $display("\nResults: %0d/%0d passed", pass_count, pass_count + fail_count);
        $finish;
    end
endmodule
```

- [ ] **Step 8: Deletions, regression script, TB fixes**

```bash
git rm -q latency_buffer.v chirp_memory_loader_param.v long_chirp_seg0_i.mem long_chirp_seg0_q.mem \
  long_chirp_seg1_i.mem long_chirp_seg1_q.mem long_chirp_seg2_i.mem long_chirp_seg2_q.mem \
  long_chirp_seg3_i.mem long_chirp_seg3_q.mem short_chirp_i.mem short_chirp_q.mem \
  tb/tb_latency_buffer.v tb/tb_matched_filter_processing_chain.v tb/tb_mf_chain_synth.v \
  tb/tb_mf_cosim.v tb/tb_multiseg_cosim.v tb/cosim/compare.py tb/cosim/compare_mf.py \
  tb/cosim/gen_mf_cosim_golden.py tb/cosim/gen_multiseg_golden.py tb/cosim/gen_chirp_mem.py tb/cosim/validate_mem_files.py
python3 tb/golden/gen_fullchain_golden.py
```

(`tb/cosim/radar_scene.py` and `fpga_model.py` stay: `gen_doppler_golden.py` and the new generators import them. The `long_chirp_lut.mem` file belongs to the transmitter and stays.)

`run_regression.sh`:
- Add after `run_test()` (line ~316) a variant that omits `-DSIMULATION`:

```bash
# ---------------------------------------------------------------------------
# Helper: compile WITHOUT -DSIMULATION (exercises the synthesizable branches)
# ---------------------------------------------------------------------------
run_test_nosim() {
    local name="$1"
    local vvp="$2"
    shift 2
    local args=("$@")

    printf "  %-45s " "$name"
    if ! iverilog -g2001 -o "$vvp" "${args[@]}" 2>/tmp/iverilog_err_$$; then
        echo -e "${RED}COMPILE FAIL${NC}"
        ERRORS="$ERRORS\n  $name: compile error ($(head -1 /tmp/iverilog_err_$$))"
        FAIL=$((FAIL + 1))
        return
    fi
    local output
    output=$(timeout 300 vvp "$vvp" 2>&1) || true
    local test_pass test_fail
    test_pass=$(echo "$output" | grep -Ec '^\[PASS([^]]*)\]' || true)
    test_fail=$(echo "$output" | grep -Ec '^\[FAIL([^]]*)\]' || true)
    if [[ "$test_fail" -gt 0 ]]; then
        echo -e "${RED}FAIL${NC} (pass=$test_pass, fail=$test_fail)"
        ERRORS="$ERRORS\n  $name: $test_fail failure(s)"
        FAIL=$((FAIL + 1))
    elif [[ "$test_pass" -gt 0 ]]; then
        echo -e "${GREEN}PASS${NC} ($test_pass checks)"
        PASS=$((PASS + 1))
    else
        echo -e "${YELLOW}UNKNOWN${NC} (no PASS/FAIL markers)"
        ERRORS="$ERRORS\n  $name: no pass/fail markers in output"
        FAIL=$((FAIL + 1))
    fi
    rm -f "$vvp"
}
```

- `PROD_RTL` and `RECEIVER_RTL`: remove `latency_buffer.v`, `chirp_memory_loader_param.v`; add `ref_spectrum_rom.v` next to `matched_filter_processing_chain.v`; add `frequency_matched_filter.v` to both lists (it is now instantiated by the pipeline). Delete the `EXTRA_RTL` array, its `for extra in ...` loop in Phase 0, and change `ALL_RTL=("${PROD_RTL[@]}" "${EXTRA_RTL[@]}")` to `ALL_RTL=("${PROD_RTL[@]}")` (an empty array breaks `set -u` on macOS bash 3.2).
- Phase 2: after the full-chain real-data test add:

```bash
run_test "Full-chain golden (d): range peak displacement" \
    tb/tb_fullchain_golden_reg.vvp \
    tb/golden/tb_fullchain_golden.v "${RECEIVER_RTL[@]}"
```

- Phase 3: replace the "Matched Filter Chain" entry with:

```bash
run_test "Matched Filter Chain (behavioral branch)" \
    tb/tb_mf_beh_reg.vvp \
    tb/tb_mf_chain.v matched_filter_processing_chain.v fft_engine.v ref_spectrum_rom.v frequency_matched_filter.v

run_test_nosim "Matched Filter Chain (synthesizable, no -DSIMULATION)" \
    tb/tb_mf_syn_reg.vvp \
    tb/tb_mf_chain.v matched_filter_processing_chain.v fft_engine.v ref_spectrum_rom.v frequency_matched_filter.v
```

`tb/tb_radar_receiver_final.v`: line 183 comment → `// Need enough DDC samples to fill the MF segment (256 at 25 MSPS = 1024 clk).`; lines 520–535 comment: drop the latency-buffer bullets (`3. Latency buffer priming: 3187`, `plus latency buffer priming`) and change `1024 MF outputs -> ... -> 64` to `4 x 256 MF outputs -> range_bin_decimator -> 64`. The hierarchical references `dut.mf_dual.m_f_p_c.fwd_in_count`/`.out_count`/`.state` still exist.

Run: `./run_regression.sh 2>&1 | tail -10`
Expected: `Tests: 33 passed, 0 failed` (`tb_fullchain_golden` reports 7 checks). The receiver golden regenerates (committed). If `tb_fullchain_golden` reports `outputs=0`, the single-chirp trigger did not start the segmenter — check that `host_mode = 2'b10` makes `radar_mode_controller` toggle `mc_new_chirp` (`radar_mode_controller.v:345-349`) and that the segmenter's `chirp_start_pulse` fires on both edges of the toggle.

- [ ] **Step 9: Commit**

```bash
git add -A .
git commit -m "fpga: 256-pt matched filter with ROM reference spectra (FFT+IFFT only), delete latency buffer/chirp loader; synth-branch regression + golden (d)

Co-Authored-By: Claude Fable 5.1 <noreply@anthropic.com>"
```

---

### Task 11: MTI history in inferable RAM; remove dead debug counters (R5)

**Files:**
- Rewrite: `mti_canceller.v`
- Modify: `radar_receiver_final.v` (delete the `frame_counter` block, formerly lines 461–488)
- Modify: `tb/tb_mti_canceller.v` (only if a check depends on the 1-cycle latency — see Step 3)

**Interfaces:**
- Produces: `mti_canceller` ports unchanged (`clk, reset_n, range_i_in, range_q_in, range_valid_in, range_bin_in, range_i_out, range_q_out, range_valid_out, range_bin_out, mti_enable, mti_first_chirp`); output latency becomes 2 clocks (was 1). History arrays `prev_i`, `prev_q` are RAM-inferable (write in their own always block, no reset, synchronous read).

- [ ] **Step 1: Run the existing MTI test to have a reference**

Run: `iverilog -g2001 -DSIMULATION -o /tmp/tb_mti.vvp tb/tb_mti_canceller.v mti_canceller.v && vvp /tmp/tb_mti.vvp | grep -c "^\[PASS"` → record the count (expected 24).

- [ ] **Step 2: Rewrite `mti_canceller.v`**

```verilog
`timescale 1ns / 1ps

/**
 * mti_canceller.v
 *
 * Moving Target Indication (MTI) — 2-pulse canceller for ground clutter removal.
 *
 * Sits between the range bin decimator and the Doppler processor.  Subtracts
 * the previous chirp's range profile from the current one, H(z) = 1 - z^-1 in
 * slow time: a null at zero Doppler removes stationary clutter.
 *
 * Algorithm, for each range bin r:
 *   out[r] = sat16( cur[r] - prev[r] );   prev[r] <= cur[r]
 * On the first chirp after reset/enable the output is zero (muted) because
 * there is no previous chirp.  When mti_enable = 0 the module passes data
 * through (same 2-clock latency).
 *
 * Implementation (RAM-inferable history):
 *   clock 1: register the input (cur_*), read prev[range_bin_in] synchronously
 *   clock 2: output = cur - prev_rd (saturated); write prev[cur_bin] <= cur
 * The write happens one clock after the read of the same address, so there is
 * never a same-cycle read/write collision on the history RAM.  The arrays are
 * written in their own always block without reset (block RAM / LUT RAM).
 *
 * Resources: 2 x 64 x 16 bit RAM, ~40 LUTs (subtract + saturate + mux),
 * ~60 flip-flops, 0 multipliers.
 */

module mti_canceller #(
    parameter NUM_RANGE_BINS = 64,
    parameter DATA_WIDTH     = 16
) (
    input wire clk,
    input wire reset_n,

    input wire signed [DATA_WIDTH-1:0] range_i_in,
    input wire signed [DATA_WIDTH-1:0] range_q_in,
    input wire                         range_valid_in,
    input wire [5:0]                   range_bin_in,

    output reg signed [DATA_WIDTH-1:0] range_i_out,
    output reg signed [DATA_WIDTH-1:0] range_q_out,
    output reg                         range_valid_out,
    output reg [5:0]                   range_bin_out,

    input wire mti_enable,

    output reg mti_first_chirp
);

// ---- Previous-chirp history (RAM) ----
reg signed [DATA_WIDTH-1:0] prev_i [0:NUM_RANGE_BINS-1];
reg signed [DATA_WIDTH-1:0] prev_q [0:NUM_RANGE_BINS-1];

// Stage 1 registers
reg signed [DATA_WIDTH-1:0] cur_i, cur_q;
reg                         cur_valid;
reg [5:0]                   cur_bin;
reg signed [DATA_WIDTH-1:0] prev_i_rd, prev_q_rd;

// Synchronous read (stage 1)
always @(posedge clk) begin
    prev_i_rd <= prev_i[range_bin_in];
    prev_q_rd <= prev_q[range_bin_in];
end

// Write port (stage 2): store the current chirp for the next one
always @(posedge clk) begin
    if (cur_valid) begin
        prev_i[cur_bin] <= cur_i;
        prev_q[cur_bin] <= cur_q;
    end
end

always @(posedge clk or negedge reset_n) begin
    if (!reset_n) begin
        cur_i <= 0; cur_q <= 0; cur_valid <= 1'b0; cur_bin <= 6'd0;
    end else begin
        cur_i     <= range_i_in;
        cur_q     <= range_q_in;
        cur_valid <= range_valid_in;
        cur_bin   <= range_bin_in;
    end
end

// ---- Difference with saturation ----
wire signed [DATA_WIDTH:0] diff_i_full = {cur_i[DATA_WIDTH-1], cur_i} - {prev_i_rd[DATA_WIDTH-1], prev_i_rd};
wire signed [DATA_WIDTH:0] diff_q_full = {cur_q[DATA_WIDTH-1], cur_q} - {prev_q_rd[DATA_WIDTH-1], prev_q_rd};

localparam signed [DATA_WIDTH:0] MAXP =  (1 << (DATA_WIDTH - 1)) - 1;
localparam signed [DATA_WIDTH:0] MINN = -(1 << (DATA_WIDTH - 1));

wire signed [DATA_WIDTH-1:0] diff_i_sat = (diff_i_full > MAXP) ? MAXP[DATA_WIDTH-1:0] :
                                          (diff_i_full < MINN) ? MINN[DATA_WIDTH-1:0] : diff_i_full[DATA_WIDTH-1:0];
wire signed [DATA_WIDTH-1:0] diff_q_sat = (diff_q_full > MAXP) ? MAXP[DATA_WIDTH-1:0] :
                                          (diff_q_full < MINN) ? MINN[DATA_WIDTH-1:0] : diff_q_full[DATA_WIDTH-1:0];

// ---- Stage 2: output ----
reg has_previous;

always @(posedge clk or negedge reset_n) begin
    if (!reset_n) begin
        range_i_out     <= {DATA_WIDTH{1'b0}};
        range_q_out     <= {DATA_WIDTH{1'b0}};
        range_valid_out <= 1'b0;
        range_bin_out   <= 6'd0;
        has_previous    <= 1'b0;
        mti_first_chirp <= 1'b1;
    end else begin
        range_valid_out <= 1'b0;
        if (cur_valid) begin
            range_bin_out   <= cur_bin;
            range_valid_out <= 1'b1;
            if (!mti_enable) begin
                range_i_out     <= cur_i;
                range_q_out     <= cur_q;
                has_previous    <= 1'b0;
                mti_first_chirp <= 1'b1;
            end else if (!has_previous) begin
                range_i_out <= {DATA_WIDTH{1'b0}};
                range_q_out <= {DATA_WIDTH{1'b0}};
                if (cur_bin == NUM_RANGE_BINS - 1) begin
                    has_previous    <= 1'b1;
                    mti_first_chirp <= 1'b0;
                end
            end else begin
                range_i_out <= diff_i_sat;
                range_q_out <= diff_q_sat;
            end
        end
    end
end

`ifdef SIMULATION
integer init_k;
initial begin
    for (init_k = 0; init_k < NUM_RANGE_BINS; init_k = init_k + 1) begin
        prev_i[init_k] = 0;
        prev_q[init_k] = 0;
    end
end
`endif

endmodule
```

- [ ] **Step 3: Run the MTI test**

Run: `iverilog -g2001 -DSIMULATION -o /tmp/tb_mti.vvp tb/tb_mti_canceller.v mti_canceller.v && vvp /tmp/tb_mti.vvp | grep -E "^\["`
Expected: the same PASS count as Step 1 and no FAIL. If a check fails because the TB samples an output a fixed number of cycles after the input (instead of on `range_valid_out`), add one `@(posedge clk);` before that sample in the TB — the module's observable behaviour on `range_valid_out` is unchanged apart from the extra cycle of latency.

- [ ] **Step 4: Remove the dead `frame_counter` in the receiver**

In `radar_receiver_final.v` delete the block `// ========== DEBUG AND VERIFICATION ==========` through the `end` of its `always` (the `frame_counter` / `chirps_in_current_frame` logic). Nothing reads these registers; the status packet (`usb_data_interface_ft2232h.v`) only uses `range_valid`/`doppler_valid`/`cfar_valid` and the AGC/status inputs.

Check that no other dead debug counters remain in the pipeline: `grep -n "sample_count\|debug_.*count\|frame_counter" ddc.v cic_decimator_4x_enhanced.v radar_receiver_final.v` → no output.

- [ ] **Step 5: Regression + commit**

Run: `./run_regression.sh 2>&1 | tail -6` → `Tests: 33 passed, 0 failed` (receiver golden regenerated: the MTI latency moved by one cycle).

```bash
git add -A .
git commit -m "fpga: mti_canceller history in inferable RAM (2-clock latency); remove dead debug counters

Co-Authored-By: Claude Fable 5.1 <noreply@anthropic.com>"
```

---

### Task 12: Resource gate script, vendor-primitive grep gate, no-SIMULATION lint, CI, README

**Files:**
- Create: `tb/golden/count_multipliers.py`, `README.md`
- Modify: `run_regression.sh` (Phase 0 layers C/D, test-name cleanup), `.github/workflows/ci-tests.yml:71-86`

**Interfaces:**
- `python3 tb/golden/count_multipliers.py [file.v ...]` prints a per-module table and exits 1 if multiply sites × instances > 55 or RAM bits > 1 048 576. With no arguments it uses the pipeline list embedded in the script (same as `PROD_RTL`).

- [ ] **Step 1: Write the resource script**

Create `tb/golden/count_multipliers.py`:

```python
#!/usr/bin/env python3
"""count_multipliers.py — static multiplier / RAM budget gate for the pipeline.

Multipliers: counts '*' operators in synthesizable code (comments, attributes,
strings, `ifdef SIMULATION / `ifdef FORMAL regions and parameter lines are
ignored), multiplied by the number of instances of each module in the
radar_system_top hierarchy (derived by scanning instantiations).  This is a
conservative STATIC bound: sites in mutually exclusive case branches
(doppler_processor window multiply) and multiplies by constant powers of two
(address computations) are counted even though synthesis merges/removes them.

RAM: hand-maintained table of inferred memories (module, array, width, depth,
instances); the script verifies that each array is still declared so the table
cannot rot silently.

Exit status 1 if multipliers > 55 or RAM bits > 1 Mbit (spec acceptance).
Run from 9_Firmware/9_2_FPGA:  python3 tb/golden/count_multipliers.py
"""
import os
import re
import sys

MAX_MULT = 55
MAX_RAM_BITS = 1 << 20
TOP = "radar_system_top"

PIPELINE = [
    "radar_system_top.v", "radar_transmitter.v", "dac_interface_single.v", "plfm_chirp_controller.v",
    "radar_receiver_final.v", "adc_cmos_interface.v", "ddc.v", "nco.v", "cic_decimator_4x_enhanced.v",
    "fir_lowpass.v", "cdc_modules.v", "matched_filter_multi_segment.v", "matched_filter_processing_chain.v",
    "frequency_matched_filter.v", "ref_spectrum_rom.v", "range_bin_decimator.v", "doppler_processor.v",
    "xfft_16.v", "fft_engine.v", "usb_data_interface.v", "usb_data_interface_ft2232h.v", "edge_detector.v",
    "radar_mode_controller.v", "rx_gain_control.v", "cfar_ca.v", "mti_canceller.v", "fpga_self_test.v",
]

# Modules inside generate branches that are NOT elaborated with the default parameters
INSTANCE_OVERRIDES = {"usb_data_interface": 0}   # USB_MODE = 1 selects usb_data_interface_ft2232h

# (file, array, width_bits, depth, instances, note)
RAM_TABLE = [
    ("fft_engine.v", "mem_re", 24, 256, 1, "range FFT data (re)"),
    ("fft_engine.v", "mem_im", 24, 256, 1, "range FFT data (im)"),
    ("fft_engine.v", "cos_rom", 16, 64, 1, "range FFT twiddles"),
    ("fft_engine.v", "mem_re", 24, 16, 1, "Doppler FFT data (re)"),
    ("fft_engine.v", "mem_im", 24, 16, 1, "Doppler FFT data (im)"),
    ("fft_engine.v", "cos_rom", 16, 4, 1, "Doppler FFT twiddles"),
    ("xfft_16.v", "in_buf_re", 16, 16, 2, "in_buf re/im"),
    ("xfft_16.v", "out_buf_re", 16, 16, 2, "out_buf re/im"),
    ("ref_spectrum_rom.v", "rom_i", 16, 1280, 2, "reference spectra i/q"),
    ("matched_filter_processing_chain.v", "sig_buf_i", 16, 256, 2, "signal buffer i/q"),
    ("matched_filter_processing_chain.v", "prod_buf_i", 16, 256, 2, "product buffer i/q"),
    ("matched_filter_multi_segment.v", "input_buffer_i", 16, 256, 2, "segment buffer i/q"),
    ("matched_filter_multi_segment.v", "overlap_cache_i", 16, 32, 2, "overlap cache i/q"),
    ("doppler_processor.v", "doppler_i_mem", 16, 2048, 2, "Doppler frame i/q (64 x 32)"),
    ("cfar_ca.v", "mag_mem", 17, 2048, 1, "CFAR magnitude map"),
    ("cfar_ca.v", "col_buf", 17, 64, 1, "CFAR column buffer"),
    ("mti_canceller.v", "prev_i", 16, 64, 2, "MTI history i/q"),
    ("plfm_chirp_controller.v", "long_chirp_lut", 8, 3600, 1, "TX long chirp LUT"),
    ("plfm_chirp_controller.v", "short_chirp_lut", 8, 60, 1, "TX short chirp LUT"),
    ("nco.v", "sin_lut", 16, 64, 1, "NCO quarter-wave LUT"),
    ("fpga_self_test.v", "test_bram", 16, 64, 1, "self-test RAM"),
    ("usb_data_interface_ft2232h.v", "status_words", 32, 6, 1, "status words"),
]

ROOT = os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "..")


def strip_for_synthesis(text):
    """Remove comments, attributes, strings and SIMULATION/FORMAL-only regions."""
    text = re.sub(r"/\*.*?\*/", " ", text, flags=re.S)
    text = text.replace("@(*)", "@(_all_)").replace("@*", "@(_all_)")   # sensitivity lists are not products
    text = re.sub(r"\(\*\s*[A-Za-z_].*?\*\)", " ", text, flags=re.S)     # (* attribute *)
    out, stack = [], []        # stack of booleans: is the current region active?
    for line in text.splitlines():
        code = re.sub(r"//.*", "", line)
        code = re.sub(r'"[^"]*"', '""', code)
        m = re.match(r"\s*`(ifdef|ifndef|else|endif)\b\s*(\w*)", code)
        if m:
            kw, sym = m.group(1), m.group(2)
            if kw == "ifdef":
                stack.append(sym not in ("SIMULATION", "FORMAL", "SIMULATION_HAS_BUFG"))
            elif kw == "ifndef":
                stack.append(sym in ("SIMULATION", "FORMAL"))
            elif kw == "else" and stack:
                stack[-1] = not stack[-1]
            elif kw == "endif" and stack:
                stack.pop()
            continue
        if all(stack):
            out.append(code)
    return "\n".join(out)


def multiply_sites(code):
    sites = []
    for ln, line in enumerate(code.splitlines(), 1):
        if re.match(r"\s*(localparam|parameter)\b", line):
            continue
        stripped = re.sub(r"\*\*", "", line)        # power operator
        count = stripped.count("*")
        if count:
            sites.append((ln, count, line.strip()))
    return sites


def module_blocks(code):
    """Return [(module_name, code_of_that_module)] — files may hold several modules."""
    blocks = []
    for m in re.finditer(r"^\s*module\s+(\w+)(.*?)^\s*endmodule", code, flags=re.M | re.S):
        blocks.append((m.group(1), m.group(2)))
    return blocks


def instantiations(code, known):
    """Return {child_module: count} for instantiations found in the code."""
    counts = {}
    for name in known:
        n = len(re.findall(r"^\s*" + re.escape(name) + r"\s*(#\s*\(|\w+\s*\()", code, flags=re.M))
        if n:
            counts[name] = n
    return counts


def main():
    files = sys.argv[1:] or PIPELINE
    code_by_file, mods_by_file, code_of_module = {}, {}, {}
    for f in files:
        with open(os.path.join(ROOT, f)) as fh:
            raw = fh.read()
        code_by_file[f] = strip_for_synthesis(raw)
        mods_by_file[f] = []
        for name, block in module_blocks(code_by_file[f]):
            mods_by_file[f].append(name)
            code_of_module[name] = block
    file_of_module = {m: f for f, ms in mods_by_file.items() for m in ms}
    children = {m: instantiations(code_of_module[m], file_of_module.keys()) for m in code_of_module}

    # instance multiplicity by DFS from TOP
    instances = {m: 0 for m in file_of_module}

    def visit(mod, mult):
        instances[mod] += mult
        for child, n in children.get(mod, {}).items():
            visit(child, mult * n)

    if TOP not in instances:
        sys.exit(f"top module {TOP} not found in the file list")
    visit(TOP, 1)
    for m, n in INSTANCE_OVERRIDES.items():
        if m in instances:
            instances[m] = n

    print(f"{'module':36s} {'inst':>4s} {'mult sites':>10s} {'total':>6s}")
    total_mult = 0
    for f in files:
        for m in mods_by_file[f]:
            sites = multiply_sites(code_of_module[m])
            n_sites = sum(c for _, c, _ in sites)
            total = n_sites * instances[m]
            total_mult += total
            if n_sites or instances[m] == 0:
                print(f"{m:36s} {instances[m]:4d} {n_sites:10d} {total:6d}")
                for ln, c, src in sites:
                    print(f"    {f}:{ln}  ({c}x)  {src[:70]}")
    print(f"\nTOTAL multiplier sites x instances: {total_mult}  (limit {MAX_MULT})")

    print(f"\n{'memory':52s} {'bits':>9s}")
    total_bits = 0
    ok = True
    for f, arr, w, d, inst, note in RAM_TABLE:
        if not re.search(r"\b" + re.escape(arr) + r"\s*\[", code_by_file.get(f, "")):
            print(f"  ERROR: {arr} not declared in {f} — update RAM_TABLE")
            ok = False
            continue
        bits = w * d * inst
        total_bits += bits
        print(f"{f + ':' + arr + ' (' + note + ')':52s} {bits:9d}")
    print(f"\nTOTAL RAM bits: {total_bits}  ({total_bits / 1024:.1f} kbit, limit {MAX_RAM_BITS // 1024} kbit)")

    if total_mult > MAX_MULT:
        print("FAIL: multiplier budget exceeded")
        ok = False
    if total_bits > MAX_RAM_BITS:
        print("FAIL: RAM budget exceeded")
        ok = False
    print("RESOURCE GATE: " + ("PASS" if ok else "FAIL"))
    sys.exit(0 if ok else 1)


if __name__ == "__main__":
    main()
```

Run: `python3 tb/golden/count_multipliers.py`
Expected: a table with `ddc 1 2 2`, `fir_lowpass 2 4 8`, `fft_engine 2 4 8`, `frequency_matched_filter 1 4 4`, `doppler_processor_optimized 1 8 8` (6 window + 2 constant address products), `cfar_ca 1 5 5`; `TOTAL multiplier sites x instances: 35 (limit 55)`; `TOTAL RAM bits: ~226000 (~221 kbit)`; `RESOURCE GATE: PASS`. If a module shows more sites than listed, open the line numbers it prints: any `*` that is a real variable×variable product counts; a `*` by a constant power of two is still counted (conservative) — document it in the README table rather than changing the script.

- [ ] **Step 2: Add the gates to `run_regression.sh` Phase 0**

After the `run_lint_static "${ALL_RTL[@]}"` call add:

```bash
    # Layer C: vendor primitives / attributes must not appear in pipeline files
    printf "  %-45s " "Vendor primitive / attribute grep gate"
    VENDOR_HITS=$(grep -nE "DSP48E1|xpm_memory|IBUFDS|IDDR|ODDR|MMCME2|PLLE2|BUFG|BUFIO|ASYNC_REG|USE_DSP|use_dsp|ram_style|rom_style|DONT_TOUCH|dont_touch|max_fanout|keep *=" "${PROD_RTL[@]}" || true)
    if [[ -n "$VENDOR_HITS" ]]; then
        echo -e "${RED}FAIL${NC}"
        echo "$VENDOR_HITS" | sed 's/^/    /'
        LINT_ERR=$((LINT_ERR + 1))
    else
        echo -e "${GREEN}PASS${NC}"
    fi

    # Layer D: the pipeline must also compile WITHOUT -DSIMULATION (synthesizable branches)
    printf "  %-45s " "iverilog -Wall (production, no SIMULATION)"
    if iverilog -g2001 -Wall -o /dev/null "${PROD_RTL[@]}" 2>/tmp/iverilog_nosim_$$.log; then
        echo -e "${GREEN}PASS${NC} ($(grep -c . /tmp/iverilog_nosim_$$.log || true) info warnings)"
    else
        echo -e "${RED}COMPILE ERROR${NC}"
        sed 's/^/    /' /tmp/iverilog_nosim_$$.log
        LINT_ERR=$((LINT_ERR + 1))
    fi
    rm -f /tmp/iverilog_nosim_$$.log

    # Layer E: static multiplier / RAM budget
    printf "  %-45s " "Resource budget (<=55 mult, <=1 Mbit RAM)"
    if python3 tb/golden/count_multipliers.py "${PROD_RTL[@]}" >/tmp/resource_$$.log 2>&1; then
        echo -e "${GREEN}PASS${NC} ($(grep 'TOTAL multiplier' /tmp/resource_$$.log | sed 's/.*instances: //'))"
    else
        echo -e "${RED}FAIL${NC}"
        sed 's/^/    /' /tmp/resource_$$.log
        LINT_ERR=$((LINT_ERR + 1))
    fi
    rm -f /tmp/resource_$$.log
```

Also rename the Phase 1 test label `"Doppler Processor (DSP48)"` → `"Doppler Processor"` and update the header comment of the script (`# Phase 0: Vivado-style lint` → `# Phase 0: lint + vendor-neutrality + resource gates`).

Run: `./run_regression.sh 2>&1 | head -16` → the four Layer lines all `PASS`; `tail -4` → `Tests: 33 passed, 0 failed`.

Run the spec's acceptance greps once more by hand:

```bash
grep -lE "DSP48E1|xpm_memory|IBUFDS|IDDR|ODDR|MMCME2|BUFG|BUFIO" $(sed -n '/^PROD_RTL=(/,/^)/p' run_regression.sh | grep '\.v' | tr -d ' ')
```
Expected: no output.

- [ ] **Step 3: Formal files**

`formal/fv_cdc_adc.*` were deleted in Task 2. The remaining `.sby` files reference unchanged modules (`cdc_handshake`, `cdc_single_bit`, `doppler_processor`+`xfft_16`+`fft_engine`, `radar_mode_controller`, `range_bin_decimator`). If `sby` is installed run `cd formal && for f in *.sby; do sby -f "$f" || exit 1; done`; otherwise note in the README that formal was not re-run locally (CI does not run it either). Edit `formal/fv_doppler_processor.sby` `[files]`: replace `../fft_twiddle_1024.mem` with `../fft_twiddle_256.mem` (the default parameter now names that file).

- [ ] **Step 4: CI**

In `.github/workflows/ci-tests.yml` replace lines 71–86 with:

```yaml
  # ===========================================================================
  # FPGA RTL Regression (33 testbenches + lint + vendor-neutrality/resource gates)
  # Golden vectors under 9_Firmware/9_2_FPGA/tb/golden are committed; the
  # generators (numpy) are re-run here to make sure they still reproduce them.
  # ===========================================================================
  fpga-regression:
    name: FPGA Regression
    runs-on: ubuntu-latest

    steps:
      - uses: actions/checkout@v4

      - uses: actions/setup-python@v5
        with:
          python-version: "3.12"

      - uses: astral-sh/setup-uv@v5

      - name: Install dependencies
        run: uv sync --group dev

      - name: Install Icarus Verilog
        run: sudo apt-get update && sudo apt-get install -y iverilog

      - name: Regenerate golden vectors and check they are unchanged
        working-directory: 9_Firmware/9_2_FPGA
        run: |
          uv run python tb/golden/gen_twiddle_rom.py 256
          uv run python tb/golden/gen_ref_spectrum.py
          uv run python tb/golden/gen_ddc_golden.py
          uv run python tb/golden/gen_fir_golden.py
          uv run python tb/golden/gen_fft_golden.py
          uv run python tb/golden/gen_mf_chain_golden.py
          uv run python tb/golden/gen_fullchain_golden.py
          git diff --exit-code -- . ':!tb/golden/golden_doppler.mem'

      - name: Run full FPGA regression
        run: bash run_regression.sh
        working-directory: 9_Firmware/9_2_FPGA
```

(The `golden_doppler.mem` exclusion is needed because the receiver golden is regenerated by the regression itself. If this step ever fails only by ±1 LSB in a `.mem`/`.hex` after a numpy upgrade, pin `numpy` in `pyproject.toml` rather than loosening the check.) Run `uv run ruff check 9_Firmware/9_2_FPGA/tb/golden` locally and fix any lint findings (unused imports, line length ≤ the repo's configured limit).

- [ ] **Step 5: README**

Create `README.md`:

````markdown
# AERIS-10 FPGA pipeline — vendor-neutral RTL (track A)

Synthesizable on Quartus and Vivado with inferred DSP and RAM only. No vendor
primitives, no synthesis attributes (`run_regression.sh` Phase 0 enforces it).

## Signal chain

```
ADC 12-bit CMOS, 100 MSPS, 20 MHz IF (adc_clk = clk_proc)
  |  12 bit offset binary, 1/clk
  v
adc_cmos_interface      sample[11:0], sample_valid, overrange (sticky)
  |  12 bit, 100 MSPS
  v
ddc  (nco 20 MHz -> mixer 12x16 [27:12] -> cic N=5 R=4 (26-bit) -> fir 32-tap folded 4-phase)
  |  16 bit I/Q, 25 MSPS (one sample per 4 clk)
  v
rx_gain_control         host gain shift / AGC
  |  16 bit I/Q, 25 MSPS
  v
matched_filter_multi_segment   256-pt segments, overlap 32 / advance 224, 4 long segments
  +-- matched_filter_processing_chain: fft_engine (256, 24-bit internal) FFT -> x conj(ref_spectrum_rom) -> IFFT
  |  16 bit I/Q, 256 bins per segment
  v
range_bin_decimator     256 -> 64 bins (peak detect)
  v
mti_canceller           2-pulse canceller (RAM history)
  v
doppler_processor       Hamming x dual 16-pt FFT (xfft_16 / fft_engine 24-bit)
  v
cfar_ca -> usb_data_interface_ft2232h (packet format unchanged)
```

Clocks: `clk_100m` **must be the ADC data clock**; `clk_120m_dac` (TX) and the
USB clock cross into it through toggle synchronizers and `cdc_handshake` only.

### Rates and widths at every boundary

| Boundary | Rate | Width | Notes |
|---|---|---|---|
| ADC pins → `adc_cmos_interface` | 100 MSPS | 12 (offset binary) | single edge |
| → mixer | 100 MSPS | 12 signed | MSB inverted |
| mixer product | 100 MSPS | 28 | 12 x 16 (NCO Q15) |
| → CIC | 100 MSPS | 16 | product[27:12], 1 bit headroom |
| CIC integrators/combs | 100 / 25 MSPS | 26 | Hogenauer 5·2 + 16 |
| CIC → FIR | 25 MSPS | 16 | exact >>>10 (gain 1) |
| FIR pre-add / product / acc | 25 MSPS | 17 / 35 / 40 | +5 guard bits |
| FIR → gain → matched filter | 25 MSPS | 16 | acc >>> 17, saturated |
| FFT data RAM | — | 24 | 16 + 8 bits growth (N = 256) |
| FFT twiddles | — | 16 | Q15, quarter-wave ROM |
| conj-multiply out | — | 16 | Q30 rounded >> 15, saturated |
| range profile | 1/clk bursts | 16 | 256 bins per segment |

## Multiplier / RAM budget (static, `tb/golden/count_multipliers.py`)

| Module | Instances | Multipliers (sites) | Width | Notes |
|---|---|---|---|---|
| ddc (mixer) | 1 | 2 | 12 x 16 | |
| fir_lowpass | 2 | 4 each = 8 | 17 x 18 | folded + 4-phase time multiplex |
| fft_engine (range, 256) | 1 | 4 | 24 x 16 | 2 x 18x18 or 1 x 27x27 each |
| fft_engine (Doppler, 16) | 1 | 4 | 24 x 16 | via xfft_16 |
| frequency_matched_filter | 1 | 4 | 16 x 16 | |
| doppler_processor | 1 | 8 counted (2 real) | 16 x 16 | 6 sites in exclusive branches share 2 multipliers; 2 constant x64 address products are shifts |
| cfar_ca | 1 | 5 counted (1–3 real) | 8 x 21, 21 x 5 | GO/SO cross-multiplies by 5-bit counts |
| nco, cic, mti, rest | — | 0 | | |
| **Total** | | **35 counted / 27 real sites** | | limit 55 (≤ 51 as 18x18 equivalents with the 24x16 FFT products counted twice) |

| Memory | Size (bits) |
|---|---|
| ref_spectrum_rom (2 x 1280 x 16) | 40 960 |
| fft_engine range data (2 x 256 x 24) + twiddles | 13 312 |
| matched_filter_processing_chain sig/prod (4 x 256 x 16) | 16 384 |
| matched_filter_multi_segment buffer + overlap | 9 216 |
| doppler_processor frame (2 x 2048 x 16) | 65 536 |
| cfar_ca magnitude map + column (2048 x 17 + 64 x 17) | 35 904 |
| mti_canceller history (2 x 64 x 16) | 2 048 |
| plfm_chirp_controller TX LUTs | 29 280 |
| Doppler engine, xfft_16 buffers, NCO LUT, self-test, status | ~3 900 |
| **Total** | **≈ 216 kbit (limit 1 Mbit)** |

(Exact numbers: run `python3 tb/golden/count_multipliers.py`.)

## Removed modules

| File | Reason |
|---|---|
| `ad9484_interface_400m.v`, `adc_clk_mmcm.v`, `tb/ad9484_interface_400m_stub.v` | 400 MHz LVDS/DDR front end: IBUFDS/IDDR/BUFIO/MMCM primitives; replaced by `adc_cmos_interface.v` |
| `ddc_400m.v`, `nco_400m_enhanced.v` | 400 MHz design with DSP48E1 instances and the Gray-code 400→100 MHz CDC; replaced by `ddc.v`, `nco.v` |
| `cdc_adc_to_processing` (in `cdc_modules.v`) | Gray coding is only safe for ±1 changes; the chirp counter now uses `cdc_handshake` |
| `ddc_input_interface.v` | 18→16-bit rescale no longer needed (FIR outputs 16 bits) |
| `latency_buffer.v` | 4096 x 32 RAM delaying static ROM data; the reference spectrum is addressed by segment instead |
| `chirp_memory_loader_param.v`, `long_chirp_seg*.mem`, `short_chirp_*.mem` | time-domain reference ROMs (95 % zeros); replaced by `ref_spectrum_rom.v` |
| `radar_system_top_50t.v` | Xilinx XC7A50T wrapper with LVDS ADC ports and DONT_TOUCH |
| `tb/tb_ad9484_xsim.v`, `tb/tb_nco_xsim.v`, `tb/tb_ddc_400m.v`, `tb/tb_ddc_cosim.v`, `tb/tb_latency_buffer.v`, `tb/tb_matched_filter_processing_chain.v`, `tb/tb_mf_chain_synth.v`, `tb/tb_mf_cosim.v`, `tb/tb_multiseg_cosim.v`, `formal/fv_cdc_adc.*`, `tb/cosim/{compare,compare_mf,gen_mf_cosim_golden,gen_multiseg_golden,gen_chirp_mem,validate_mem_files}.py` | tests/scripts of the removed modules |

## Verification

`./run_regression.sh` — Phase 0: `iverilog -Wall` with and without `-DSIMULATION`,
case-default lint, vendor-primitive grep gate, resource gate. Then 33 testbenches,
including the synthesizable matched-filter branch (`tb/tb_mf_chain.v` without
`-DSIMULATION`, bit-exact against `tb/golden/gen_mf_chain_golden.py`) and the
golden tests in `tb/golden/`: (a) DDC bit-exact, (b) folded FIR == direct form,
(c) 256-pt FFT vs numpy (|err| ≤ 256, RMS ≤ 32), (d) full-chain range-peak
displacement. Generators: `tb/golden/gen_*.py` (numpy); `tb/cosim/fpga_model.py`
provides the bit-accurate FFT / conjugate-multiply models they reuse.
`tb/cosim/radar_scene.py`, `gen_doppler_golden.py` and the `real_data/` vectors
still serve the unchanged Doppler path.

## Known limitations carried over from the upstream design

- `fft_engine` has no per-stage scaling: forward-FFT outputs saturate at 16 bits
  when the input exceeds roughly 1/16 full scale. Use `host_gain_shift`
  (attenuate) / AGC so the matched-filter input stays small.
- Matched-filter throughput: 4 segments x (2 x ~4.6 k + ~0.6 k) ≈ 42 k clocks per
  long chirp at 100 MHz, i.e. ~420 µs vs. the 167 µs PRI assumed in the BOM report.
  The segmenter drops chirp starts while busy (as before). A pipelined
  butterfly (one per clock) would bring this under the PRI.
- Overlap-save alignment between signal segments and reference segments is now
  consistent (both advance 224 samples) but has not been validated on real data.

## Hand-off: what remains board-specific

- Top-level wrapper for the Cyclone dev board: pin assignments, I/O standards,
  `clk_100m` driven by the ADC DCO (or a PLL locked to it), the 120 MHz DAC
  clock and clock forwarding to the DAC/FT232H pins (`dac_clk`, `ft601_clk_out`
  are plain signals here).
- PLL/reset sequencing (`reset_n` is an asynchronous active-low input).
- Timing constraints (SDC/XDC). The `constraints/` and `scripts/` directories
  are the old Xilinx flows and are stale (they reference removed tops/ports).
- Formal (`formal/*.sby`) is not part of CI; rerun with `sby` when available.
````

- [ ] **Step 6: Final regression, placeholder scan, commit**

Run:

```bash
./run_regression.sh 2>&1 | tail -6
grep -rnE "TODO|TBD|FIXME" README.md tb/golden/*.py adc_cmos_interface.v ddc.v nco.v fir_lowpass.v ref_spectrum_rom.v matched_filter_*.v mti_canceller.v || echo "no placeholders"
git status --short | head
```
Expected: `Tests: 33 passed, 0 failed`, `no placeholders`.

```bash
git add -A 9_Firmware/9_2_FPGA .github/workflows/ci-tests.yml
git commit -m "fpga: resource/vendor gates in regression, CI golden regeneration check, README for the vendor-neutral pipeline

Co-Authored-By: Claude Fable 5.1 <noreply@anthropic.com>"
```

---

## Self-review against the spec

| Spec item | Task |
|---|---|
| R1 no primitives / attributes in pipeline files; inferred multipliers; inferable RAM; lint gate | T2 (scrub), T4 (nco), T5 (cic), T6 (ddc), T7 (fir), T8 (fft RAM), T10 (chain RAM), T11 (mti RAM), T12 (grep gate + no-SIM lint) |
| R2 delete `ad9484_interface_400m.v`, `adc_clk_mmcm.v`; `adc_cmos_interface.v` with `DATA_W`, `adc_clk`, `adc_data`, `adc_ovr`, `sample`, `sample_valid`, sticky `overrange`; `clk_proc = adc_clk`; Gray CDC deleted | T3, T6 (chirp-counter CDC moved to handshake in T2) |
| R3 NCO rename + `PHASE_INC` 0x3333_3333, 32-bit accumulator, quarter-wave LUT, 16-bit out | T4 |
| R3 mixer 12 x 16 single multiplier per I/Q, truncation documented | T6 (`ddc.v` header) |
| R3 CIC 5 stages R = 4 M = 1, Hogenauer widths, `+` on signed regs | T5 |
| R3 FIR folded, accumulator guard bits, coefficients unchanged, ≤ 32 multipliers | T7 (8 multipliers) |
| R3 second ×4 decimator | **not implemented — see Decision 1** (CIC already yields 25 MSPS) |
| R4 `N_FFT` parameter default 256, segment arithmetic documented | T10 (`matched_filter_multi_segment.v` header) |
| R4 `INTERNAL_W` default 24, twiddles 16 bits, ≤ 4 multipliers per engine, choice documented | T8 (4 real multiplies) |
| R4 reference spectrum ROM generated by `tb/golden/gen_ref_spectrum.py`; reference FFT pass removed | T9, T10 |
| R4 delete `latency_buffer.v`; ROM addressed by offset | T10 |
| R4 short-chirp ROM ≤ 64 entries | vacuous — no time-domain ROM remains (Decision 5) |
| R4 range-bin decimator 256 → 64 | T10 |
| R5 MTI RAM inference; dead debug counters removed; CFAR header | T11, T5/T6 (CIC monitors, DDC counters), T2 (CFAR) |
| R6 regression passes; obsolete tests deleted with entries; synth MF path compiled without `-DSIMULATION`; golden tests (a)–(d) as Verilog TBs reading generated `.hex`; formal updated; CI | every task; T10 (`run_test_nosim`); T6/T7/T8/T10; T12 |
| R7 README with diagram, budget table, removed modules, hand-off | T12 |
| Acceptance greps and ≤ 55 / ≤ 1 Mbit | T12 (Layer C/E) |
