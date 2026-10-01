`timescale 1ns / 1ps
// ============================================================================
// tb_mf_segmenter.v — matched_filter_multi_segment (overlap-save segmenter)
//
// Checks the exact sample stream the segmenter presents to the chain:
//   long chirp : 4 segments of 256, segment s = input samples [224*s, 224*s+256)
//                (32-sample overlap), zero padded after sample 749,
//                ref_segment = s
//   short chirp: 1 segment, 13 samples then zeros, ref_segment = 4
// Input sample k carries the value k+1, so every fed word identifies its index.
// The source only drives a sample while the segmenter is collecting (the DDC of
// the receiver is not stalled by the segmenter, this TB isolates the buffering
// and RAM read pipeline).  Also: toggle semantics of mc_new_chirp (both edges
// start a chirp), a reset in the middle of a segment, back-to-back chirps.
// Compiled with -DSIMULATION (behavioral chain) so each segment takes ~300 clk.
// ============================================================================
module tb_mf_segmenter;
    localparam CLK_PERIOD = 10;
    localparam LONG_N = 750, SHORT_N = 13, ADV = 224, NF = 1024;
    reg clk, reset_n;
    reg signed [15:0] ddc_i, ddc_q;
    reg ddc_valid, use_long, tog;
    wire signed [15:0] pc_i, pc_q;
    wire pc_valid;
    wire [3:0] status;
    integer pass_count, fail_count, test_num;
    integer feeds, outs, k, t, i, bad, bad_ref, first_bad;
    integer feed_val [0:NF-1];
    integer feed_ref [0:NF-1];

    always #(CLK_PERIOD/2) clk = ~clk;

    matched_filter_multi_segment uut (
        .clk(clk), .reset_n(reset_n),
        .ddc_i(ddc_i), .ddc_q(ddc_q), .ddc_valid(ddc_valid),
        .use_long_chirp(use_long), .chirp_counter(6'd0),
        .mc_new_chirp(tog), .mc_new_elevation(1'b0), .mc_new_azimuth(1'b0),
        .pc_i_w(pc_i), .pc_q_w(pc_q), .pc_valid_w(pc_valid), .status(status));

    task check;
        input cond; input [511:0] label;
        begin
            test_num = test_num + 1;
            if (cond) begin $display("[PASS] Test %0d: %0s", test_num, label); pass_count = pass_count + 1; end
            else       begin $display("[FAIL] Test %0d: %0s", test_num, label); fail_count = fail_count + 1; end
        end
    endtask

    // Record what the chain is fed and what it returns
    always @(posedge clk) if (reset_n) begin
        if (uut.fft_input_valid) begin
            if (feeds < NF) begin
                feed_val[feeds] = uut.fft_input_i;
                feed_ref[feeds] = uut.ref_segment;
            end
            feeds = feeds + 1;
        end
        if (pc_valid) outs = outs + 1;
    end

    // Drive n samples (value k+1), only while the segmenter collects
    task drive_samples;
        input integer n;
        begin
            k = 0; t = 0;
            while (k < n && t < 200000) begin
                @(posedge clk); #1; t = t + 1;
                if (uut.state == 4'd1 && uut.buffer_write_ptr < 256) begin
                    ddc_i = k + 1; ddc_q = -(k + 1); ddc_valid = 1'b1; k = k + 1;
                    @(posedge clk); #1; ddc_valid = 1'b0;
                    repeat (3) @(posedge clk);       // 25 MSPS: one sample per 4 clk
                    #1;
                end
            end
            ddc_valid = 1'b0;
        end
    endtask

    task wait_idle;
        begin
            t = 0;
            while (uut.state != 4'd0 && t < 400000) begin @(posedge clk); t = t + 1; end
            repeat (10) @(posedge clk); #1;
        end
    endtask

    task start_chirp;
        input long;
        begin
            feeds = 0; outs = 0; use_long = long;
            @(posedge clk); #1; tog = ~tog;          // toggle: either edge starts a chirp
        end
    endtask

    task check_long_stream;
        input [511:0] tag;
        integer s, m, n, exp_v;
        begin
            bad = 0; bad_ref = 0; first_bad = -1;
            for (i = 0; i < NF; i = i + 1) begin
                s = i / 256; m = i % 256; n = ADV * s + m;
                exp_v = (n < LONG_N) ? n + 1 : 0;
                if (feed_val[i] !== exp_v) begin bad = bad + 1; if (first_bad < 0) first_bad = i; end
                if (feed_ref[i] !== s) bad_ref = bad_ref + 1;
            end
            $display("%0s: feeds=%0d outputs=%0d bad_samples=%0d (first %0d) bad_ref=%0d",
                     tag, feeds, outs, bad, first_bad, bad_ref);
            if (first_bad >= 0)
                $display("  first bad: fed %0d, expected %0d", feed_val[first_bad],
                         (ADV * (first_bad / 256) + first_bad % 256 < LONG_N) ? ADV * (first_bad / 256) + first_bad % 256 + 1 : 0);
            check(feeds == NF, "long chirp: 4 x 256 samples fed to the chain");
            check(bad == 0, "long chirp: segment s = samples [224s, 224s+256), zero padded");
            check(bad_ref == 0, "long chirp: ref_segment = segment index");
            check(outs == NF, "long chirp: 1024 range-profile outputs");
            check(uut.state == 4'd0, "long chirp: segmenter back in IDLE");
        end
    endtask

    initial begin
        clk = 0; reset_n = 0; ddc_i = 0; ddc_q = 0; ddc_valid = 0; use_long = 1; tog = 0;
        pass_count = 0; fail_count = 0; test_num = 0; feeds = 0; outs = 0;
        repeat (4) @(posedge clk); #1; reset_n = 1; repeat (3) @(posedge clk); #1;

        // ---- long chirp ----
        start_chirp(1);
        drive_samples(LONG_N);
        wait_idle;
        check_long_stream("long chirp");

        // ---- short chirp (second toggle edge, back-to-back) ----
        start_chirp(0);
        drive_samples(SHORT_N);
        wait_idle;
        bad = 0; bad_ref = 0;
        for (i = 0; i < 256; i = i + 1) begin
            if (feed_val[i] !== ((i < SHORT_N) ? i + 1 : 0)) bad = bad + 1;
            if (feed_ref[i] !== 4) bad_ref = bad_ref + 1;
        end
        $display("short chirp: feeds=%0d outputs=%0d bad_samples=%0d bad_ref=%0d", feeds, outs, bad, bad_ref);
        check(feeds == 256 && outs == 256, "short chirp: one 256-sample segment, 256 outputs");
        check(bad == 0, "short chirp: 13 samples then zero padding");
        check(bad_ref == 0, "short chirp: ref_segment = 4");
        check(uut.state == 4'd0, "short chirp: segmenter back in IDLE");

        // ---- reset in the middle of a long chirp, then a clean long chirp ----
        start_chirp(1);
        drive_samples(100);
        // the toggle source (radar_mode_controller) is reset together with the segmenter
        reset_n = 0; tog = 0; repeat (3) @(posedge clk); #1; reset_n = 1; repeat (3) @(posedge clk); #1;
        check(uut.state == 4'd0 && uut.buffer_write_ptr == 0, "mid-segment reset: segmenter back in IDLE, buffer empty");
        start_chirp(1);
        drive_samples(LONG_N);
        wait_idle;
        check_long_stream("long chirp after mid-segment reset");

        $display("\nResults: %0d/%0d passed", pass_count, pass_count + fail_count);
        $finish;
    end

    // a hang must fail, never run forever
    initial begin
        #20_000_000;
        $display("[FAIL] Test 999: watchdog expired");
        $display("\nResults: %0d/%0d passed", pass_count, pass_count + fail_count + 1);
        $finish;
    end
endmodule
