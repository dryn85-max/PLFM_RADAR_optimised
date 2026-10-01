`timescale 1ns / 1ps

module radar_receiver_final (
    input wire clk,           // processing clock = ADC data clock (nominal 100 MHz)
    input wire reset_n,

    // ADC CMOS parallel interface (12-bit offset binary, one sample per clk)
    input wire [11:0] adc_data,
    input wire        adc_ovr,            // ADC over-range pin
    output wire       adc_pwdn,
    output wire       adc_overrange,      // sticky, cleared at every Doppler frame end
    
    // Chirp counter from transmitter (for matched filter indexing)
    input wire [5:0] chirp_counter,
    // Frame-start pulse from transmitter (CDC-synchronized, 1 clk_100m cycle)
    input wire tx_frame_start,
    
    output wire [31:0] doppler_output,
    output wire doppler_valid,
    output wire [4:0] doppler_bin,
    output wire [5:0] range_bin,
    
    // Matched filter range profile output (for USB)
    output wire signed [15:0] range_profile_i_out,
    output wire signed [15:0] range_profile_q_out,
    output wire range_profile_valid_out,
    
    // Host command inputs (Gap 4: USB Read Path, CDC-synchronized)
    // CDC-synchronized in radar_system_top.v before reaching here
    input wire [1:0] host_mode,      // Radar mode: 00=STM32, 01=auto-scan, 10=single-chirp
    input wire host_trigger,          // Single-chirp trigger pulse (1 clk cycle)

    // Gap 2: Host-configurable chirp timing (CDC-synchronized in radar_system_top.v)
    input wire [15:0] host_long_chirp_cycles,
    input wire [15:0] host_long_listen_cycles,
    input wire [15:0] host_guard_cycles,
    input wire [15:0] host_short_chirp_cycles,
    input wire [15:0] host_short_listen_cycles,
    input wire [5:0]  host_chirps_per_elev,

    // Digital gain control (Fix 3: between DDC output and matched filter)
    // [3]=direction: 0=amplify(left shift), 1=attenuate(right shift)
    // [2:0]=shift amount: 0..7 bits. Default 0 = pass-through.
    input wire [3:0] host_gain_shift,

    // AGC configuration (opcodes 0x28-0x2C, active only when agc_enable=1)
    input wire        host_agc_enable,      // 0x28: 0=manual, 1=auto AGC
    input wire [7:0]  host_agc_target,      // 0x29: target peak magnitude
    input wire [3:0]  host_agc_attack,      // 0x2A: gain-down step on clipping
    input wire [3:0]  host_agc_decay,       // 0x2B: gain-up step when weak
    input wire [3:0]  host_agc_holdoff,     // 0x2C: frames before gain-up

    // STM32 toggle signals for mode 00 (STM32-driven) pass-through.
    // These are CDC-synchronized in radar_system_top.v / radar_transmitter.v
    // before reaching this module. In mode 00, the RX mode controller uses
    // these to synchronize receiver processing with STM32-timed chirps.
    input wire stm32_new_chirp_rx,
    input wire stm32_new_elevation_rx,
    input wire stm32_new_azimuth_rx,

    // CFAR integration: expose Doppler frame_complete to top level
    output wire doppler_frame_done_out,

    // Ground clutter removal controls
    input wire        host_mti_enable,       // 1=MTI active, 0=pass-through
    input wire [2:0]  host_dc_notch_width,   // DC notch: zero Doppler bins within ±width of DC

    // ADC raw data tap (clk_100m domain, post-DDC, for self-test / debug)
    output wire [15:0] dbg_adc_i,            // DDC output I (16-bit signed, 25 MSPS)
    output wire [15:0] dbg_adc_q,            // DDC output Q (16-bit signed, 25 MSPS)
    output wire        dbg_adc_valid,        // DDC output valid (25 MSPS strobe)

    // AGC status outputs (for status readback / STM32 outer loop)
    output wire [7:0]  agc_saturation_count, // Per-frame clipped sample count
    output wire [7:0]  agc_peak_magnitude,   // Per-frame peak (upper 8 bits)
    output wire [3:0]  agc_current_gain      // Effective gain_shift encoding
);

// ========== INTERNAL SIGNALS ==========
wire use_long_chirp;
// NOTE: chirp_counter is now an input port (was undriven internal wire — bug NEW-1)
wire chirp_start;
wire azimuth_change;
wire elevation_change;

// Mode controller outputs → matched_filter_multi_segment
wire mc_new_chirp;
wire mc_new_elevation;
wire mc_new_azimuth;

wire [15:0] adc_i_scaled, adc_q_scaled;
wire adc_valid_sync;

// Gain-controlled signals (between DDC output and matched filter)
wire signed [15:0] gc_i, gc_q;
wire gc_valid;
wire [7:0] gc_saturation_count;  // Diagnostic: per-frame clipped sample counter
wire [7:0] gc_peak_magnitude;    // Diagnostic: per-frame peak magnitude
wire [3:0] gc_current_gain;      // Diagnostic: effective gain_shift

// ========== DOPPLER PROCESSING SIGNALS ==========
wire [31:0] range_data_32bit;
wire range_data_valid;
wire new_chirp_frame;

// Doppler processor outputs
wire [31:0] doppler_spectrum;
wire doppler_spectrum_valid;
wire [4:0] doppler_bin_out;
wire doppler_processing;
wire doppler_frame_done;
assign doppler_frame_done_out = doppler_frame_done;

// ========== RANGE BIN DECIMATOR SIGNALS ==========
wire signed [15:0] decimated_range_i;
wire signed [15:0] decimated_range_q;
wire decimated_range_valid;
wire [5:0] decimated_range_bin;

// ========== MTI CANCELLER SIGNALS ==========
wire signed [15:0] mti_range_i;
wire signed [15:0] mti_range_q;
wire mti_range_valid;
wire [5:0] mti_range_bin;
wire mti_first_chirp;

// ========== RADAR MODE CONTROLLER SIGNALS ==========
wire rmc_scanning;
wire rmc_scan_complete;
wire [5:0] rmc_chirp_count;
wire [5:0] rmc_elevation_count;
wire [5:0] rmc_azimuth_count;

// ========== MODULE INSTANTIATIONS ==========

// 0. Radar Mode Controller — drives chirp/elevation/azimuth timing signals
//    Default mode: auto-scan (2'b01). Change to 2'b00 for STM32 pass-through.
radar_mode_controller rmc (
    .clk(clk),
    .reset_n(reset_n),
    .mode(host_mode),                     // Controlled by host via USB (default: 2'b01 auto-scan)
    .stm32_new_chirp(stm32_new_chirp_rx),
    .stm32_new_elevation(stm32_new_elevation_rx),
    .stm32_new_azimuth(stm32_new_azimuth_rx),
    .trigger(host_trigger),           // Single-chirp trigger from host via USB
    // Gap 2: Runtime-configurable timing from host USB commands
    .cfg_long_chirp_cycles(host_long_chirp_cycles),
    .cfg_long_listen_cycles(host_long_listen_cycles),
    .cfg_guard_cycles(host_guard_cycles),
    .cfg_short_chirp_cycles(host_short_chirp_cycles),
    .cfg_short_listen_cycles(host_short_listen_cycles),
    .cfg_chirps_per_elev(host_chirps_per_elev),
    .use_long_chirp(use_long_chirp),
    .mc_new_chirp(mc_new_chirp),
    .mc_new_elevation(mc_new_elevation),
    .mc_new_azimuth(mc_new_azimuth),
    .chirp_count(rmc_chirp_count),
    .elevation_count(rmc_elevation_count),
    .azimuth_count(rmc_azimuth_count),
    .scanning(rmc_scanning),
    .scan_complete(rmc_scan_complete)
);
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
wire signed [15:0] ddc_out_i;
wire signed [15:0] ddc_out_q;
wire ddc_valid;

ddc #(.ADC_W(12), .OUT_W(16)) ddc_inst (
    .clk(clk),
    .reset_n(reset_n),
    .mixers_enable(1'b1),
    .adc_data(adc_sample),
    .adc_valid(adc_sample_valid),
    .baseband_i(ddc_out_i),
    .baseband_q(ddc_out_q),
    .baseband_valid(ddc_valid)
);

// DDC output is already 16-bit at 25 MSPS - no rescaling stage
assign adc_i_scaled   = ddc_out_i;
assign adc_q_scaled   = ddc_out_q;
assign adc_valid_sync = ddc_valid;

// 2b. Digital Gain Control with AGC
// Host-configurable power-of-2 shift between DDC output and matched filter.
// Default gain_shift=0, agc_enable=0 → pass-through (no behavioral change).
// When agc_enable=1: auto-adjusts gain per frame based on peak/saturation.
rx_gain_control gain_ctrl (
    .clk(clk),
    .reset_n(reset_n),
    .data_i_in(adc_i_scaled),
    .data_q_in(adc_q_scaled),
    .valid_in(adc_valid_sync),
    .gain_shift(host_gain_shift),
    // AGC configuration
    .agc_enable(host_agc_enable),
    .agc_target(host_agc_target),
    .agc_attack(host_agc_attack),
    .agc_decay(host_agc_decay),
    .agc_holdoff(host_agc_holdoff),
    // Frame boundary from Doppler processor
    .frame_boundary(doppler_frame_done),
    // Outputs
    .data_i_out(gc_i),
    .data_q_out(gc_q),
    .valid_out(gc_valid),
    .saturation_count(gc_saturation_count),
    .peak_magnitude(gc_peak_magnitude),
    .current_gain(gc_current_gain)
);

// Matched filter range-profile wires

wire signed [15:0] range_profile_i;
wire signed [15:0] range_profile_q;
wire range_valid;

// Expose matched filter output to top level for USB range profile
assign range_profile_i_out = range_profile_i;
assign range_profile_q_out = range_profile_q;
assign range_profile_valid_out = range_valid;

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

// ========== CRITICAL: RANGE BIN DECIMATOR ==========
// Convert 256 range bins to 64 bins for Doppler
range_bin_decimator #(
    .INPUT_BINS(256),
    .OUTPUT_BINS(64),
    .DECIMATION_FACTOR(4)
) range_decim (
    .clk(clk),
    .reset_n(reset_n),
    .range_i_in(range_profile_i),
    .range_q_in(range_profile_q),
    .range_valid_in(range_valid),
    .range_i_out(decimated_range_i),
    .range_q_out(decimated_range_q),
    .range_valid_out(decimated_range_valid),
    .range_bin_index(decimated_range_bin),
    .decimation_mode(2'b01),           // Peak detection mode
    .start_bin(10'd0),
    .watchdog_timeout()                // Diagnostic — unconnected (monitored via ILA if needed)
);

// ========== MTI CANCELLER (Ground Clutter Removal) ==========
// 2-pulse canceller: subtracts previous chirp from current chirp.
// H(z) = 1 - z^{-1} → null at DC Doppler, removes stationary clutter.
// When host_mti_enable=0: transparent pass-through.
mti_canceller #(
    .NUM_RANGE_BINS(64),
    .DATA_WIDTH(16)
) mti_inst (
    .clk(clk),
    .reset_n(reset_n),
    .range_i_in(decimated_range_i),
    .range_q_in(decimated_range_q),
    .range_valid_in(decimated_range_valid),
    .range_bin_in(decimated_range_bin),
    .range_i_out(mti_range_i),
    .range_q_out(mti_range_q),
    .range_valid_out(mti_range_valid),
    .range_bin_out(mti_range_bin),
    .mti_enable(host_mti_enable),
    .mti_first_chirp(mti_first_chirp)
);

// ========== FRAME SYNC FROM TRANSMITTER ==========
// [FPGA-001 FIXED] Use the authoritative new_chirp_frame signal from the
// transmitter (via plfm_chirp_controller_enhanced), CDC-synchronized to
// clk_100m in radar_system_top.  Previous code tried to derive frame
// boundaries from chirp_counter == 0, but that counter comes from the
// transmitter path (plfm_chirp_controller_enhanced) which does NOT wrap
// at chirps_per_elev — it overflows to N and only wraps at 6-bit rollover
// (64).  This caused frame pulses at half the expected rate for N=32.
reg tx_frame_start_prev;
reg new_frame_pulse;

always @(posedge clk or negedge reset_n) begin
    if (!reset_n) begin
        tx_frame_start_prev <= 1'b0;
        new_frame_pulse <= 1'b0;
    end else begin
        new_frame_pulse <= 1'b0;
        
        // Edge detect: tx_frame_start is a toggle-CDC derived pulse that
        // may be 1 clock wide.  Capture rising edge for clean 1-cycle pulse.
        if (tx_frame_start && !tx_frame_start_prev) begin
            new_frame_pulse <= 1'b1;
        end
        
        tx_frame_start_prev <= tx_frame_start;
    end
end

assign new_chirp_frame = new_frame_pulse;

// ========== DATA PACKING FOR DOPPLER ==========
// Use MTI-filtered data (or pass-through if MTI disabled)
assign range_data_32bit = {mti_range_q, mti_range_i};
assign range_data_valid = mti_range_valid;

// ========== DOPPLER PROCESSOR ==========
doppler_processor_optimized #(
    .DOPPLER_FFT_SIZE(16),
    .RANGE_BINS(64),
    .CHIRPS_PER_FRAME(32),
    .CHIRPS_PER_SUBFRAME(16)
) doppler_proc (
    .clk(clk),
    .reset_n(reset_n),
    .range_data(range_data_32bit),
    .data_valid(range_data_valid),
    .new_chirp_frame(new_chirp_frame),
    
    // Outputs
    .doppler_output(doppler_output),
    .doppler_valid(doppler_valid),
    .doppler_bin(doppler_bin),
    .range_bin(range_bin),
    
    // Status
    .processing_active(doppler_processing),
    .frame_complete(doppler_frame_done),
    .status()
);

// ========== OUTPUT CONNECTIONS ==========
// doppler_output, doppler_valid, doppler_bin, range_bin are directly
// connected to doppler_proc ports above

// ========== STATUS ==========

// ========== ADC DEBUG TAP (for self-test / bring-up) ==========
assign dbg_adc_i     = adc_i_scaled;
assign dbg_adc_q     = adc_q_scaled;
assign dbg_adc_valid = adc_valid_sync;

// ========== AGC STATUS OUTPUTS ==========
assign agc_saturation_count = gc_saturation_count;
assign agc_peak_magnitude   = gc_peak_magnitude;
assign agc_current_gain     = gc_current_gain;

endmodule
