`timescale 1ns / 1ps
// ============================================================================
// fir_lowpass.v — 32-tap symmetric low-pass FIR, folded and time-multiplexed
//
// Input rate: 25 MSPS on a 100 MHz clock = one valid sample every 4 clocks.
// Symmetry (c[k] == c[31-k]) folds the 32 taps into 16 pre-added pairs, and
// the 4 idle clocks between samples let 4 multipliers compute the 16 pairs
// in 4 phases.  Total: 4 multipliers (17 x 18) per channel.
//
//   y[n] = sum_{k=0}^{15} c[k] * (x[n-k] + x[n-31+k])      (exact == direct form)
//   data_out = saturate16( acc >>> 17 )                     (coefficients are Q1.17)
//
// Pipeline for a sample accepted at edge e (data_valid = 1):
//   e      : delay line shift; run <= 1, phase <= 0
//   e+1..4 : stage A  pre-adders for pairs 4p..4p+3 (p = phase), coeff regs
//   e+2..5 : stage B  4 products (registered)
//   e+3..6 : stage C  acc <= (p == 0 ? 0 : acc) + p0 + p1 + p2 + p3
//   e+7    : stage D  data_out <= saturate(acc >>> 17), data_out_valid
// Widths: pre-add 17, product 35, accumulator 35 + log2(32) = 40 bits
// (no overflow possible: |acc| <= 2^16 * sum|c| < 2^36).
//
// Contract: data_valid must not be asserted more than once per 4 clocks
// (fir_ready reports when a new sample may be accepted on the next edge).
// The CIC in ddc.v guarantees this cadence.
//
// Resources: 4 multipliers, ~32*16 + 2*4*17 + 4*35 + 40 + ~40 flip-flops, 0 RAM.
// ============================================================================
module fir_lowpass #(
    parameter DATA_W  = 16,
    parameter COEFF_W = 18,
    parameter TAPS    = 32
)(
    input  wire                     clk,
    input  wire                     reset_n,
    input  wire signed [DATA_W-1:0] data_in,
    input  wire                     data_valid,
    output reg  signed [DATA_W-1:0] data_out,
    output reg                      data_out_valid,
    output wire                     fir_ready,
    output reg                      filter_overflow
);

localparam HALF   = TAPS / 2;            // 16 pairs
localparam SUM_W  = DATA_W + 1;          // 17
localparam PROD_W = SUM_W + COEFF_W;     // 35
localparam ACC_W  = PROD_W + 5;          // 40 (+log2(TAPS) guard bits)
localparam SHIFT  = COEFF_W - 1;         // 17

localparam signed [ACC_W-1:0] OUT_MAX =  (1 << (DATA_W - 1)) - 1;
localparam signed [ACC_W-1:0] OUT_MIN = -(1 << (DATA_W - 1));

// Coefficients, first half (c[k] == c[31-k])
reg signed [COEFF_W-1:0] coeff [0:HALF-1];
initial begin
    coeff[ 0] = 18'sh00AD; coeff[ 1] = 18'sh00CE; coeff[ 2] = 18'sh3FD87; coeff[ 3] = 18'sh02A6;
    coeff[ 4] = 18'sh00E0; coeff[ 5] = 18'sh3F8C0; coeff[ 6] = 18'sh0A45; coeff[ 7] = 18'sh3FD82;
    coeff[ 8] = 18'sh3F0B5; coeff[ 9] = 18'sh1CAD; coeff[10] = 18'sh3EE59; coeff[11] = 18'sh3E821;
    coeff[12] = 18'sh4841; coeff[13] = 18'sh3B340; coeff[14] = 18'sh3E299; coeff[15] = 18'sh1FFFF;
end

// Delay line (dline[0] = newest sample)
reg signed [DATA_W-1:0] dline [0:TAPS-1];
reg       run;
reg [1:0] phase;

// Stage A/B/C registers
reg signed [SUM_W-1:0]   pre0, pre1, pre2, pre3;
reg signed [COEFF_W-1:0] c0, c1, c2, c3;
reg                      vA; reg [1:0] phA;
reg signed [PROD_W-1:0]  p0, p1, p2, p3;
reg                      vB; reg [1:0] phB;
reg signed [ACC_W-1:0]   acc;
reg                      vC;

integer i;

wire [4:0] base = {phase, 2'b00};   // 4 * phase

assign fir_ready = (!run) || (phase == 2'd3);

// ---- Delay line + phase scheduler ----
always @(posedge clk) begin
    if (!reset_n) begin
        for (i = 0; i < TAPS; i = i + 1) dline[i] <= {DATA_W{1'b0}};
        run   <= 1'b0;
        phase <= 2'd0;
    end else begin
        if (data_valid) begin
            for (i = TAPS-1; i > 0; i = i - 1) dline[i] <= dline[i-1];
            dline[0] <= data_in;
            run   <= 1'b1;
            phase <= 2'd0;
        end else if (run) begin
            phase <= phase + 2'd1;
            if (phase == 2'd3) run <= 1'b0;
        end
    end
end

// ---- Stage A: pre-adders (folding) ----
always @(posedge clk) begin
    if (!reset_n) begin
        vA <= 1'b0; phA <= 2'd0;
        pre0 <= 0; pre1 <= 0; pre2 <= 0; pre3 <= 0;
        c0 <= 0; c1 <= 0; c2 <= 0; c3 <= 0;
    end else begin
        vA  <= run;
        phA <= phase;
        pre0 <= dline[base + 0] + dline[TAPS - 1 - base - 0];
        pre1 <= dline[base + 1] + dline[TAPS - 1 - base - 1];
        pre2 <= dline[base + 2] + dline[TAPS - 1 - base - 2];
        pre3 <= dline[base + 3] + dline[TAPS - 1 - base - 3];
        c0 <= coeff[base + 0];
        c1 <= coeff[base + 1];
        c2 <= coeff[base + 2];
        c3 <= coeff[base + 3];
    end
end

// ---- Stage B: 4 multipliers ----
always @(posedge clk) begin
    if (!reset_n) begin
        vB <= 1'b0; phB <= 2'd0;
        p0 <= 0; p1 <= 0; p2 <= 0; p3 <= 0;
    end else begin
        vB  <= vA;
        phB <= phA;
        p0  <= pre0 * c0;
        p1  <= pre1 * c1;
        p2  <= pre2 * c2;
        p3  <= pre3 * c3;
    end
end

// ---- Stage C: accumulate the 4 phases ----
always @(posedge clk) begin
    if (!reset_n) begin
        acc <= {ACC_W{1'b0}};
        vC  <= 1'b0;
    end else begin
        vC <= vB & (phB == 2'd3);
        if (vB) begin
            if (phB == 2'd0)
                acc <= p0 + p1 + p2 + p3;
            else
                acc <= acc + p0 + p1 + p2 + p3;
        end
    end
end

// ---- Stage D: scale + saturate ----
wire signed [ACC_W-1:0] acc_sh = acc >>> SHIFT;

always @(posedge clk) begin
    if (!reset_n) begin
        data_out        <= {DATA_W{1'b0}};
        data_out_valid  <= 1'b0;
        filter_overflow <= 1'b0;
    end else begin
        data_out_valid <= vC;
        if (vC) begin
            if (acc_sh > OUT_MAX) begin
                data_out <= OUT_MAX[DATA_W-1:0];
                filter_overflow <= 1'b1;
            end else if (acc_sh < OUT_MIN) begin
                data_out <= OUT_MIN[DATA_W-1:0];
                filter_overflow <= 1'b1;
            end else begin
                data_out <= acc_sh[DATA_W-1:0];
                filter_overflow <= 1'b0;
            end
        end
    end
end

endmodule
