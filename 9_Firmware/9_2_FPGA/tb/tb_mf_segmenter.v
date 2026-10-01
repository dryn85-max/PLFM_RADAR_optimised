`timescale 1ns / 1ps
// ============================================================================
// tb_mf_segmenter.v — matched_filter_multi_segment (overlap-save segmenter)
//
// Checks the exact sample stream the segmenter presents to the chain:
//   long chirp : 4 segments of 256, segment s = input samples [224*s, 224*s+256)
//                (32-sample overlap) of the 928-sample receive window,
//                ref_segment = s
//   short chirp: 1 segment, 13 samples then zeros, ref_segment = 4
// Input sample k carries the value k+1, so every fed word identifies its index.
// The source is a CONTINUOUS stream, 1 sample / 4 clk (25 MSPS) from the chirp
// start on, and never waits for the segmenter (the DDC of the receiver is not
// stalled): the segmenter has to buffer the whole window without gaps while
// it is still processing earlier segments, and ignore samples after it.
// Also: toggle semantics of mc_new_chirp (both edges start a chirp), a reset in
// the middle of a segment, back-to-back chirps, the sticky mf_overrun flag
// (a chirp that arrives while busy is ignored, nothing is corrupted) and the
// processing time per chirp (budget: < PRI = 1 ms = 100000 clk).
// Run twice: with -DSIMULATION (behavioral chain, ~300 clk/segment) and without
// (synthesizable chain, ~9.75k clk/segment).
// ============================================================================
module tb_mf_segmenter;
    localparam CLK_PERIOD = 10;
    localparam LONG_WIN = 928, STREAM_N = 1000, SHORT_N = 13, ADV = 224, NF = 1024;
    reg clk, reset_n;
    reg signed [15:0] ddc_i, ddc_q;
    reg ddc_valid, use_long, tog;
    wire signed [15:0] pc_i, pc_q;
    wire pc_valid;
    wire [3:0] status;
    wire mf_overrun;
    integer pass_count, fail_count, test_num;
    integer tog_at, feeds, outs, k, t, i, bad, bad_ref, first_bad, busy_cycles, max_busy;
    integer feed_val [0:NF-1];
    integer feed_ref [0:NF-1];

    always #(CLK_PERIOD/2) clk = ~clk;

    matched_filter_multi_segment uut (
        .clk(clk), .reset_n(reset_n),
        .ddc_i(ddc_i), .ddc_q(ddc_q), .ddc_valid(ddc_valid),
        .use_long_chirp(use_long), .chirp_counter(6'd0),
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
        if (uut.state != 4'd0) busy_cycles = busy_cycles + 1;
    end

    // Continuous stream: n samples (value k+1), one per 4 clk, whatever the segmenter does
    task drive_samples;
        input integer n;
        begin
            for (k = 0; k < n; k = k + 1) begin
                @(posedge clk); #1;
                ddc_i = k + 1; ddc_q = -(k + 1); ddc_valid = 1'b1;
                if (k == tog_at) tog = ~tog;
                @(posedge clk); #1; ddc_valid = 1'b0;
                repeat (2) @(posedge clk);
                #1;
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
            feeds = 0; outs = 0; busy_cycles = 0; use_long = long;
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
                exp_v = (n < LONG_WIN) ? n + 1 : 0;
                if (feed_val[i] !== exp_v) begin bad = bad + 1; if (first_bad < 0) first_bad = i; end
                if (feed_ref[i] !== s) bad_ref = bad_ref + 1;
            end
            $display("%0s: feeds=%0d outputs=%0d bad_samples=%0d (first %0d) bad_ref=%0d",
                     tag, feeds, outs, bad, first_bad, bad_ref);
            if (first_bad >= 0)
                $display("  first bad: fed %0d, expected %0d", feed_val[first_bad],
                         (ADV * (first_bad / 256) + first_bad % 256 < LONG_WIN) ? ADV * (first_bad / 256) + first_bad % 256 + 1 : 0);
            check(feeds == NF, "long chirp: 4 x 256 samples fed to the chain");
            check(bad == 0, "long chirp: segment s = samples [224s, 224s+256), zero padded");
            check(bad_ref == 0, "long chirp: ref_segment = segment index");
            check(outs == NF, "long chirp: 1024 range-profile outputs");
            check(uut.state == 4'd0, "long chirp: segmenter back in IDLE");
            $display("  processing time: %0d clk busy per long chirp (PRI budget 100000 clk)", busy_cycles);
            check(busy_cycles < 100000, "long chirp: collect + 4 segments fit in the 1 ms PRI");
            if (busy_cycles > max_busy) max_busy = busy_cycles;
        end
    endtask

    initial begin
        clk = 0; reset_n = 0; ddc_i = 0; ddc_q = 0; ddc_valid = 0; use_long = 1; tog = 0;
        pass_count = 0; fail_count = 0; test_num = 0; feeds = 0; outs = 0; busy_cycles = 0; max_busy = 0; tog_at = -1;
        repeat (4) @(posedge clk); #1; reset_n = 1; repeat (3) @(posedge clk); #1;

        // ---- long chirp ----
        start_chirp(1);
        drive_samples(STREAM_N);
        wait_idle;
        check_long_stream("long chirp");

        // ---- short chirp (second toggle edge, back-to-back) ----
        start_chirp(0);
        drive_samples(40);
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
        drive_samples(STREAM_N);
        wait_idle;
        check_long_stream("long chirp after mid-segment reset");

        check(mf_overrun == 1'b0, "no overrun flag after well-spaced chirps");

        // ---- overrun: a new chirp while segments are still being processed ----
        tog_at = 950;                            // window complete at 928: segment 0 is processing
        start_chirp(1);
        drive_samples(STREAM_N);                 // new chirp (toggle) arrives at sample 950
        check(mf_overrun == 1'b1, "early chirp (during processing) sets the sticky mf_overrun flag");
        wait_idle;
        check_long_stream("long chirp with an early chirp ignored");
        check(mf_overrun == 1'b1, "mf_overrun stays set (sticky) after the segmenter is idle");
        // the next well-spaced chirp is processed normally, flag stays sticky
        tog_at = -1;
        start_chirp(1);
        drive_samples(STREAM_N);
        wait_idle;
        check_long_stream("long chirp after an overrun");
        check(mf_overrun == 1'b1, "mf_overrun not cleared by a later chirp");
        // a chirp during collection is an overrun as well
        reset_n = 0; tog = 0; repeat (3) @(posedge clk); #1; reset_n = 1; repeat (3) @(posedge clk); #1;
        check(mf_overrun == 1'b0, "reset clears mf_overrun");
        tog_at = 100;
        start_chirp(1);
        drive_samples(STREAM_N);                 // toggle at sample 100, still collecting
        check(mf_overrun == 1'b1, "chirp during collection sets mf_overrun");
        wait_idle;
        check_long_stream("long chirp with a chirp ignored during collection");
        $display("max busy time of a long chirp: %0d clk = %0d us at 100 MHz", max_busy, max_busy / 100);

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
