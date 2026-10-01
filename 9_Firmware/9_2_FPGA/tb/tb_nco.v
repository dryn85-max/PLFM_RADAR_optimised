`timescale 1ns / 1ps
// tb_nco.v — frequency, quadrature and phase-offset checks for nco.v
module tb_nco;
    localparam CLK_PERIOD = 10.0;                 // 100 MHz
    localparam [31:0] FTW_20MHZ = 32'h3333_3333;  // 0.2 * 2^32
    localparam [31:0] FTW_1MHZ  = 32'h028F_5C29;  // 0.01 * 2^32

    reg clk, reset_n, phase_valid;
    wire signed [15:0] sin20, cos20, sin1, cos1, sin20o, cos20o;
    wire ready20, ready1, ready20o;
    integer pass_count, fail_count, test_num, i;
    integer zc20, zc1, mismatch;
    reg signed [15:0] prev20, prev1;
    integer mag_sq, mag_min, mag_max;

    always #(CLK_PERIOD/2) clk = ~clk;

    nco #(.PHASE_INC(FTW_20MHZ), .DITHER_EN(0)) u20 (
        .clk(clk), .reset_n(reset_n), .phase_valid(phase_valid), .phase_offset(16'h0000),
        .sin_out(sin20), .cos_out(cos20), .dds_ready(ready20));
    nco #(.PHASE_INC(FTW_1MHZ), .DITHER_EN(0)) u1 (
        .clk(clk), .reset_n(reset_n), .phase_valid(phase_valid), .phase_offset(16'h0000),
        .sin_out(sin1), .cos_out(cos1), .dds_ready(ready1));
    nco #(.PHASE_INC(FTW_20MHZ), .DITHER_EN(0)) u20o (          // +90 degrees
        .clk(clk), .reset_n(reset_n), .phase_valid(phase_valid), .phase_offset(16'h4000),
        .sin_out(sin20o), .cos_out(cos20o), .dds_ready(ready20o));

    task check;
        input cond; input [511:0] label;
        begin
            test_num = test_num + 1;
            if (cond) begin $display("[PASS] Test %0d: %0s", test_num, label); pass_count = pass_count + 1; end
            else       begin $display("[FAIL] Test %0d: %0s", test_num, label); fail_count = fail_count + 1; end
        end
    endtask

    initial begin
        clk = 0; reset_n = 0; phase_valid = 0;
        pass_count = 0; fail_count = 0; test_num = 0;
        zc20 = 0; zc1 = 0; mismatch = 0; mag_min = 32'h7FFFFFFF; mag_max = 0;
        repeat (3) @(posedge clk); #1;
        check(ready20 === 1'b0, "dds_ready = 0 in reset");
        check(cos20 === 16'sh7FFF && sin20 === 16'sh0000, "reset phase 0: cos=0x7FFF sin=0");
        reset_n = 1; phase_valid = 1;
        repeat (8) @(posedge clk); #1;
        check(ready20 === 1'b1, "dds_ready asserts after pipeline fill");

        // 1000 cycles: 20 MHz -> 200 periods, 1 MHz -> 10 periods
        prev20 = sin20; prev1 = sin1;
        for (i = 0; i < 1000; i = i + 1) begin
            @(posedge clk); #1;
            if (prev20 < 0 && sin20 >= 0) zc20 = zc20 + 1;
            if (prev1  < 0 && sin1  >= 0) zc1  = zc1  + 1;
            prev20 = sin20; prev1 = sin1;
            mag_sq = sin20 * sin20 + cos20 * cos20;
            if (mag_sq < mag_min) mag_min = mag_sq;
            if (mag_sq > mag_max) mag_max = mag_sq;
            if (sin20o !== cos20) mismatch = mismatch + 1;
        end
        $display("zero crossings: 20MHz=%0d (exp 200), 1MHz=%0d (exp 10)", zc20, zc1);
        $display("sin^2+cos^2 range: %0d .. %0d (32767^2 = %0d)", mag_min, mag_max, 32767*32767);
        check(zc20 >= 199 && zc20 <= 201, "20 MHz: 200 +/-1 rising zero crossings in 1000 cycles");
        check(zc1 >= 9 && zc1 <= 11,      "1 MHz: 10 +/-1 rising zero crossings in 1000 cycles");
        check(mag_min > (32767*32767/100)*96 && mag_max < (32767*32767/100)*104,
              "quadrature: sin^2+cos^2 within 4% of full scale");
        check(mismatch == 0, "offset 0x4000 (+90deg): sin == unshifted cos, exact");

        // phase_valid gating freezes the output
        phase_valid = 0; repeat (6) @(posedge clk); #1;
        prev20 = sin20; repeat (10) @(posedge clk); #1;
        check(sin20 === prev20, "phase_valid=0 freezes the output");

        $display("\nResults: %0d/%0d passed", pass_count, pass_count + fail_count);
        $finish;
    end
endmodule
