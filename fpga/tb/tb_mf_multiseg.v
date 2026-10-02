`timescale 1ns / 1ps
// ============================================================================
// tb_mf_multiseg.v — segmenter + chain, all 4 overlap-save segments with a
// continuous input stream (1 sample / 4 clk, never stalled).
//
// The input is the long chirp delayed by d = 20 / 100 / 150 samples (whole
// 928-sample receive window, tb/golden/mf_ms_d*.hex from
// gen_mf_chain_golden.py).  Segment s correlates window [224s, 224s+256)
// with ROM segment s, so for d <= 178 the range-profile peak of EVERY segment
// sits at bin d.  With the old segmenter (samples dropped while processing)
// segments 1..3 never saw their data and had no peak.
// Compiled twice by run_regression.sh (behavioral and synthesizable chain).
// ============================================================================
module tb_mf_multiseg;
    localparam CLK_PERIOD = 10, NS = 4, N = 256, WIN = 1024;
    reg clk, reset_n, ddc_valid, tog;
    reg signed [15:0] ddc_i, ddc_q;
    reg [15:0] sig_i [0:WIN-1], sig_q [0:WIN-1];
    wire signed [15:0] pc_i, pc_q;
    wire pc_valid, mf_overrun;
    wire [3:0] status;
    reg signed [15:0] cap_i [0:NS*N-1], cap_q [0:NS*N-1];
    integer cap_count, i, k, s, pass_count, fail_count, test_num, t;
    integer peak_bin, peak_mag, mean_mag, mag;
    real m2, peak_m2;

    always #(CLK_PERIOD/2) clk = ~clk;

    matched_filter_multi_segment uut (
        .clk(clk), .reset_n(reset_n),
        .ddc_i(ddc_i), .ddc_q(ddc_q), .ddc_valid(ddc_valid),
        .use_long_chirp(1'b1), .chirp_counter(6'd0),
        .mc_new_chirp(tog), .mc_new_elevation(1'b0), .mc_new_azimuth(1'b0),
        .pc_i_w(pc_i), .pc_q_w(pc_q), .pc_valid_w(pc_valid), .status(status), .mf_overrun(mf_overrun));

    task check;
        input cond; input [511:0] label;
        begin
            test_num = test_num + 1;
            if (cond) begin $display("[PASS] Test %0d: %0s", test_num, label); pass_count = pass_count + 1; end
            else       begin $display("[FAIL] Test %0d: %0s", test_num, label); fail_count = fail_count + 1; end
        end
    endtask

    always @(posedge clk) if (reset_n && pc_valid && cap_count < NS*N) begin
        cap_i[cap_count] = pc_i; cap_q[cap_count] = pc_q; cap_count = cap_count + 1;
    end

    task run_delay;
        input integer d;
        begin
            case (d)
            20:  begin $readmemh("tb/golden/mf_ms_d20_i.hex", sig_i);  $readmemh("tb/golden/mf_ms_d20_q.hex", sig_q); end
            100: begin $readmemh("tb/golden/mf_ms_d100_i.hex", sig_i); $readmemh("tb/golden/mf_ms_d100_q.hex", sig_q); end
            default: begin $readmemh("tb/golden/mf_ms_d150_i.hex", sig_i); $readmemh("tb/golden/mf_ms_d150_q.hex", sig_q); end
            endcase
            cap_count = 0;
            @(posedge clk); #1; tog = ~tog;
            for (k = 0; k < 1000; k = k + 1) begin     // continuous stream, no back-pressure
                @(posedge clk); #1; ddc_i = sig_i[k]; ddc_q = sig_q[k]; ddc_valid = 1'b1;
                @(posedge clk); #1; ddc_valid = 1'b0;
                repeat (2) @(posedge clk);
                #1;
            end
            t = 0;
            while ((uut.state != 4'd0 || cap_count < NS*N) && t < 400000) begin @(posedge clk); t = t + 1; end
            repeat (5) @(posedge clk); #1;
            check(cap_count == NS*N, "1024 range-profile outputs (4 segments)");
            for (s = 0; s < NS; s = s + 1) begin
                peak_bin = -1; peak_mag = 0; mean_mag = 0; peak_m2 = -1.0;
                for (i = 0; i < N; i = i + 1) begin
                    mag = (cap_i[s*N+i] < 0 ? -cap_i[s*N+i] : cap_i[s*N+i])
                        + (cap_q[s*N+i] < 0 ? -cap_q[s*N+i] : cap_q[s*N+i]);
                    mean_mag = mean_mag + mag;
                    m2 = 1.0 * cap_i[s*N+i] * cap_i[s*N+i] + 1.0 * cap_q[s*N+i] * cap_q[s*N+i];
                    if (m2 > peak_m2) begin peak_m2 = m2; peak_mag = mag; peak_bin = i; end
                end
                mean_mag = mean_mag / N;
                $display("delay %0d segment %0d: peak_bin=%0d peak=%0d mean=%0d", d, s, peak_bin, peak_mag, mean_mag);
                check(peak_bin == d, "segment peak bin == target delay");
                check(peak_mag > 4 * mean_mag, "segment peak >= 4x mean");
            end
            check(mf_overrun == 1'b0, "no overrun");
        end
    endtask

    initial begin
        #20_000_000;
        $display("[FAIL] Test 999: watchdog expired");
        $display("\nResults: %0d/%0d passed", pass_count, pass_count + fail_count + 1);
        $finish;
    end

    initial begin
        clk = 0; reset_n = 0; ddc_valid = 0; ddc_i = 0; ddc_q = 0; tog = 0;
        pass_count = 0; fail_count = 0; test_num = 0; cap_count = 0;
        repeat (4) @(posedge clk); #1; reset_n = 1; repeat (3) @(posedge clk); #1;
        run_delay(20);
        run_delay(100);
        run_delay(150);
        $display("\nResults: %0d/%0d passed", pass_count, pass_count + fail_count);
        $finish;
    end
endmodule
