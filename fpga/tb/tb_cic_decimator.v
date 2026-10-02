`timescale 1ns / 1ps
// tb_cic_decimator.v — 5-stage CIC, R=4, Hogenauer widths (DATA_W=16)
module tb_cic_decimator;
    localparam CLK_PERIOD = 10.0;  // 100 MHz
    reg clk, reset_n;
    reg  signed [15:0] data_in;
    reg         data_valid;
    wire signed [15:0] data_out;
    wire        data_out_valid;
    integer pass_count, fail_count, test_num, i, out_count;
    reg signed [15:0] last_out;
    integer impulse_sum;
    integer imp_out [0:7];

    always #(CLK_PERIOD/2) clk = ~clk;

    cic_decimator_4x_enhanced #(.DATA_W(16), .STAGES(5), .LOG2_R(2)) uut (
        .clk(clk), .reset_n(reset_n), .data_in(data_in), .data_valid(data_valid),
        .data_out(data_out), .data_out_valid(data_out_valid));

    task check;
        input cond; input [511:0] label;
        begin
            test_num = test_num + 1;
            if (cond) begin $display("[PASS] Test %0d: %0s", test_num, label); pass_count = pass_count + 1; end
            else       begin $display("[FAIL] Test %0d: %0s", test_num, label); fail_count = fail_count + 1; end
        end
    endtask

    always @(posedge clk) if (data_out_valid) begin
        if (out_count < 8) imp_out[out_count] = data_out;
        out_count = out_count + 1; last_out = data_out; impulse_sum = impulse_sum + data_out;
    end

    task reset_dut;
        begin
            reset_n = 0; data_valid = 0; data_in = 0; out_count = 0; last_out = 0; impulse_sum = 0;
            for (i = 0; i < 8; i = i + 1) imp_out[i] = 0;
            repeat (3) @(posedge clk); #1; reset_n = 1; @(posedge clk); #1;
        end
    endtask

    initial begin
        clk = 0; pass_count = 0; fail_count = 0; test_num = 0;
        reset_dut;
        check(data_out_valid === 1'b0, "no output in reset");

        // --- DC: 400 samples of +12345 -> steady-state output == input exactly (gain 4^5 / 2^10 = 1) ---
        data_valid = 1;
        for (i = 0; i < 400; i = i + 1) begin data_in = 16'sd12345; @(posedge clk); #1; end
        data_valid = 0; repeat (10) @(posedge clk); #1;
        check(out_count == 100, "R=4: 400 inputs -> 100 outputs");
        check(last_out === 16'sd12345, "DC gain exactly 1 after >>10 (steady state)");

        // --- Negative full scale: no wrap ---
        reset_dut; data_valid = 1;
        for (i = 0; i < 400; i = i + 1) begin data_in = -16'sd32768; @(posedge clk); #1; end
        data_valid = 0; repeat (10) @(posedge clk); #1;
        check(last_out === -16'sd32768, "DC -32768 passes without wrap (26-bit integrators)");

        // --- Impulse 10000 at n=0: decimated impulse response, exact values from the
        //     integer model (Delta^5 S5[4j-2] >> 10): 0, 146, 1318, 986, 48, 0 ...
        reset_dut; data_valid = 1;
        data_in = 16'sd10000; @(posedge clk); #1; data_in = 0;
        for (i = 0; i < 199; i = i + 1) begin @(posedge clk); #1; end
        data_valid = 0; repeat (10) @(posedge clk); #1;
        check(out_count == 50, "impulse run: 200 inputs -> 50 outputs");
        check(impulse_sum == 2498, "impulse response sums to 2498 (0+146+1318+986+48, floor >>10)");
        check(imp_out[0] == 0 && imp_out[1] == 146 && imp_out[2] == 1318 &&
              imp_out[3] == 986 && imp_out[4] == 48 && imp_out[5] == 0,
              "impulse response taps exact: 0,146,1318,986,48,0");

        // --- data_valid gaps: outputs only count valid inputs ---
        reset_dut;
        for (i = 0; i < 80; i = i + 1) begin
            data_valid = (i % 2 == 0); data_in = 16'sd1000; @(posedge clk); #1;
        end
        data_valid = 0; repeat (10) @(posedge clk); #1;
        check(out_count == 10, "40 valid inputs with gaps -> 10 outputs");

        $display("\nResults: %0d/%0d passed", pass_count, pass_count + fail_count);
        $finish;
    end
endmodule
