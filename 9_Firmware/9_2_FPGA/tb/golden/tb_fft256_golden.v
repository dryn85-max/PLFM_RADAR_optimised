`timescale 1ns / 1ps
// ============================================================================
// tb_fft256_golden.v — golden test (c): 256-point fft_engine vs numpy FFT.
// Tolerance (see gen_fft_golden.py): forward |err| <= 256 per component and
// sum of squared errors <= N * 32^2 (RMS <= 32); inverse +-4 per component.
// Additionally bit-exact vs tb/cosim/fpga_model.py FFTEngine(internal_w=25),
// after a mid-load reset, and for a full-scale I/Q input whose exact spectrum
// exceeds 2^23 (it wrapped at 24 bits): the output must equal the exact numpy
// FFT saturated to 16 bits (no internal wrap), peak bin = +32767.
// ============================================================================
module tb_fft256_golden;
    localparam N = 256, LOG2N = 8, CLK_PERIOD = 10;
    localparam MAX_ABS = 256;
    localparam MAX_SQ_SUM = N * 32 * 32;
    reg clk, reset_n, start, inverse, din_valid;
    reg  signed [15:0] din_re, din_im;
    wire signed [15:0] dout_re, dout_im;
    wire dout_valid, busy, done;
    reg [15:0] in_i [0:N-1], in_q [0:N-1], fwd_i [0:N-1], fwd_q [0:N-1];
    reg [15:0] mdl_i [0:N-1], mdl_q [0:N-1];      // bit-exact model output, nominal vector
    reg [15:0] fs_in_i [0:N-1], fs_in_q [0:N-1];  // full-scale adversarial input
    reg [15:0] fs_mdl_i [0:N-1], fs_mdl_q [0:N-1];
    reg [15:0] fs_np_i [0:N-1], fs_np_q [0:N-1];  // numpy FFT of the full-scale input, saturated to 16 bit
    integer fs_peak;
    reg use_fs;
    reg signed [15:0] cap_re [0:N-1], cap_im [0:N-1];
    integer mism, cap_count, i, err_re, err_im, max_err, sq_sum, pass_count, fail_count, test_num;

    always #(CLK_PERIOD/2) clk = ~clk;

    fft_engine #(.N(N), .LOG2N(LOG2N), .DATA_W(16), .INTERNAL_W(25), .TWIDDLE_W(16),
                 .TWIDDLE_FILE("fft_twiddle_256.mem")) dut (
        .clk(clk), .reset_n(reset_n), .start(start), .inverse(inverse),
        .din_re(din_re), .din_im(din_im), .din_valid(din_valid),
        .dout_re(dout_re), .dout_im(dout_im), .dout_valid(dout_valid), .busy(busy), .done(done));

    task check;
        input cond; input [511:0] label;
        begin
            test_num = test_num + 1;
            if (cond) begin $display("[PASS] Test %0d: %0s", test_num, label); pass_count = pass_count + 1; end
            else       begin $display("[FAIL] Test %0d: %0s", test_num, label); fail_count = fail_count + 1; end
        end
    endtask

    always @(posedge clk) if (dout_valid && cap_count < N) begin
        cap_re[cap_count] <= dout_re; cap_im[cap_count] <= dout_im; cap_count <= cap_count + 1;
    end

    task run_fft;
        input inv;
        integer k;
        begin
            cap_count = 0;
            @(posedge clk); #1; start = 1; inverse = inv; @(posedge clk); #1; start = 0;
            for (k = 0; k < N; k = k + 1) begin
                din_re = inv ? $signed(fwd_i[k]) : (use_fs ? $signed(fs_in_i[k]) : $signed(in_i[k]));
                din_im = inv ? $signed(fwd_q[k]) : (use_fs ? $signed(fs_in_q[k]) : $signed(in_q[k]));
                din_valid = 1; @(posedge clk); #1;
            end
            din_valid = 0;
            @(posedge done);            // done is a 1-cycle pulse after the last output
            @(posedge clk); #1;
        end
    endtask

    initial begin
        $readmemh("tb/golden/fft256_in_i.hex", in_i);  $readmemh("tb/golden/fft256_in_q.hex", in_q);
        $readmemh("tb/golden/fft256_fwd_i.hex", fwd_i); $readmemh("tb/golden/fft256_fwd_q.hex", fwd_q);
        $readmemh("tb/golden/fft256_mdl_i.hex", mdl_i); $readmemh("tb/golden/fft256_mdl_q.hex", mdl_q);
        $readmemh("tb/golden/fft256_fs_in_i.hex", fs_in_i); $readmemh("tb/golden/fft256_fs_in_q.hex", fs_in_q);
        $readmemh("tb/golden/fft256_fs_mdl_i.hex", fs_mdl_i); $readmemh("tb/golden/fft256_fs_mdl_q.hex", fs_mdl_q);
        $readmemh("tb/golden/fft256_fs_np_i.hex", fs_np_i); $readmemh("tb/golden/fft256_fs_np_q.hex", fs_np_q);
        use_fs = 0;
        clk = 0; reset_n = 0; start = 0; inverse = 0; din_valid = 0; din_re = 0; din_im = 0;
        cap_count = 0; pass_count = 0; fail_count = 0; test_num = 0;
        repeat (3) @(posedge clk); #1; reset_n = 1;

        // ---- forward ----
        run_fft(0);
        max_err = 0; sq_sum = 0;
        for (i = 0; i < N; i = i + 1) begin
            err_re = cap_re[i] - $signed(fwd_i[i]); if (err_re < 0) err_re = -err_re;
            err_im = cap_im[i] - $signed(fwd_q[i]); if (err_im < 0) err_im = -err_im;
            if (err_re > max_err) max_err = err_re;
            if (err_im > max_err) max_err = err_im;
            sq_sum = sq_sum + err_re * err_re + err_im * err_im;
        end
        $display("forward: outputs=%0d max_err=%0d sq_sum=%0d (limits %0d, %0d)", cap_count, max_err, sq_sum, MAX_ABS, MAX_SQ_SUM);
        check(cap_count == N, "forward: 256 outputs");
        check(max_err <= MAX_ABS, "forward: max |err| <= 256 vs numpy");
        check(sq_sum <= MAX_SQ_SUM, "forward: RMS err <= 32 vs numpy");

        // ---- inverse (from the rounded numpy spectrum back to the input) ----
        run_fft(1);
        max_err = 0;
        for (i = 0; i < N; i = i + 1) begin
            err_re = cap_re[i] - $signed(in_i[i]); if (err_re < 0) err_re = -err_re;
            err_im = cap_im[i] - $signed(in_q[i]); if (err_im < 0) err_im = -err_im;
            if (err_re > max_err) max_err = err_re;
            if (err_im > max_err) max_err = err_im;
        end
        $display("inverse: outputs=%0d max_err=%0d", cap_count, max_err);
        check(cap_count == N, "inverse: 256 outputs");
        check(max_err <= 4, "inverse: round trip within +-4");

        // ---- bit-exact vs the Python model (25-bit internal width) ----
        run_fft(0);
        mism = 0;
        for (i = 0; i < N; i = i + 1)
            if (cap_re[i] !== $signed(mdl_i[i]) || cap_im[i] !== $signed(mdl_q[i])) mism = mism + 1;
        check(cap_count == N && mism == 0, "forward: bit-exact vs fpga_model (25-bit)");

        // ---- reset in the middle of a transform, then a clean run ----
        @(posedge clk); #1; start = 1; inverse = 0; @(posedge clk); #1; start = 0;
        for (i = 0; i < N / 2; i = i + 1) begin
            din_re = 16'sd1234; din_im = -16'sd777; din_valid = 1; @(posedge clk); #1;
        end
        din_valid = 0; reset_n = 0; repeat (3) @(posedge clk); #1; reset_n = 1;
        check(!busy, "mid-load reset returns the engine to idle");
        run_fft(0);
        mism = 0;
        for (i = 0; i < N; i = i + 1)
            if (cap_re[i] !== $signed(mdl_i[i]) || cap_im[i] !== $signed(mdl_q[i])) mism = mism + 1;
        check(cap_count == N && mism == 0, "forward after mid-load reset: bit-exact");

        // ---- full-scale I/Q: exact spectrum > 2^23 per component, must NOT wrap ----
        use_fs = 1;
        run_fft(0);
        mism = 0;
        for (i = 0; i < N; i = i + 1)
            if (cap_re[i] !== $signed(fs_mdl_i[i]) || cap_im[i] !== $signed(fs_mdl_q[i])) mism = mism + 1;
        check(cap_count == N && mism == 0, "full-scale input: bit-exact vs model");
        max_err = 0; fs_peak = 0;
        for (i = 0; i < N; i = i + 1) begin
            err_re = cap_re[i] - $signed(fs_np_i[i]); if (err_re < 0) err_re = -err_re;
            err_im = cap_im[i] - $signed(fs_np_q[i]); if (err_im < 0) err_im = -err_im;
            if (err_re > max_err) max_err = err_re;
            if (err_im > max_err) max_err = err_im;
            if (cap_re[i] > fs_peak) fs_peak = cap_re[i];
        end
        $display("full-scale: max_err vs saturated numpy = %0d, peak = %0d", max_err, fs_peak);
        check(max_err <= MAX_ABS, "full-scale input: no internal wrap (equals saturated numpy FFT)");
        check(fs_peak == 32767 && cap_re[5] == 16'sd32767, "full-scale input: peak bin 5 equals the exact value (+32767)");
        use_fs = 0;

        $display("\nResults: %0d/%0d passed", pass_count, pass_count + fail_count);
        $finish;
    end
endmodule
