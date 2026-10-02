`timescale 1ns / 1ps
// tb_ref_spectrum_rom.v — every ROM word reads back the .mem contents with 1-cycle latency
module tb_ref_spectrum_rom;
    localparam N_FFT = 256, LOG2N = 8, N_SEG = 5, DEPTH = N_SEG * N_FFT;
    reg clk;
    reg  [LOG2N+2:0] addr;
    wire signed [15:0] dout_i, dout_q;
    reg [15:0] exp_i [0:DEPTH-1];
    reg [15:0] exp_q [0:DEPTH-1];
    integer a, mismatches, nonzero_long, nonzero_short, pass_count, fail_count, test_num;

    always #5 clk = ~clk;

    ref_spectrum_rom #(.N_FFT(N_FFT), .LOG2N(LOG2N), .N_SEG(N_SEG), .DATA_W(16)) uut (
        .clk(clk), .addr(addr), .dout_i(dout_i), .dout_q(dout_q));

    task check;
        input cond; input [511:0] label;
        begin
            test_num = test_num + 1;
            if (cond) begin $display("[PASS] Test %0d: %0s", test_num, label); pass_count = pass_count + 1; end
            else       begin $display("[FAIL] Test %0d: %0s", test_num, label); fail_count = fail_count + 1; end
        end
    endtask

    initial begin
        $readmemh("ref_spectrum_i.mem", exp_i);
        $readmemh("ref_spectrum_q.mem", exp_q);
        clk = 0; addr = 0; mismatches = 0; nonzero_long = 0; nonzero_short = 0;
        pass_count = 0; fail_count = 0; test_num = 0;
        for (a = 0; a < DEPTH; a = a + 1) begin
            addr = a; @(posedge clk); #1;                 // registered read: data valid now
            if (dout_i !== $signed(exp_i[a]) || dout_q !== $signed(exp_q[a])) mismatches = mismatches + 1;
            if (exp_i[a] != 0 || exp_q[a] != 0) begin
                if (a < 4 * N_FFT) nonzero_long = nonzero_long + 1; else nonzero_short = nonzero_short + 1;
            end
        end
        check(mismatches == 0, "all 1280 words match the .mem files (1-cycle latency)");
        check(nonzero_long > 900, "long segments are dense (> 900 non-zero bins of 1024)");
        check(nonzero_short > 200, "short segment is dense (> 200 non-zero bins of 256)");
        $display("\nResults: %0d/%0d passed", pass_count, pass_count + fail_count);
        $finish;
    end
endmodule
