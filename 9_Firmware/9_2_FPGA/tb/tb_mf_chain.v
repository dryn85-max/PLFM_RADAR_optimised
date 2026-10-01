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
    real m2, peak_m2, gold_peak_m2;   // squared L2 magnitude (|.|^2 can reach 2^31: use real, not integer)
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
            peak_m2 = -1.0; gold_peak_m2 = -1.0;
            // Peak bin by L2 magnitude (the golden's argmax); peak/mean reported as L1 = |I|+|Q|.
            // (An L1 argmax can pick the neighbouring bin when two bins are within 1 %.)
            for (i = 0; i < N; i = i + 1) begin
                if (cap_i[i] !== $signed(gold_i[i]) || cap_q[i] !== $signed(gold_q[i])) mismatches = mismatches + 1;
                mag = (cap_i[i] < 0 ? -cap_i[i] : cap_i[i]) + (cap_q[i] < 0 ? -cap_q[i] : cap_q[i]);
                mean_mag = mean_mag + mag;
                m2 = 1.0 * cap_i[i] * cap_i[i] + 1.0 * cap_q[i] * cap_q[i];
                if (m2 > peak_m2) begin peak_m2 = m2; peak_mag = mag; peak_bin = i; end
                mag = ($signed(gold_i[i]) < 0 ? -$signed(gold_i[i]) : $signed(gold_i[i]))
                    + ($signed(gold_q[i]) < 0 ? -$signed(gold_q[i]) : $signed(gold_q[i]));
                m2 = 1.0 * $signed(gold_i[i]) * $signed(gold_i[i]) + 1.0 * $signed(gold_q[i]) * $signed(gold_q[i]);
                if (m2 > gold_peak_m2) begin gold_peak_m2 = m2; gold_peak_mag = mag; gold_peak_bin = i; end
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

    // Full-scale chain-level vector (square-wave I/Q, spectrum peak ~1.27*2^23):
    // exact match to the bit-accurate model (INTERNAL_W = 25).  A chain built
    // with a 24-bit internal width wraps in the forward FFT and fails this.
    task run_fullscale;
        integer wait_count, mm;
        begin
            $readmemh("tb/golden/mf_sig_fs_i.hex", sig_i);   $readmemh("tb/golden/mf_sig_fs_q.hex", sig_q);
            $readmemh("tb/golden/mf_gold_fs_i.hex", gold_i); $readmemh("tb/golden/mf_gold_fs_q.hex", gold_q);
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
            mm = 0;
            for (i = 0; i < N; i = i + 1)
                if (cap_i[i] !== $signed(gold_i[i]) || cap_q[i] !== $signed(gold_q[i])) mm = mm + 1;
            $display("full-scale: outputs=%0d mismatches=%0d", cap_count, mm);
            check(cap_count == N, "full-scale: 256 range-profile outputs");
            if (EXACT) check(mm == 0, "full-scale: synth branch bit-exact vs golden (INTERNAL_W=25)");
        end
    endtask

    initial begin
        clk = 0; reset_n = 0; adc_valid = 0; adc_data_i = 0; adc_data_q = 0; ref_segment = 0;
        pass_count = 0; fail_count = 0; test_num = 0; cap_count = 0;
        $display("tb_mf_chain: %s branch", EXACT ? "SYNTHESIZABLE (exact)" : "BEHAVIORAL (tolerant)");
        run_frame(0);
        run_frame(20);
        run_frame(100);
        run_fullscale;
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
