`timescale 1ns / 1ps
// ============================================================================
// cic_decimator_4x_enhanced.v — 5-stage CIC decimator, R = 4, M = 1
//
// Register width from Hogenauer:  B_max = N * log2(R*M) + B_in
//                                      = STAGES * LOG2_R + DATA_W = 26 bits
// All integrators and combs are ACC_W wide and wrap (modular arithmetic);
// the result is exact.  Gain = (R*M)^N = 2^(STAGES*LOG2_R) = 1024, removed
// by taking the top DATA_W bits of the last comb (an exact >>> 10), so DC
// gain is exactly 1 and no saturation logic is needed.
//
// Structure (one clock per stage, all enabled by valid):
//   integ[0] <= integ[0] + x ; integ[k] <= integ[k] + integ[k-1]      (N stages)
//   every R-th valid input:  sampled <= integ[N-1]                     (decimate)
//   comb[0] <= sampled - comb_d[0]; comb[k] <= comb[k-1] - comb_d[k]   (N stages)
//   data_out <= comb[N-1][ACC_W-1 : ACC_W-DATA_W]
// With x[n] the n-th valid input and S_N its N-fold cumulative sum, the j-th
// output is  Delta^N S_N[4j-2] >> 10  (the k-th integrator lags k samples and
// the decimator captures the integrator value BEFORE the 4j+3-rd update).
// Latency: output j is registered 6 edges after input 4j+3.
//
// Resources: 2*N*ACC_W + 2*DATA_W + ~10 flip-flops, 0 multipliers, 0 RAM.
// ============================================================================
module cic_decimator_4x_enhanced #(
    parameter DATA_W = 16,
    parameter STAGES = 5,
    parameter LOG2_R = 2
)(
    input  wire                     clk,
    input  wire                     reset_n,
    input  wire signed [DATA_W-1:0] data_in,
    input  wire                     data_valid,
    output reg  signed [DATA_W-1:0] data_out,
    output reg                      data_out_valid
);

localparam R     = (1 << LOG2_R);
localparam ACC_W = DATA_W + STAGES * LOG2_R;

reg signed [ACC_W-1:0] integ  [0:STAGES-1];
reg signed [ACC_W-1:0] comb   [0:STAGES-1];
reg signed [ACC_W-1:0] comb_d [0:STAGES-1];
reg        [STAGES-1:0] comb_valid;
reg        [LOG2_R-1:0] decim_cnt;
reg signed [ACC_W-1:0] sampled;
reg                    sampled_valid;

integer k;

// ---- Integrator cascade ----
always @(posedge clk) begin
    if (!reset_n) begin
        for (k = 0; k < STAGES; k = k + 1) integ[k] <= {ACC_W{1'b0}};
    end else if (data_valid) begin
        integ[0] <= integ[0] + {{(ACC_W-DATA_W){data_in[DATA_W-1]}}, data_in};
        for (k = 1; k < STAGES; k = k + 1) integ[k] <= integ[k] + integ[k-1];
    end
end

// ---- Decimation ----
always @(posedge clk) begin
    if (!reset_n) begin
        decim_cnt     <= {LOG2_R{1'b0}};
        sampled       <= {ACC_W{1'b0}};
        sampled_valid <= 1'b0;
    end else begin
        sampled_valid <= 1'b0;
        if (data_valid) begin
            if (decim_cnt == R - 1) begin
                decim_cnt     <= {LOG2_R{1'b0}};
                sampled       <= integ[STAGES-1];
                sampled_valid <= 1'b1;
            end else begin
                decim_cnt <= decim_cnt + 1'b1;
            end
        end
    end
end

// ---- Comb cascade + output ----
always @(posedge clk) begin
    if (!reset_n) begin
        for (k = 0; k < STAGES; k = k + 1) begin
            comb[k]   <= {ACC_W{1'b0}};
            comb_d[k] <= {ACC_W{1'b0}};
        end
        comb_valid     <= {STAGES{1'b0}};
        data_out       <= {DATA_W{1'b0}};
        data_out_valid <= 1'b0;
    end else begin
        comb_valid <= {comb_valid[STAGES-2:0], sampled_valid};
        if (sampled_valid) begin
            comb[0]   <= sampled - comb_d[0];
            comb_d[0] <= sampled;
        end
        for (k = 1; k < STAGES; k = k + 1) begin
            if (comb_valid[k-1]) begin
                comb[k]   <= comb[k-1] - comb_d[k];
                comb_d[k] <= comb[k-1];
            end
        end
        data_out_valid <= comb_valid[STAGES-1];
        if (comb_valid[STAGES-1])
            data_out <= comb[STAGES-1][ACC_W-1:ACC_W-DATA_W];
    end
end

endmodule
