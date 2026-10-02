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
    reg signed [63:0] dc_exp;   // 64-bit: 10000*coef_sum overflows 32 bits

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
        dc_exp = (64'sd10000 * coef_sum) >>> 17;
        $display("DC: last_out=%0d expected=%0d (coef_sum=%0d)", last_out, dc_exp, coef_sum);
        check(out_count == 64, "one output per input (64)");
        check(last_out == dc_exp[15:0], "DC gain = sum(c) / 2^17 exactly");

        // Saturation: full-scale DC must saturate, flag asserted
        out_count = 0;
        for (i = 0; i < 64; i = i + 1) feed(16'sd32767);
        repeat (10) @(posedge clk); #1;
        check(last_out == 16'sd32767 && filter_overflow === 1'b1,
              "full-scale DC saturates to 32767, overflow flag set");

        // Cadence: fir_ready pattern 0,0,0,1 after a sample
        data_in = 0; data_valid = 1; @(posedge clk); #1; data_valid = 0;
        check(fir_ready === 1'b0, "fir_ready low on phase 0");
        @(posedge clk); #1; @(posedge clk); #1; @(posedge clk); #1;
        check(fir_ready === 1'b1, "fir_ready high on phase 3 (next sample may be accepted)");

        $display("\nResults: %0d/%0d passed", pass_count, pass_count + fail_count);
        $finish;
    end
endmodule
