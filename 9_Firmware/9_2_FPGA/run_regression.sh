#!/bin/bash
# ===========================================================================
# FPGA Regression Test Runner for AERIS-10 Radar
# Phase 0: lint + vendor-neutrality + resource gates (catches issues iverilog silently accepts)
# Phase 1+: Compile and run all verified iverilog testbenches
#
# Usage:  ./run_regression.sh [--quick] [--skip-lint]
#   --quick      Skip long-running integration tests (receiver golden, system TB)
#   --skip-lint  Skip Phase 0 lint checks (not recommended)
#
# Exit code: 0 if all tests pass, 1 if any fail
# ===========================================================================

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
cd "$SCRIPT_DIR"

QUICK=0
SKIP_LINT=0
for arg in "$@"; do
    case "$arg" in
        --quick) QUICK=1 ;;
        --skip-lint) SKIP_LINT=1 ;;
    esac
done

# GNU timeout: `timeout` on Linux, `gtimeout` from coreutils on macOS; run unguarded if neither exists
if command -v timeout >/dev/null 2>&1; then
    TIMEOUT_CMD="timeout"
elif command -v gtimeout >/dev/null 2>&1; then
    TIMEOUT_CMD="gtimeout"
else
    TIMEOUT_CMD=""
    echo "WARNING: no 'timeout'/'gtimeout' found — simulations run without a time limit" >&2
fi

PASS=0
FAIL=0
SKIP=0
LINT_WARN=0
LINT_ERR=0
ERRORS=""

# Colors (if terminal supports it)
RED='\033[0;31m'
GREEN='\033[0;32m'
YELLOW='\033[0;33m'
CYAN='\033[0;36m'
NC='\033[0m' # No Color

# ===========================================================================
# PHASE 0: VIVADO-STYLE LINT
# Two layers:
#   (A) iverilog -Wall full-design compile — parse for serious warnings
#   (B) Custom regex checks for patterns Vivado treats as errors
# ===========================================================================

# Production RTL file list (same as system TB minus testbench files)
PROD_RTL=(
    radar_system_top.v
    radar_transmitter.v
    dac_interface_single.v
    plfm_chirp_controller.v
    radar_receiver_final.v
    adc_cmos_interface.v
    ddc.v
    nco.v
    cic_decimator_4x_enhanced.v
    cdc_modules.v
    fir_lowpass.v
    matched_filter_multi_segment.v
    matched_filter_processing_chain.v
    frequency_matched_filter.v
    ref_spectrum_rom.v
    range_bin_decimator.v
    doppler_processor.v
    xfft_16.v
    fft_engine.v
    usb_data_interface.v
    usb_data_interface_ft2232h.v
    edge_detector.v
    radar_mode_controller.v
    rx_gain_control.v
    cfar_ca.v
    mti_canceller.v
    fpga_self_test.v
)

# ---------------------------------------------------------------------------
# Shared RTL file lists for integration / system tests
# Centralised here so a new module only needs adding once.
# ---------------------------------------------------------------------------

# Receiver chain (used by golden generate/compare tests)
RECEIVER_RTL=(
    radar_receiver_final.v
    radar_mode_controller.v
    adc_cmos_interface.v ddc.v nco.v cic_decimator_4x_enhanced.v
    cdc_modules.v fir_lowpass.v
    matched_filter_multi_segment.v matched_filter_processing_chain.v
    frequency_matched_filter.v ref_spectrum_rom.v
    range_bin_decimator.v doppler_processor.v xfft_16.v fft_engine.v
    rx_gain_control.v mti_canceller.v
)

# Full system top (receiver chain + TX + USB + detection + self-test)
SYSTEM_RTL=(
    radar_system_top.v
    radar_transmitter.v dac_interface_single.v plfm_chirp_controller.v
    "${RECEIVER_RTL[@]}"
    usb_data_interface.v usb_data_interface_ft2232h.v edge_detector.v
    cfar_ca.v fpga_self_test.v
)

# ---- Layer A: iverilog -Wall compilation ----
run_lint_iverilog() {
    local label="$1"
    shift
    local files=("$@")
    local warn_file="/tmp/iverilog_lint_$$_${label}.log"

    printf "  %-45s " "iverilog -Wall ($label)"

    if ! iverilog -g2001 -DSIMULATION -Wall -o /dev/null "${files[@]}" 2>"$warn_file"; then
        # Hard compile error — always fatal
        echo -e "${RED}COMPILE ERROR${NC}"
        while IFS= read -r line; do
            echo "    $line"
        done < "$warn_file"
        LINT_ERR=$((LINT_ERR + 1))
        rm -f "$warn_file"
        return 1
    fi

    # Parse warnings — classify as error-level or info-level
    local err_count=0
    local info_count=0
    local err_lines=""

    while IFS= read -r line; do
        # Part-select out of range — Vivado Synth 8-524 (ERROR in Vivado)
        if echo "$line" | grep -q 'Part select.*is selecting after the vector\|out of bound bits'; then
            err_count=$((err_count + 1))
            err_lines="$err_lines\n    ${RED}[VIVADO-ERR]${NC} $line"
        # Port width mismatch / connection mismatch
        elif echo "$line" | grep -q 'port.*does not match\|Port.*mismatch'; then
            err_count=$((err_count + 1))
            err_lines="$err_lines\n    ${RED}[VIVADO-ERR]${NC} $line"
        # Informational warnings (timescale, dangling ports, array sensitivity)
        elif echo "$line" | grep -q 'timescale\|dangling\|sensitive to all'; then
            info_count=$((info_count + 1))
        # Unknown warning — report but don't fail
        elif [[ -n "$line" ]]; then
            info_count=$((info_count + 1))
        fi
    done < "$warn_file"

    if [[ "$err_count" -gt 0 ]]; then
        echo -e "${RED}FAIL${NC} ($err_count Vivado-class errors, $info_count info)"
        echo -e "$err_lines"
        LINT_ERR=$((LINT_ERR + err_count))
    else
        echo -e "${GREEN}PASS${NC} ($info_count info warnings)"
    fi

    rm -f "$warn_file"
}

# ---- Layer B: Custom regex static checks ----
# Catches patterns that Vivado treats as errors/warnings but iverilog ignores
run_lint_static() {
    printf "  %-45s " "Static RTL checks"

    local err_count=0
    local warn_count=0
    local err_lines=""
    local warn_lines=""

    for f in "$@"; do
        [[ -f "$f" ]] || continue
        # Skip testbench files (tb/ directory) — only lint production RTL
        case "$f" in tb/*) continue ;; esac

        local linenum=0
        while IFS= read -r line; do
            linenum=$((linenum + 1))

            # --- CHECK 1: Part-select with literal range on reg ---
            # Pattern: identifier[N:M] where N exceeds declared width
            # (iverilog catches this, but belt-and-suspenders)

            # --- CHECK 2: case/casex/casez without default (non-full case) ---
            # Vivado SYNTH-6 / inferred latch warning
            # Heuristic: look for case/casex/casez, then check if 'default' appears
            # before the matching 'endcase'. This is approximate — full parsing
            # would need a real parser. We flag 'case' lines so the developer
            # can manually verify.
            # (Handled below as a multi-line check)

            # --- CHECK 3: Blocking assignment (=) inside always @(posedge ...) ---
            # Vivado SYNTH-5 warning for inferred latches / race conditions
            # Only flag if the always block is clocked (posedge/negedge)
            # This is a heuristic — we check for '= ' that isn't '<=', '==', '!='
            # inside an always block header containing 'posedge' or 'negedge'.
            # (Too complex for line-by-line — skip for now, handled by testbenches)

            # --- CHECK 4: Multi-driven register (assign + always on same signal) ---
            # (Would need cross-file analysis — skip for v1)

        done < "$f"
    done

    # --- Multi-line check: case without default ---
    for f in "$@"; do
        [[ -f "$f" ]] || continue
        case "$f" in tb/*) continue ;; esac

        # Find case blocks and check for default
        # Use awk to find case..endcase blocks missing 'default'
        local missing_defaults
        missing_defaults=$(awk '
            /^[[:space:]]*(case|casex|casez)[[:space:]]*\(/ {
                case_line = NR
                case_file = FILENAME
                has_default = 0
                in_case = 1
                next
            }
            in_case && /default[[:space:]]*:/ {
                has_default = 1
            }
            in_case && /endcase/ {
                if (!has_default) {
                    printf "%s:%d: case statement without default\n", FILENAME, case_line
                }
                in_case = 0
            }
        ' "$f" 2>/dev/null)

        if [[ -n "$missing_defaults" ]]; then
            while IFS= read -r hit; do
                warn_count=$((warn_count + 1))
                warn_lines="$warn_lines\n    ${YELLOW}[SYNTH-6]${NC} $hit"
            done <<< "$missing_defaults"
        fi
    done

    # CHECK 5 ($readmemh in synth code) and CHECK 6 (unused includes)
    # require multi-line ifdef tracking / cross-file analysis. Not feasible
    # with line-by-line regex. Omitted — use Vivado lint instead.

    if [[ "$err_count" -gt 0 ]]; then
        echo -e "${RED}FAIL${NC} ($err_count errors, $warn_count warnings)"
        echo -e "$err_lines"
        LINT_ERR=$((LINT_ERR + err_count))
    elif [[ "$warn_count" -gt 0 ]]; then
        echo -e "${YELLOW}WARN${NC} ($warn_count warnings)"
        echo -e "$warn_lines"
        LINT_WARN=$((LINT_WARN + warn_count))
    else
        echo -e "${GREEN}PASS${NC}"
    fi
}

# ---------------------------------------------------------------------------
# Helper: classify a testbench's simulation output
#   evaluate_output <name> <output>
# Rules (no "it reached \$finish" fallback -- vvp always prints that):
#   * [PASS...] / [FAIL...] markers are counted at any indentation.
#   * Any [FAIL...] marker, or a failure summary such as "SOME TESTS FAILED",
#     "3 TESTS FAILED", "2 TEST(S) FAILED", "1 PASSED, 2 FAILED", fails the test.
#     (Per-count lines like "FAILED: 0 / 32" are not failure summaries.)
#   * Otherwise the test passes only if it printed at least one [PASS...]
#     marker or an explicit "ALL [N] TESTS PASSED" line; anything else is
#     reported as UNKNOWN and counted as a failure.
# ---------------------------------------------------------------------------
evaluate_output() {
    local name="$1"
    local output="$2"
    local test_pass test_fail fail_text success_text
    test_pass=$(echo "$output" | grep -Ec '^[[:space:]]*\[PASS[^]]*\]' || true)
    test_fail=$(echo "$output" | grep -Ec '^[[:space:]]*\[FAIL[^]]*\]' || true)
    fail_text=$(echo "$output" | grep -Ec 'SOME TESTS FAILED|TESTS FAILED|TEST\(S\) FAILED|[1-9][0-9]* FAILED' || true)
    success_text=$(echo "$output" | grep -Ec 'ALL ([0-9]+ )?TESTS PASSED' || true)

    if [[ "$test_fail" -gt 0 || "$fail_text" -gt 0 ]]; then
        echo -e "${RED}FAIL${NC} (pass=$test_pass, fail=$test_fail, failure summary lines=$fail_text)"
        ERRORS="$ERRORS\n  $name: $test_fail [FAIL] marker(s), $fail_text failure summary line(s)"
        FAIL=$((FAIL + 1))
    elif [[ "$test_pass" -gt 0 ]]; then
        echo -e "${GREEN}PASS${NC} ($test_pass checks)"
        PASS=$((PASS + 1))
    elif [[ "$success_text" -gt 0 ]]; then
        echo -e "${GREEN}PASS${NC} (explicit ALL TESTS PASSED)"
        PASS=$((PASS + 1))
    else
        echo -e "${YELLOW}UNKNOWN${NC} (no PASS/FAIL markers, no ALL TESTS PASSED line)"
        ERRORS="$ERRORS\n  $name: no pass/fail markers in output"
        FAIL=$((FAIL + 1))
    fi
}

# ---------------------------------------------------------------------------
# Helper: compile and run a single testbench
#   run_test <name> <vvp_path> <iverilog_args...>
# ---------------------------------------------------------------------------
run_test() {
    local name="$1"
    local vvp="$2"
    shift 2
    local args=("$@")

    printf "  %-45s " "$name"

    # Compile
    if ! iverilog -g2001 -DSIMULATION -o "$vvp" "${args[@]}" 2>/tmp/iverilog_err_$$; then
        echo -e "${RED}COMPILE FAIL${NC}"
        ERRORS="$ERRORS\n  $name: compile error ($(head -1 /tmp/iverilog_err_$$))"
        FAIL=$((FAIL + 1))
        return
    fi

    # Run
    local output
    output=$(${TIMEOUT_CMD:+$TIMEOUT_CMD 120} vvp "$vvp" 2>&1) || true

    evaluate_output "$name" "$output"
    rm -f "$vvp"
}

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
    output=$(${TIMEOUT_CMD:+$TIMEOUT_CMD 300} vvp "$vvp" 2>&1) || true
    evaluate_output "$name" "$output"
    rm -f "$vvp"
}

# ===========================================================================
echo "============================================"
echo "  AERIS-10 FPGA Regression Test Suite"
echo "============================================"
echo ""
echo "Date: $(date)"
echo "iverilog: $(iverilog -V 2>&1 | head -1)"
echo ""

# ===========================================================================
# PHASE 0: LINT, vendor-neutrality and resource gates
# ===========================================================================
if [[ "$SKIP_LINT" -eq 0 ]]; then
    echo "--- PHASE 0: LINT, vendor-neutrality and resource gates ---"

    # Layer A: iverilog -Wall on full production design
    run_lint_iverilog "production" "${PROD_RTL[@]}"

    # Layer B: custom static regex checks
    ALL_RTL=("${PROD_RTL[@]}")
    run_lint_static "${ALL_RTL[@]}"

    # Layer C: vendor primitives / synthesis attributes must not appear in pipeline files
    printf "  %-45s " "Vendor primitive / attribute grep gate"
    VENDOR_HITS=$(grep -nE "DSP48E1|xpm_memory|IBUFDS|IDDR|ODDR|MMCME2|PLLE2|BUFG|BUFIO|ASYNC_REG|USE_DSP|use_dsp|ram_style|rom_style|DONT_TOUCH|dont_touch|max_fanout|keep *=|\(\*[[:space:]]*[A-Za-z]" "${PROD_RTL[@]}" || true)
    if [[ -n "$VENDOR_HITS" ]]; then
        echo -e "${RED}FAIL${NC}"
        echo "$VENDOR_HITS" | sed 's/^/    /'
        LINT_ERR=$((LINT_ERR + 1))
    else
        echo -e "${GREEN}PASS${NC}"
    fi

    # Layer D: the pipeline must also compile WITHOUT -DSIMULATION (synthesizable branches)
    printf "  %-45s " "iverilog -Wall (production, no SIMULATION)"
    NOSIM_LOG="/tmp/iverilog_nosim_$$.log"
    if iverilog -g2001 -Wall -o /dev/null "${PROD_RTL[@]}" 2>"$NOSIM_LOG"; then
        echo -e "${GREEN}PASS${NC} ($(grep -c . "$NOSIM_LOG" || true) info warnings)"
    else
        echo -e "${RED}COMPILE ERROR${NC}"
        sed 's/^/    /' "$NOSIM_LOG"
        LINT_ERR=$((LINT_ERR + 1))
    fi
    rm -f "$NOSIM_LOG"

    # Layer E: static multiplier / RAM budget (<=55 multipliers 18x18-equivalent, <=1 Mbit)
    printf "  %-45s " "Resource budget (<=55 mult, <=1 Mbit RAM)"
    RES_LOG="/tmp/resource_$$.log"
    if python3 tb/golden/count_multipliers.py "${PROD_RTL[@]}" >"$RES_LOG" 2>&1; then
        echo -e "${GREEN}PASS${NC} ($(grep 'TOTAL multipliers' "$RES_LOG" | sed 's/.*equivalents: //'); $(grep -o 'TOTAL RAM+ROM bits: [0-9]*' "$RES_LOG"))"
    else
        echo -e "${RED}FAIL${NC}"
        sed 's/^/    /' "$RES_LOG"
        LINT_ERR=$((LINT_ERR + 1))
    fi
    rm -f "$RES_LOG"

    echo ""
    if [[ "$LINT_ERR" -gt 0 ]]; then
        echo -e "${RED}  LINT FAILED: $LINT_ERR Vivado-class error(s) detected.${NC}"
        echo "  Fix lint errors before pushing to Vivado. Aborting regression."
        echo ""
        exit 1
    elif [[ "$LINT_WARN" -gt 0 ]]; then
        echo -e "${YELLOW}  LINT: $LINT_WARN advisory warning(s) (non-blocking)${NC}"
    else
        echo -e "${GREEN}  LINT: All checks passed${NC}"
    fi
    echo ""
else
    echo "--- PHASE 0: LINT (skipped via --skip-lint) ---"
    echo ""
fi

# ===========================================================================
# PHASE 1: UNIT TESTS — Changed Modules (HIGH PRIORITY)
# ===========================================================================
echo "--- PHASE 1: Changed Modules ---"

run_test "CIC Decimator" \
    tb/tb_cic_reg.vvp \
    tb/tb_cic_decimator.v cic_decimator_4x_enhanced.v

run_test "Chirp Controller (BRAM)" \
    tb/tb_chirp_reg.vvp \
    tb/tb_chirp_controller.v plfm_chirp_controller.v

run_test "Chirp Contract" \
    tb/tb_chirp_ctr_reg.vvp \
    tb/tb_chirp_contract.v plfm_chirp_controller.v

run_test "Doppler Processor" \
    tb/tb_doppler_reg.vvp \
    tb/tb_doppler_cosim.v doppler_processor.v xfft_16.v fft_engine.v

run_test "Threshold Detector (detection bugs)" \
    tb/tb_threshold_detector.vvp \
    tb/tb_threshold_detector.v

run_test "RX Gain Control (digital gain)" \
    tb/tb_rx_gain_control.vvp \
    tb/tb_rx_gain_control.v rx_gain_control.v

run_test "MTI Canceller (ground clutter)" \
    tb/tb_mti_canceller.vvp \
    tb/tb_mti_canceller.v mti_canceller.v

run_test "CFAR CA Detector" \
    tb/tb_cfar_ca.vvp \
    tb/tb_cfar_ca.v cfar_ca.v

run_test "FPGA Self-Test" \
    tb/tb_fpga_self_test.vvp \
    tb/tb_fpga_self_test.v fpga_self_test.v

echo ""

# ===========================================================================
# PHASE 2: INTEGRATION TESTS
# ===========================================================================
echo "--- PHASE 2: Integration Tests ---"

run_test "DDC golden (a): NCO+mixer+CIC+FIR bit-exact" \
    tb/tb_ddc_golden_reg.vvp \
    tb/golden/tb_ddc_golden.v ddc.v nco.v cic_decimator_4x_enhanced.v fir_lowpass.v

# Real-data co-simulation: committed golden hex vs RTL (exact match required).
# These catch architecture mismatches (e.g. 32-pt → dual 16-pt Doppler FFT)
# that self-blessing golden-generate/compare tests cannot detect.
run_test "Doppler Real-Data (ADI CN0566, exact match)" \
    tb/tb_doppler_realdata.vvp \
    tb/tb_doppler_realdata.v doppler_processor.v xfft_16.v fft_engine.v

run_test "Full-Chain Real-Data (decim→Doppler, exact match)" \
    tb/tb_fullchain_realdata.vvp \
    tb/tb_fullchain_realdata.v range_bin_decimator.v \
    doppler_processor.v xfft_16.v fft_engine.v

if [[ "$QUICK" -eq 0 ]]; then
    # Golden generate
    run_test "Receiver (golden generate)" \
        tb/tb_rx_golden_reg.vvp \
        -DGOLDEN_GENERATE \
        tb/tb_radar_receiver_final.v "${RECEIVER_RTL[@]}"

    # Golden compare
    run_test "Receiver (golden compare)" \
        tb/tb_rx_compare_reg.vvp \
        tb/tb_radar_receiver_final.v "${RECEIVER_RTL[@]}"

    # Golden (d): ADC -> DDC -> matched filter, range-peak displacement
    run_test "Full-chain golden (d): range peak displacement" \
        tb/tb_fullchain_golden_reg.vvp \
        tb/golden/tb_fullchain_golden.v "${RECEIVER_RTL[@]}"

    run_test_nosim "Full-chain golden (d), synthesizable chain" \
        tb/tb_fullchain_golden_syn_reg.vvp \
        tb/golden/tb_fullchain_golden.v "${RECEIVER_RTL[@]}"

    # Full system top (monitoring-only, legacy)
    run_test "System Top (radar_system_tb)" \
        tb/tb_system_reg.vvp \
        tb/radar_system_tb.v "${SYSTEM_RTL[@]}"

    # E2E integration (46 strict checks: TX, RX, USB R/W, CDC, safety, reset)
    run_test "System E2E (tb_system_e2e)" \
        tb/tb_system_e2e_reg.vvp \
        tb/tb_system_e2e.v "${SYSTEM_RTL[@]}"

    # USB_MODE=1 (FT2232H production) variants of system tests
    run_test "System Top USB_MODE=1 (FT2232H)" \
        tb/tb_system_ft2232h_reg.vvp \
        -DUSB_MODE_1 \
        tb/radar_system_tb.v "${SYSTEM_RTL[@]}"

    run_test "System E2E USB_MODE=1 (FT2232H)" \
        tb/tb_system_e2e_ft2232h_reg.vvp \
        -DUSB_MODE_1 \
        tb/tb_system_e2e.v "${SYSTEM_RTL[@]}"
else
    echo "  (skipped receiver golden + system top + E2E — use without --quick)"
    SKIP=$((SKIP + 8))
fi

echo ""

# ===========================================================================
# PHASE 3: UNIT TESTS — Signal Processing
# ===========================================================================
echo "--- PHASE 3: Signal Processing ---"

run_test "FFT Engine" \
    tb/tb_fft_reg.vvp \
    tb/tb_fft_engine.v fft_engine.v

run_test "FFT golden (c): 256-pt engine vs numpy" \
    tb/tb_fft256_reg.vvp \
    tb/golden/tb_fft256_golden.v fft_engine.v

run_test "NCO (20 MHz IF, inferred accumulator)" \
    tb/tb_nco_reg.vvp \
    tb/tb_nco.v nco.v

run_test "FIR Lowpass (folded, 4-phase)" \
    tb/tb_fir_reg.vvp \
    tb/tb_fir_lowpass.v fir_lowpass.v

run_test "FIR golden (b): folded == direct form" \
    tb/tb_fir_golden_reg.vvp \
    tb/golden/tb_fir_golden.v fir_lowpass.v

run_test "Matched Filter Chain (behavioral branch)" \
    tb/tb_mf_beh_reg.vvp \
    tb/tb_mf_chain.v matched_filter_processing_chain.v fft_engine.v ref_spectrum_rom.v frequency_matched_filter.v

run_test_nosim "Matched Filter Chain (synthesizable, no -DSIMULATION)" \
    tb/tb_mf_syn_reg.vvp \
    tb/tb_mf_chain.v matched_filter_processing_chain.v fft_engine.v ref_spectrum_rom.v frequency_matched_filter.v

run_test "Matched Filter Segmenter (overlap-save stream)" \
    tb/tb_mf_seg_reg.vvp \
    tb/tb_mf_segmenter.v matched_filter_multi_segment.v matched_filter_processing_chain.v \
    fft_engine.v ref_spectrum_rom.v frequency_matched_filter.v

run_test_nosim "Matched Filter Segmenter (synthesizable chain)" \
    tb/tb_mf_seg_syn_reg.vvp \
    tb/tb_mf_segmenter.v matched_filter_multi_segment.v matched_filter_processing_chain.v \
    fft_engine.v ref_spectrum_rom.v frequency_matched_filter.v

run_test "Matched Filter 4 segments, continuous stream (behavioral)" \
    tb/tb_mf_ms_beh_reg.vvp \
    tb/tb_mf_multiseg.v matched_filter_multi_segment.v matched_filter_processing_chain.v \
    fft_engine.v ref_spectrum_rom.v frequency_matched_filter.v

run_test_nosim "Matched Filter 4 segments, continuous stream (synth)" \
    tb/tb_mf_ms_syn_reg.vvp \
    tb/tb_mf_multiseg.v matched_filter_multi_segment.v matched_filter_processing_chain.v \
    fft_engine.v ref_spectrum_rom.v frequency_matched_filter.v

run_test "Reference spectrum ROM" \
    tb/tb_ref_rom_reg.vvp \
    tb/tb_ref_spectrum_rom.v ref_spectrum_rom.v

echo ""

# ===========================================================================
# PHASE 4: UNIT TESTS — Infrastructure
# ===========================================================================
echo "--- PHASE 4: Infrastructure ---"

run_test "CDC Modules (single-bit + handshake)" \
    tb/tb_cdc_reg.vvp \
    tb/tb_cdc_modules.v cdc_modules.v

run_test "Edge Detector" \
    tb/tb_edge_reg.vvp \
    tb/tb_edge_detector.v edge_detector.v

run_test "USB Data Interface" \
    tb/tb_usb_reg.vvp \
    tb/tb_usb_data_interface.v usb_data_interface.v

run_test "Range Bin Decimator" \
    tb/tb_rbd_reg.vvp \
    tb/tb_range_bin_decimator.v range_bin_decimator.v

run_test "Radar Mode Controller" \
    tb/tb_rmc_reg.vvp \
    tb/tb_radar_mode_controller.v radar_mode_controller.v

run_test "ADC CMOS Interface" \
    tb/tb_adc_reg.vvp \
    tb/tb_adc_cmos_interface.v adc_cmos_interface.v

echo ""

# ===========================================================================
# SUMMARY
# ===========================================================================
TOTAL=$((PASS + FAIL + SKIP))
echo "============================================"
echo "  RESULTS"
echo "============================================"
if [[ "$SKIP_LINT" -eq 0 ]]; then
    if [[ "$LINT_ERR" -gt 0 ]]; then
        echo -e "  Lint:  ${RED}$LINT_ERR error(s)${NC}, $LINT_WARN warning(s)"
    elif [[ "$LINT_WARN" -gt 0 ]]; then
        echo -e "  Lint:  ${GREEN}0 errors${NC}, ${YELLOW}$LINT_WARN warning(s)${NC}"
    else
        echo -e "  Lint:  ${GREEN}clean${NC}"
    fi
fi
echo "  Tests: $PASS passed, $FAIL failed, $SKIP skipped / $TOTAL total"
echo "============================================"

if [[ -n "$ERRORS" ]]; then
    echo ""
    echo "Failures:"
    echo -e "$ERRORS"
fi

echo ""

# Exit with error if any failures
if [[ "$FAIL" -gt 0 ]]; then
    exit 1
fi

exit 0
