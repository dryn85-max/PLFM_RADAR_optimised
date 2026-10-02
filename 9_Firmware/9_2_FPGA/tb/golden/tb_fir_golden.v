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
