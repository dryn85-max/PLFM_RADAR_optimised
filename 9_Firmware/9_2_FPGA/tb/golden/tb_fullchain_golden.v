`timescale 1ns / 1ps
// ============================================================================
// tb_fullchain_golden.v — golden test (d): ADC -> DDC -> gain -> matched
// filter range profile.  Single-chirp mode (host_mode = 2'b10, host_trigger):
// the echo starts D ADC samples after the chirp-start pulse; the range-profile
// peak of segment 0 must move by (D2 - D1)/4 = 40 bins between the two runs
// and stand at least 8x above the mean.  Gain shift 1100b (/16) keeps the
// FFT input below saturation.
// The ADC stream is continuous (DDC never stalls), so this also checks that the
// segmenter buffers the whole receive window: the echo shows up in all four
// overlap-save segments, at the same bin as in segment 0 (+-2), >= 4x the
// segment mean (segment 3 only overlaps the chirp tail).
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
    integer cap_count, peak_bin, peak_mag, mean_mag, mag, peak1, peak2, sg, ss;
    integer seg_peak_bin [0:3], seg_peak_mag [0:3], seg_mean [0:3];
    real seg_peak_m2 [0:3];
    reg capturing;
    reg chirp_toggle0;
    real m2, peak_m2;   // squared L2 magnitude (real: |.|^2 can reach 2^31)

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

    // Capture all 4 segments (4 x N_FFT outputs) after a trigger
    always @(posedge clk) if (capturing && rp_valid) begin
        if (cap_count < 4 * N_FFT) begin
            sg = cap_count / N_FFT;
            mag = (rp_i < 0 ? -rp_i : rp_i) + (rp_q < 0 ? -rp_q : rp_q);
            seg_mean[sg] = seg_mean[sg] + mag;
            m2 = 1.0 * rp_i * rp_i + 1.0 * rp_q * rp_q;
            if (m2 > seg_peak_m2[sg]) begin
                seg_peak_m2[sg] = m2; seg_peak_mag[sg] = mag; seg_peak_bin[sg] = cap_count % N_FFT;
            end
        end
        cap_count = cap_count + 1;
    end

    task run_chirp;
        input [1023:0] hexfile;
        integer t;
        begin
            $readmemh(hexfile, adc_mem);
            cap_count = 0; capturing = 1;
            for (ss = 0; ss < 4; ss = ss + 1) begin
                seg_peak_bin[ss] = -1; seg_peak_mag[ss] = 0; seg_peak_m2[ss] = -1.0; seg_mean[ss] = 0;
            end
            // trigger one long chirp; the mode controller toggles mc_new_chirp a few cycles later
            // (the toggle happens at the first clock edge that sees host_trigger = 1, so poll
            //  for it after that edge instead of waiting for an event that already fired)
            chirp_toggle0 = dut.mc_new_chirp;
            host_trigger = 1; @(posedge clk); #1; host_trigger = 0;
            t = 0;
            while (dut.mc_new_chirp === chirp_toggle0 && t < 100) begin @(posedge clk); #1; t = t + 1; end
            check(dut.mc_new_chirp !== chirp_toggle0, "single-chirp trigger toggled mc_new_chirp");
            for (k = 0; k < N_IN; k = k + 1) begin
                @(posedge clk); #1; adc_data = adc_mem[k];
            end
            adc_data = 12'h800;
            t = 0;
            while (cap_count < 4 * N_FFT && t < 400000) begin @(posedge clk); t = t + 1; end
            capturing = 0;
            for (ss = 0; ss < 4; ss = ss + 1) begin
                seg_mean[ss] = seg_mean[ss] / N_FFT;
                $display("  segment %0d: peak_bin=%0d peak=%0d mean=%0d", ss, seg_peak_bin[ss], seg_peak_mag[ss], seg_mean[ss]);
            end
            peak_bin = seg_peak_bin[0]; peak_mag = seg_peak_mag[0]; mean_mag = seg_mean[0];
            check(cap_count == 4 * N_FFT, "4 segments x 256 range bins produced");
            // The mode controller only accepts a trigger in S_IDLE, which it re-enters
            // long_chirp + long_listen = 16700 cycles after the trigger; the segmenter
            // needs ~44k cycles for 4 segments (budget: PRI = 100000).  Wait for both.
            repeat (17000) @(posedge clk); #1;
            t = 0;
            while (dut.mf_dual.state != 4'd0 && t < 100000) begin @(posedge clk); t = t + 1; end
            check(dut.mf_dual.state == 4'd0, "segmenter back in IDLE after the chirp");
            check(!dut.mf_overrun, "no matched-filter overrun");
            for (ss = 1; ss < 4; ss = ss + 1) begin
                check(seg_peak_bin[ss] >= seg_peak_bin[0] - 2 && seg_peak_bin[ss] <= seg_peak_bin[0] + 2,
                      "segs 1-3: peak bin within +-2 of segment 0");
                check(seg_peak_mag[ss] > 4 * seg_mean[ss], "segs 1-3: peak >= 4x mean");
            end
        end
    endtask

    // global watchdog: a hang must fail, never run forever
    initial begin
        #10_000_000;
        $display("[FAIL] Test 999: simulation watchdog expired");
        $display("\nResults: %0d/%0d passed", pass_count, pass_count + fail_count + 1);
        $finish;
    end

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
