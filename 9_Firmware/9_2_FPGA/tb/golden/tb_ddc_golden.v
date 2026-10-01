`timescale 1ns / 1ps
// ============================================================================
// tb_ddc_golden.v — golden test (a): ddc.v vs the integer model in
// tb/golden/gen_ddc_golden.py.  Exact match required (tolerance 0).
// DITHER_EN = 0 so the NCO phase of sample n is PHASE_INC*n.
// ============================================================================
module tb_ddc_golden;
    localparam CLK_PERIOD = 10.0;
    localparam N_IN   = 4096;
    localparam N_OUT  = N_IN / 4;
    localparam OUT_W  = 16;

    reg clk, reset_n, adc_valid;
    reg  [11:0] adc_data;
    wire signed [OUT_W-1:0] bb_i, bb_q;
    wire bb_valid;

    reg [11:0]      adc_mem   [0:N_IN-1];
    reg [OUT_W-1:0] gold_i    [0:N_OUT-1];
    reg [OUT_W-1:0] gold_q    [0:N_OUT-1];
    integer out_count, mismatches, first_bad, i;
    integer pass_count, fail_count, test_num;

    always #(CLK_PERIOD/2) clk = ~clk;

    ddc #(.ADC_W(12), .OUT_W(OUT_W), .DITHER_EN(0)) uut (
        .clk(clk), .reset_n(reset_n), .mixers_enable(1'b1),
        .adc_data(adc_data), .adc_valid(adc_valid),
        .baseband_i(bb_i), .baseband_q(bb_q), .baseband_valid(bb_valid));

    task check;
        input cond; input [511:0] label;
        begin
            test_num = test_num + 1;
            if (cond) begin $display("[PASS] Test %0d: %0s", test_num, label); pass_count = pass_count + 1; end
            else       begin $display("[FAIL] Test %0d: %0s", test_num, label); fail_count = fail_count + 1; end
        end
    endtask

    always @(posedge clk) begin
        if (reset_n && bb_valid) begin
            if (out_count < N_OUT) begin
                if (bb_i !== $signed(gold_i[out_count]) || bb_q !== $signed(gold_q[out_count])) begin
                    if (mismatches < 10)
                        $display("  mismatch @%0d: rtl=(%0d,%0d) gold=(%0d,%0d)", out_count,
                                 bb_i, bb_q, $signed(gold_i[out_count]), $signed(gold_q[out_count]));
                    if (first_bad < 0) first_bad = out_count;
                    mismatches = mismatches + 1;
                end
            end
            out_count = out_count + 1;
        end
    end

    initial begin
        $readmemh("tb/golden/ddc_adc_in.hex", adc_mem);
        $readmemh("tb/golden/ddc_golden_i.hex", gold_i);
        $readmemh("tb/golden/ddc_golden_q.hex", gold_q);
        clk = 0; reset_n = 0; adc_valid = 0; adc_data = 12'h800;
        out_count = 0; mismatches = 0; first_bad = -1;
        pass_count = 0; fail_count = 0; test_num = 0;
        repeat (4) @(posedge clk); #1;
        reset_n = 1;
        repeat (4) @(posedge clk); #1;          // let the mixers_enable synchronizer settle
        for (i = 0; i < N_IN; i = i + 1) begin
            adc_data = adc_mem[i]; adc_valid = 1'b1;
            @(posedge clk); #1;
        end
        adc_valid = 1'b0; adc_data = 12'h800;
        repeat (60) @(posedge clk); #1;        // drain NCO(4)+mixer(3)+CIC(6)+FIR(7)+out(1)
        $display("outputs=%0d mismatches=%0d first_bad=%0d", out_count, mismatches, first_bad);
        check(out_count == N_OUT, "exactly N_IN/4 baseband samples");
        check(mismatches == 0, "all baseband samples match the integer model exactly");
        $display("\nResults: %0d/%0d passed", pass_count, pass_count + fail_count);
        $finish;
    end
endmodule
