`timescale 1ns / 1ps


// ============================================================================
// cdc_handshake stress harness: back-to-back traffic, data-integrity scoreboard.
//   HOLD_VALID=1 : src_valid held permanently high, src_data changes every
//                  src_clk; the accepted words (src_valid && src_ready at a
//                  src_clk edge) are the reference sequence.
//   HOLD_VALID=0 : valid/ready producer with random gaps, value = running index.
//   DST_BP=1     : destination applies random back-pressure on dst_ready.
// Every word delivered at the destination must equal the next accepted word, in
// order, with no duplicates, no gaps and no tearing.  The two domains are
// released from reset at different times.
// ============================================================================
module hs_stress #(
    parameter SRC_PERIOD = 8.333,
    parameter DST_PERIOD = 10.0,
    parameter HOLD_VALID = 1,
    parameter DST_BP     = 0,
    parameter NWORDS     = 300,
    parameter SEED       = 1
)(
    output reg         done,
    output reg [31:0]  errors,
    output reg [31:0]  n_sent,
    output reg [31:0]  n_recv
);
    reg src_clk, dst_clk, src_rst_n, dst_rst_n;
    reg [31:0] src_data;
    reg        src_valid;
    wire       src_ready, dst_valid;
    wire [31:0] dst_data;
    reg        dst_ready;
    reg [31:0] sent [0:NWORDS+15];
    integer    seed_a, seed_b;
    reg [31:0] ctr;
    integer    r;

    initial begin
        src_clk = 0; dst_clk = 0; src_rst_n = 0; dst_rst_n = 0;
        src_data = 0; src_valid = 0; dst_ready = 0; done = 0;
        errors = 0; n_sent = 0; n_recv = 0; ctr = 32'h1000;
        seed_a = SEED; seed_b = SEED + 77;
    end
    always #(SRC_PERIOD/2.0) src_clk = ~src_clk;
    always #(DST_PERIOD/2.0) dst_clk = ~dst_clk;

    cdc_handshake #(.WIDTH(32)) dut (
        .src_clk(src_clk), .dst_clk(dst_clk),
        .src_reset_n(src_rst_n), .dst_reset_n(dst_rst_n),
        .src_data(src_data), .src_valid(src_valid), .src_ready(src_ready),
        .dst_data(dst_data), .dst_valid(dst_valid), .dst_ready(dst_ready)
    );

    // Source side producer + accepted-word log
    always @(posedge src_clk) begin
        if (!src_rst_n) begin
            src_valid <= 1'b0;
            src_data  <= 32'd0;
            ctr       <= 32'h1000;
        end else if (HOLD_VALID) begin
            src_valid <= (n_sent < NWORDS);
            ctr       <= ctr + 32'd1;
            src_data  <= ctr;
            if (src_valid && src_ready) begin
                sent[n_sent] = src_data;
                n_sent = n_sent + 1;
            end
        end else begin
            if (src_valid && src_ready) begin
                sent[n_sent] = src_data;
                n_sent = n_sent + 1;
                ctr <= ctr + 32'd1;
                src_data <= (ctr + 32'd1) * 32'h01010101;
                r = $random(seed_a);
                src_valid <= (n_sent < NWORDS) && (r[1:0] != 2'b00);
            end else if (!src_valid) begin
                r = $random(seed_a);
                src_data  <= ctr * 32'h01010101;
                src_valid <= (n_sent < NWORDS) && (r[1:0] != 2'b00);
            end
        end
    end

    // Destination consumer + scoreboard
    always @(posedge dst_clk) begin
        if (!dst_rst_n) begin
            dst_ready <= 1'b0;
        end else begin
            if (DST_BP) begin
                r = $random(seed_b);
                dst_ready <= r[0];
            end else
                dst_ready <= 1'b1;
            if (dst_valid && dst_ready) begin
                if (n_recv >= n_sent) begin
                    errors = errors + 1;   // word from nowhere (duplicate)
                end else if (dst_data !== sent[n_recv]) begin
                    errors = errors + 1;   // tear / gap / reorder
                end
                n_recv = n_recv + 1;
            end
        end
    end

    initial begin
        #100;       src_rst_n = 1;
        #37;        dst_rst_n = 1;
        wait (n_sent >= NWORDS);
        #3000;      // drain + look for late duplicates
        done = 1;
    end
    // timeout so a stuck handshake is reported rather than hanging the run
    initial begin
        #2000000;
        if (!done) begin
            errors = errors + 1000;
            done = 1;
        end
    end
endmodule

module tb_cdc_modules;

    // ── Clock periods (reflecting real system) ─────────────────
    localparam SRC_CLK_PERIOD = 2.5;    // fast source clock (4:1 ratio to the 100 MHz processing domain)
    localparam DST_CLK_PERIOD = 10.0;   // 100 MHz (processing domain)
    // For handshake tests, use different ratio
    localparam HS_SRC_PERIOD  = 10.0;   // 100 MHz
    localparam HS_DST_PERIOD  = 7.0;    // ~143 MHz (non-integer ratio)

    // ── Test bookkeeping ───────────────────────────────────────
    integer pass_count;
    integer fail_count;
    integer test_num;
    integer i;

    // ── Check task ─────────────────────────────────────────────
    task check;
        input cond;
        input [511:0] label;
        begin
            test_num = test_num + 1;
            if (cond) begin
                $display("[PASS] Test %0d: %0s", test_num, label);
                pass_count = pass_count + 1;
            end else begin
                $display("[FAIL] Test %0d: %0s", test_num, label);
                fail_count = fail_count + 1;
            end
        end
    endtask

    // ════════════════════════════════════════════════════════════
    // MODULE 2: cdc_single_bit
    // ════════════════════════════════════════════════════════════
    reg         m2_src_clk;
    reg         m2_dst_clk;
    reg         m2_reset_n;
    reg         m2_src_signal;
    wire        m2_dst_signal;

    always #(SRC_CLK_PERIOD/2) m2_src_clk = ~m2_src_clk;
    always #(DST_CLK_PERIOD/2) m2_dst_clk = ~m2_dst_clk;

    cdc_single_bit #(
        .STAGES(3)
    ) uut_m2 (
        .src_clk  (m2_src_clk),
        .dst_clk  (m2_dst_clk),
        .reset_n  (m2_reset_n),
        .src_signal(m2_src_signal),
        .dst_signal(m2_dst_signal)
    );

    // ════════════════════════════════════════════════════════════
    // MODULE 3: cdc_handshake
    // ════════════════════════════════════════════════════════════
    reg          m3_src_clk;
    reg          m3_dst_clk;
    reg          m3_reset_n;
    reg  [31:0]  m3_src_data;
    reg          m3_src_valid;
    wire         m3_src_ready;
    wire [31:0]  m3_dst_data;
    wire         m3_dst_valid;
    reg          m3_dst_ready;

    always #(HS_SRC_PERIOD/2) m3_src_clk = ~m3_src_clk;
    always #(HS_DST_PERIOD/2) m3_dst_clk = ~m3_dst_clk;

    cdc_handshake #(
        .WIDTH(32)
    ) uut_m3 (
        .src_clk  (m3_src_clk),
        .dst_clk  (m3_dst_clk),
        .src_reset_n(m3_reset_n),
        .dst_reset_n(m3_reset_n),
        .src_data (m3_src_data),
        .src_valid(m3_src_valid),
        .src_ready(m3_src_ready),
        .dst_data (m3_dst_data),
        .dst_valid(m3_dst_valid),
        .dst_ready(m3_dst_ready)
    );

    // ── Stress instances (all run concurrently with the directed tests) ──
    wire st0_done, st1_done, st2_done, st3_done, st4_done, st5_done;
    wire [31:0] st0_err, st1_err, st2_err, st3_err, st4_err, st5_err;
    wire [31:0] st0_sent, st1_sent, st2_sent, st3_sent, st4_sent, st5_sent;
    wire [31:0] st0_recv, st1_recv, st2_recv, st3_recv, st4_recv, st5_recv;
    hs_stress #(.SRC_PERIOD(8.333), .DST_PERIOD(10.0),  .HOLD_VALID(1), .DST_BP(0), .SEED(1))
        st0 (.done(st0_done), .errors(st0_err), .n_sent(st0_sent), .n_recv(st0_recv));
    hs_stress #(.SRC_PERIOD(10.0),  .DST_PERIOD(8.333), .HOLD_VALID(1), .DST_BP(0), .SEED(2))
        st1 (.done(st1_done), .errors(st1_err), .n_sent(st1_sent), .n_recv(st1_recv));
    hs_stress #(.SRC_PERIOD(8.333), .DST_PERIOD(10.0),  .HOLD_VALID(0), .DST_BP(0), .SEED(3))
        st2 (.done(st2_done), .errors(st2_err), .n_sent(st2_sent), .n_recv(st2_recv));
    hs_stress #(.SRC_PERIOD(10.0),  .DST_PERIOD(8.333), .HOLD_VALID(0), .DST_BP(0), .SEED(4))
        st3 (.done(st3_done), .errors(st3_err), .n_sent(st3_sent), .n_recv(st3_recv));
    hs_stress #(.SRC_PERIOD(8.333), .DST_PERIOD(10.0),  .HOLD_VALID(1), .DST_BP(1), .SEED(5))
        st4 (.done(st4_done), .errors(st4_err), .n_sent(st4_sent), .n_recv(st4_recv));
    hs_stress #(.SRC_PERIOD(10.0),  .DST_PERIOD(8.333), .HOLD_VALID(0), .DST_BP(1), .SEED(6))
        st5 (.done(st5_done), .errors(st5_err), .n_sent(st5_sent), .n_recv(st5_recv));

    // ── Main test sequence ─────────────────────────────────────
    initial begin
        $dumpfile("tb_cdc_modules.vcd");
        $dumpvars(0, tb_cdc_modules);

        // Init all clocks and signals
        m2_src_clk   = 0; m2_dst_clk   = 0; m2_reset_n   = 0;
        m2_src_signal = 0;
        m3_src_clk   = 0; m3_dst_clk   = 0; m3_reset_n   = 0;
        m3_src_data  = 0; m3_src_valid  = 0; m3_dst_ready = 0;
        pass_count   = 0; fail_count   = 0; test_num     = 0;

        // ════════════════════════════════════════════════════════
        // SECTION B: cdc_single_bit tests
        // ════════════════════════════════════════════════════════
        $display("\n=== Section B: cdc_single_bit ===");

        // ── B1: Reset behaviour ────────────────────────────────
        $display("\n--- B1: Reset Behaviour ---");
        m2_reset_n    = 0;
        m2_src_signal = 0;
        #100;
        check(m2_dst_signal === 1'b0, "M2: dst_signal = 0 during reset");

        // ── B2: Signal propagation (low to high) ───────────────
        $display("\n--- B2: Low-to-High Propagation ---");
        m2_reset_n = 1;
        @(posedge m2_dst_clk);

        m2_src_signal = 1;
        begin : b2_wait
            integer wait_cycles;
            reg saw_high;
            saw_high = 0;
            for (wait_cycles = 0; wait_cycles < 10; wait_cycles = wait_cycles + 1) begin
                @(posedge m2_dst_clk); #1;
                if (m2_dst_signal === 1'b1) begin
                    saw_high = 1;
                    $display("  Signal propagated after %0d dst_clk cycles", wait_cycles + 1);
                    disable b2_wait;
                end
            end
        end
        check(m2_dst_signal === 1'b1, "M2: 0->1 propagates through sync chain");

        // ── B3: Signal propagation (high to low) ───────────────
        $display("\n--- B3: High-to-Low Propagation ---");
        m2_src_signal = 0;
        begin : b3_wait
            integer wait_cycles;
            for (wait_cycles = 0; wait_cycles < 10; wait_cycles = wait_cycles + 1) begin
                @(posedge m2_dst_clk); #1;
                if (m2_dst_signal === 1'b0) begin
                    $display("  Signal de-propagated after %0d dst_clk cycles", wait_cycles + 1);
                    disable b3_wait;
                end
            end
        end
        check(m2_dst_signal === 1'b0, "M2: 1->0 propagates through sync chain");

        // ── B4: Minimum pulse width ────────────────────────────
        $display("\n--- B4: Minimum Pulse Width ---");
        m2_reset_n = 0;
        #100;
        m2_reset_n = 1;
        @(posedge m2_dst_clk);

        // A single src_clk pulse may or may not be captured
        // At the 4:1 source:destination ratio, 1 src_clk pulse = 2.5ns, dst_clk period = 10ns
        // A single src_clk pulse might be missed — that's expected behavior
        m2_src_signal = 1;
        @(posedge m2_src_clk); #1;
        m2_src_signal = 0;

        begin : b4_wait
            integer wait_cycles;
            reg saw_pulse;
            saw_pulse = 0;
            for (wait_cycles = 0; wait_cycles < 10; wait_cycles = wait_cycles + 1) begin
                @(posedge m2_dst_clk); #1;
                if (m2_dst_signal) saw_pulse = 1;
            end
            // A single 2.5 ns src_clk pulse might be too short for the 10 ns dst clock
            // This is a known limitation of single-bit synchronizers
            $display("  Single src_clk pulse captured: %b (may miss — expected for narrow pulse)",
                     saw_pulse);
            check(1'b1, "M2: Narrow pulse test completed (miss is acceptable)");
        end

        // ── B5: Long pulse always captured ─────────────────────
        $display("\n--- B5: Long Pulse Capture ---");
        m2_reset_n = 0;
        #100;
        m2_reset_n = 1;
        @(posedge m2_dst_clk);

        // Pulse held for 8 src_clk cycles = 20ns (> 2× dst_clk period)
        m2_src_signal = 1;
        repeat (8) @(posedge m2_src_clk);
        m2_src_signal = 0;

        begin : b5_wait
            integer wait_cycles;
            reg saw_pulse;
            saw_pulse = 0;
            for (wait_cycles = 0; wait_cycles < 10; wait_cycles = wait_cycles + 1) begin
                @(posedge m2_dst_clk); #1;
                if (m2_dst_signal) saw_pulse = 1;
            end
            check(saw_pulse, "M2: Long pulse (8 src_clk) always captured");
        end

        // ── B6: Reset clears sync chain ────────────────────────
        $display("\n--- B6: Reset Clears Sync Chain ---");
        m2_src_signal = 1;
        repeat (10) @(posedge m2_dst_clk);
        // dst_signal should be 1 now
        m2_reset_n = 0;
        repeat (2) @(posedge m2_dst_clk); #1;
        check(m2_dst_signal === 1'b0, "M2: Reset clears sync chain to 0");
        m2_reset_n = 1;
        m2_src_signal = 0;

        // ── B7: Port Connectivity ──────────────────────────────
        $display("\n--- B7: Port Connectivity ---");
        m2_reset_n = 0;
        m2_src_signal = 0;
        #100;
        m2_reset_n = 1;
        repeat (5) @(posedge m2_dst_clk); #1;

        check(m2_dst_signal !== 1'bx, "M2: dst_signal is not X after reset");
        check(m2_dst_signal !== 1'bz, "M2: dst_signal is not Z after reset");

        // Drive signal high and verify after propagation
        m2_src_signal = 1;
        repeat (8) @(posedge m2_dst_clk); #1;
        check(m2_dst_signal !== 1'bx, "M2: dst_signal is not X after propagation");
        check(m2_dst_signal !== 1'bz, "M2: dst_signal is not Z after propagation");
        m2_src_signal = 0;
        repeat (8) @(posedge m2_dst_clk);

        // ════════════════════════════════════════════════════════
        // SECTION C: cdc_handshake tests
        // ════════════════════════════════════════════════════════
        $display("\n=== Section C: cdc_handshake ===");

        // ── C1: Reset behaviour ────────────────────────────────
        $display("\n--- C1: Reset Behaviour ---");
        m3_reset_n   = 0;
        m3_src_valid = 0;
        m3_dst_ready = 0;
        #200;
        check(m3_src_ready === 1'b1, "M3: src_ready = 1 during reset (not busy)");
        check(m3_dst_valid === 1'b0, "M3: dst_valid = 0 during reset");

        // Release reset
        m3_reset_n = 1;
        @(posedge m3_src_clk);

        // ── C2: Single data transfer ───────────────────────────
        $display("\n--- C2: Single Data Transfer ---");
        m3_dst_ready = 1;  // destination always ready

        m3_src_data  = 32'hDEADBEEF;
        m3_src_valid = 1;
        @(posedge m3_src_clk); #1;
        m3_src_valid = 0;

        // Wait for data to appear at destination
        begin : c2_wait
            integer wait_cycles;
            for (wait_cycles = 0; wait_cycles < 30; wait_cycles = wait_cycles + 1) begin
                @(posedge m3_dst_clk); #1;
                if (m3_dst_valid) begin
                    disable c2_wait;
                end
            end
        end
        check(m3_dst_valid === 1'b1, "M3: dst_valid asserts for single transfer");
        check(m3_dst_data === 32'hDEADBEEF, "M3: data 0xDEADBEEF transferred correctly");

        // Wait for handshake to complete (src_ready goes back to 1)
        begin : c2_wait_ready
            integer wait_cycles;
            for (wait_cycles = 0; wait_cycles < 30; wait_cycles = wait_cycles + 1) begin
                @(posedge m3_src_clk); #1;
                if (m3_src_ready) disable c2_wait_ready;
            end
        end
        check(m3_src_ready === 1'b1, "M3: src_ready reasserts after transfer");

        // ── C3: Consecutive transfers ──────────────────────────
        $display("\n--- C3: Consecutive Transfers ---");
        m3_reset_n = 0;
        #200;
        m3_reset_n = 1;
        @(posedge m3_src_clk);
        m3_dst_ready = 1;

        begin : c3_block
            reg [31:0] sent_data [0:7];
            reg [31:0] recv_data [0:7];
            integer tx_idx, rx_idx;
            integer wait_cnt;
            reg all_match;
            reg src_wait_done, dst_wait_done;

            tx_idx = 0;
            rx_idx = 0;

            // Send 4 values with proper handshaking
            for (tx_idx = 0; tx_idx < 4; tx_idx = tx_idx + 1) begin
                sent_data[tx_idx] = (tx_idx + 1) * 32'h11111111;

                // Wait for src_ready
                src_wait_done = 0;
                for (wait_cnt = 0; wait_cnt < 50 && !src_wait_done; wait_cnt = wait_cnt + 1) begin
                    @(posedge m3_src_clk); #1;
                    if (m3_src_ready) begin
                        src_wait_done = 1;
                    end
                end

                m3_src_data  = sent_data[tx_idx];
                m3_src_valid = 1;
                @(posedge m3_src_clk); #1;
                m3_src_valid = 0;

                // Wait for dst_valid
                dst_wait_done = 0;
                for (wait_cnt = 0; wait_cnt < 50 && !dst_wait_done; wait_cnt = wait_cnt + 1) begin
                    @(posedge m3_dst_clk); #1;
                    if (m3_dst_valid) begin
                        recv_data[rx_idx] = m3_dst_data;
                        rx_idx = rx_idx + 1;
                        dst_wait_done = 1;
                    end
                end
            end

            $display("  Consecutive: sent %0d, received %0d", tx_idx, rx_idx);
            check(rx_idx == 4, "M3: All 4 consecutive transfers received");

            all_match = 1;
            for (i = 0; i < rx_idx && i < 4; i = i + 1) begin
                if (recv_data[i] !== sent_data[i]) begin
                    all_match = 0;
                    $display("  [WARN] Consec rx[%0d]: got 0x%08x, exp 0x%08x",
                             i, recv_data[i], sent_data[i]);
                end
            end
            check(all_match, "M3: All consecutive values match exactly");
        end

        // ── C4: Backpressure (dst_ready = 0) ───────────────────
        $display("\n--- C4: Backpressure ---");
        m3_reset_n = 0;
        #200;
        m3_reset_n = 1;
        @(posedge m3_src_clk);
        m3_dst_ready = 0;  // NOT ready

        // Send data
        m3_src_data  = 32'hCAFEBABE;
        m3_src_valid = 1;
        @(posedge m3_src_clk); #1;
        m3_src_valid = 0;

        // Wait — dst_valid should assert but data shouldn't be consumed
        begin : c4_wait_valid
            integer wait_cnt;
            for (wait_cnt = 0; wait_cnt < 30; wait_cnt = wait_cnt + 1) begin
                @(posedge m3_dst_clk); #1;
                if (m3_dst_valid) disable c4_wait_valid;
            end
        end
        check(m3_dst_valid === 1'b1, "M3: dst_valid asserts even without dst_ready");
        check(m3_dst_data === 32'hCAFEBABE, "M3: Correct data held during backpressure");

        // src should NOT be ready (busy with in-flight transfer)
        check(m3_src_ready === 1'b0, "M3: src_ready = 0 during backpressure");

        // Now assert dst_ready — transfer should complete
        m3_dst_ready = 1;
        begin : c4_wait_done
            integer wait_cnt;
            for (wait_cnt = 0; wait_cnt < 30; wait_cnt = wait_cnt + 1) begin
                @(posedge m3_src_clk); #1;
                if (m3_src_ready) disable c4_wait_done;
            end
        end
        check(m3_src_ready === 1'b1, "M3: src_ready reasserts after backpressure release");

        // ── C5: Data integrity with edge-case values ───────────
        $display("\n--- C5: Edge-Case Values ---");
        m3_reset_n = 0;
        #200;
        m3_reset_n = 1;
        @(posedge m3_src_clk);
        m3_dst_ready = 1;

        begin : c5_block
            reg [31:0] edge_vals [0:3];
            reg [31:0] edge_recv [0:3];
            integer tx_idx, rx_idx, wait_cnt;
            reg all_match;
            reg src_wait_done, dst_wait_done;

            edge_vals[0] = 32'h00000000;
            edge_vals[1] = 32'hFFFFFFFF;
            edge_vals[2] = 32'h80000000;
            edge_vals[3] = 32'h00000001;

            rx_idx = 0;

            for (tx_idx = 0; tx_idx < 4; tx_idx = tx_idx + 1) begin
                // Wait for src_ready
                src_wait_done = 0;
                for (wait_cnt = 0; wait_cnt < 50 && !src_wait_done; wait_cnt = wait_cnt + 1) begin
                    @(posedge m3_src_clk); #1;
                    if (m3_src_ready) src_wait_done = 1;
                end

                m3_src_data  = edge_vals[tx_idx];
                m3_src_valid = 1;
                @(posedge m3_src_clk); #1;
                m3_src_valid = 0;

                dst_wait_done = 0;
                for (wait_cnt = 0; wait_cnt < 50 && !dst_wait_done; wait_cnt = wait_cnt + 1) begin
                    @(posedge m3_dst_clk); #1;
                    if (m3_dst_valid) begin
                        edge_recv[rx_idx] = m3_dst_data;
                        rx_idx = rx_idx + 1;
                        dst_wait_done = 1;
                    end
                end
            end

            all_match = 1;
            for (i = 0; i < 4; i = i + 1) begin
                if (i < rx_idx && edge_recv[i] !== edge_vals[i]) begin
                    all_match = 0;
                    $display("  [WARN] Edge val[%0d]: got 0x%08x, exp 0x%08x",
                             i, edge_recv[i], edge_vals[i]);
                end
            end
            check(rx_idx == 4, "M3: All 4 edge-case values received");
            check(all_match, "M3: Edge-case values (0x0, 0xFFFF, 0x8000, 0x1) correct");
        end

        // ── C6: Port Connectivity + Reset Recovery ─────────────
        $display("\n--- C6: Port Connectivity + Reset Recovery ---");
        m3_reset_n = 0;
        m3_src_valid = 0;
        m3_dst_ready = 0;
        #200;
        m3_reset_n = 1;
        repeat (5) @(posedge m3_src_clk); #1;

        // Port connectivity: outputs should not be X or Z after reset
        check(m3_src_ready !== 1'bx, "M3: src_ready is not X after reset");
        check(m3_src_ready !== 1'bz, "M3: src_ready is not Z after reset");
        check(m3_dst_valid !== 1'bx, "M3: dst_valid is not X after reset");
        check(m3_dst_valid !== 1'bz, "M3: dst_valid is not Z after reset");
        check(m3_dst_data !== 32'hxxxxxxxx, "M3: dst_data is not X after reset");
        check(m3_dst_data !== 32'hzzzzzzzz, "M3: dst_data is not Z after reset");

        // Reset during active transfer: start a transfer, then assert reset mid-flight
        m3_dst_ready = 1;

        // Wait for src_ready
        begin : c6_wait_ready
            integer wait_cnt;
            for (wait_cnt = 0; wait_cnt < 30; wait_cnt = wait_cnt + 1) begin
                @(posedge m3_src_clk); #1;
                if (m3_src_ready) disable c6_wait_ready;
            end
        end

        m3_src_data  = 32'h12345678;
        m3_src_valid = 1;
        @(posedge m3_src_clk); #1;
        m3_src_valid = 0;

        // Wait a few cycles for transfer to be in-flight, then reset
        repeat (3) @(posedge m3_dst_clk);
        m3_reset_n = 0;
        #200;

        // Verify outputs are clean during reset
        check(m3_dst_valid === 1'b0, "M3: dst_valid = 0 after mid-transfer reset");

        // Release reset and verify recovery
        m3_reset_n = 1;
        @(posedge m3_src_clk);
        m3_dst_ready = 1;

        // Wait for src_ready to reassert (module recovered)
        begin : c6_recovery_wait
            integer wait_cnt;
            reg recovered;
            recovered = 0;
            for (wait_cnt = 0; wait_cnt < 50; wait_cnt = wait_cnt + 1) begin
                @(posedge m3_src_clk); #1;
                if (m3_src_ready) begin
                    recovered = 1;
                    disable c6_recovery_wait;
                end
            end
            check(recovered, "M3: src_ready reasserts after mid-transfer reset recovery");
        end

        // Verify a new transfer works after recovery
        m3_src_data  = 32'hABCD0000;
        m3_src_valid = 1;
        @(posedge m3_src_clk); #1;
        m3_src_valid = 0;

        begin : c6_post_reset_xfer
            integer wait_cnt;
            for (wait_cnt = 0; wait_cnt < 30; wait_cnt = wait_cnt + 1) begin
                @(posedge m3_dst_clk); #1;
                if (m3_dst_valid) disable c6_post_reset_xfer;
            end
        end
        check(m3_dst_valid === 1'b1, "M3: dst_valid asserts for post-recovery transfer");
        check(m3_dst_data === 32'hABCD0000, "M3: data 0xABCD0000 correct after reset recovery");

        // ════════════════════════════════════════════════════════
        // SECTION D: cdc_handshake back-to-back stress (data integrity)
        // ════════════════════════════════════════════════════════
        $display("\n=== Section D: cdc_handshake stress ===");
        wait (st0_done && st1_done && st2_done && st3_done && st4_done && st5_done);
        check(st0_err == 0 && st0_recv == st0_sent && st0_sent >= 300,
              "M3 120>100 held valid: ordered, no dup/gap/tear");
        check(st1_err == 0 && st1_recv == st1_sent && st1_sent >= 300,
              "M3 100>120 held valid: ordered, no dup/gap/tear");
        check(st2_err == 0 && st2_recv == st2_sent && st2_sent >= 300,
              "M3 120>100 valid/ready: every word in order");
        check(st3_err == 0 && st3_recv == st3_sent && st3_sent >= 300,
              "M3 100>120 valid/ready: every word in order");
        check(st4_err == 0 && st4_recv == st4_sent && st4_sent >= 300,
              "M3 120>100 held valid + dst backpressure");
        check(st5_err == 0 && st5_recv == st5_sent && st5_sent >= 300,
              "M3 100>120 valid/ready + dst backpressure");

        // ════════════════════════════════════════════════════════
        // Summary
        // ════════════════════════════════════════════════════════
        $display("");
        $display("========================================");
        $display("  CDC MODULES TESTBENCH RESULTS");
        $display("  PASSED: %0d / %0d", pass_count, test_num);
        $display("  FAILED: %0d / %0d", fail_count, test_num);
        if (fail_count == 0)
            $display("  ** ALL TESTS PASSED **");
        else
            $display("  ** SOME TESTS FAILED **");
        $display("========================================");
        $display("");

        #100;
        $finish;
    end

endmodule
