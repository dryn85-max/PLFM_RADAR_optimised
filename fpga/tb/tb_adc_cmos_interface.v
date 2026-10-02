`timescale 1ns / 1ps
// tb_adc_cmos_interface.v — unit test for the 12-bit CMOS ADC capture
module tb_adc_cmos_interface;
    localparam CLK_PERIOD = 10.0;   // 100 MHz ADC DCO
    reg         clk;
    reg         reset_n;
    reg  [11:0] adc_data;
    reg         adc_ovr;
    reg         overrange_clear;
    wire [11:0] sample;
    wire        sample_valid;
    wire        overrange;

    integer pass_count, fail_count, test_num, i;

    always #(CLK_PERIOD/2) clk = ~clk;

    adc_cmos_interface #(.DATA_W(12)) uut (
        .adc_clk(clk), .reset_n(reset_n),
        .adc_data(adc_data), .adc_ovr(adc_ovr), .overrange_clear(overrange_clear),
        .sample(sample), .sample_valid(sample_valid), .overrange(overrange)
    );

    task check;
        input cond;
        input [511:0] label;
        begin
            test_num = test_num + 1;
            if (cond) begin $display("[PASS] Test %0d: %0s", test_num, label); pass_count = pass_count + 1; end
            else       begin $display("[FAIL] Test %0d: %0s", test_num, label); fail_count = fail_count + 1; end
        end
    endtask

    initial begin
        clk = 0; reset_n = 0; adc_data = 12'h800; adc_ovr = 0; overrange_clear = 0;
        pass_count = 0; fail_count = 0; test_num = 0;

        // --- Group 1: reset ---
        repeat (3) @(posedge clk); #1;
        check(sample_valid === 1'b0, "sample_valid = 0 in reset");
        check(overrange === 1'b0,    "overrange = 0 in reset");
        reset_n = 1;

        // --- Group 2: ramp with 2-cycle latency ---
        for (i = 0; i < 20; i = i + 1) begin
            adc_data = 12'd100 + i;
            @(posedge clk); #1;
        end
        // After the loop: adc_data was 119 at the last edge; sample shows 118 (presented 2 edges before)
        check(sample === 12'd118, "sample lags adc_data by exactly 2 edges");
        check(sample_valid === 1'b1, "sample_valid = 1 after reset release");

        // --- Group 3: overrange sticky + clear ---
        adc_ovr = 1; @(posedge clk); #1; adc_ovr = 0;
        @(posedge clk); #1;
        check(overrange === 1'b1, "overrange sets 2 edges after adc_ovr pulse");
        repeat (5) @(posedge clk); #1;
        check(overrange === 1'b1, "overrange is sticky");
        overrange_clear = 1; @(posedge clk); #1; overrange_clear = 0; @(posedge clk); #1;
        check(overrange === 1'b0, "overrange_clear clears the flag");

        // --- Group 4: clear and set in the same cycle -> set wins ---
        adc_ovr = 1; @(posedge clk); #1; adc_ovr = 0;      // captured into adc_ovr_q1
        overrange_clear = 1; @(posedge clk); #1; overrange_clear = 0;  // same edge as the set
        check(overrange === 1'b1, "set has priority over clear");

        // --- Group 5: reset clears everything ---
        reset_n = 0; @(posedge clk); #1;
        check(overrange === 1'b0 && sample_valid === 1'b0, "async reset clears flags");

        $display("\nResults: %0d/%0d passed", pass_count, pass_count + fail_count);
        $finish;
    end
endmodule
