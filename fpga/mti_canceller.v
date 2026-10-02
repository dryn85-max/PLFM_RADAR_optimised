`timescale 1ns / 1ps

/**
 * mti_canceller.v
 *
 * Moving Target Indication (MTI) — 2-pulse canceller for ground clutter removal.
 *
 * Sits between the range bin decimator and the Doppler processor.  Subtracts
 * the previous chirp's range profile from the current one, H(z) = 1 - z^-1 in
 * slow time: a null at zero Doppler removes stationary clutter.
 *
 * Algorithm, for each range bin r:
 *   out[r] = sat16( cur[r] - prev[r] );   prev[r] <= cur[r]
 * On the first chirp after reset/enable the output is zero (muted) because
 * there is no previous chirp.  When mti_enable = 0 the module passes data
 * through (same 2-clock latency).
 *
 * Implementation (RAM-inferable history):
 *   clock 1: register the input (cur_*), read prev[range_bin_in] synchronously
 *   clock 2: output = cur - prev_rd (saturated); write prev[cur_bin] <= cur
 * The write happens one clock after the read of the same address, so there is
 * never a same-cycle read/write collision on the history RAM.  The arrays are
 * written in their own always block without reset (block RAM / LUT RAM).
 *
 * Resources: 2 x 64 x 16 bit RAM (2048 bits), small subtract/saturate/mux
 * logic, 0 multipliers.
 */

module mti_canceller #(
    parameter NUM_RANGE_BINS = 64,
    parameter DATA_WIDTH     = 16
) (
    input wire clk,
    input wire reset_n,

    input wire signed [DATA_WIDTH-1:0] range_i_in,
    input wire signed [DATA_WIDTH-1:0] range_q_in,
    input wire                         range_valid_in,
    input wire [5:0]                   range_bin_in,

    output reg signed [DATA_WIDTH-1:0] range_i_out,
    output reg signed [DATA_WIDTH-1:0] range_q_out,
    output reg                         range_valid_out,
    output reg [5:0]                   range_bin_out,

    input wire mti_enable,

    output reg mti_first_chirp
);

// ---- Previous-chirp history (RAM) ----
reg signed [DATA_WIDTH-1:0] prev_i [0:NUM_RANGE_BINS-1];
reg signed [DATA_WIDTH-1:0] prev_q [0:NUM_RANGE_BINS-1];

// Stage 1 registers
reg signed [DATA_WIDTH-1:0] cur_i, cur_q;
reg                         cur_valid;
reg [5:0]                   cur_bin;
reg signed [DATA_WIDTH-1:0] prev_i_rd, prev_q_rd;

// Synchronous read (stage 1)
always @(posedge clk) begin
    prev_i_rd <= prev_i[range_bin_in];
    prev_q_rd <= prev_q[range_bin_in];
end

// Write port (stage 2): store the current chirp for the next one
always @(posedge clk) begin
    if (cur_valid) begin
        prev_i[cur_bin] <= cur_i;
        prev_q[cur_bin] <= cur_q;
    end
end

always @(posedge clk or negedge reset_n) begin
    if (!reset_n) begin
        cur_i <= 0; cur_q <= 0; cur_valid <= 1'b0; cur_bin <= 6'd0;
    end else begin
        cur_i     <= range_i_in;
        cur_q     <= range_q_in;
        cur_valid <= range_valid_in;
        cur_bin   <= range_bin_in;
    end
end

// ---- Difference with saturation ----
wire signed [DATA_WIDTH:0] diff_i_full = {cur_i[DATA_WIDTH-1], cur_i} - {prev_i_rd[DATA_WIDTH-1], prev_i_rd};
wire signed [DATA_WIDTH:0] diff_q_full = {cur_q[DATA_WIDTH-1], cur_q} - {prev_q_rd[DATA_WIDTH-1], prev_q_rd};

localparam signed [DATA_WIDTH:0] MAXP =  (1 << (DATA_WIDTH - 1)) - 1;
localparam signed [DATA_WIDTH:0] MINN = -(1 << (DATA_WIDTH - 1));

wire signed [DATA_WIDTH-1:0] diff_i_sat = (diff_i_full > MAXP) ? MAXP[DATA_WIDTH-1:0] :
                                          (diff_i_full < MINN) ? MINN[DATA_WIDTH-1:0] : diff_i_full[DATA_WIDTH-1:0];
wire signed [DATA_WIDTH-1:0] diff_q_sat = (diff_q_full > MAXP) ? MAXP[DATA_WIDTH-1:0] :
                                          (diff_q_full < MINN) ? MINN[DATA_WIDTH-1:0] : diff_q_full[DATA_WIDTH-1:0];

// ---- Stage 2: output ----
reg has_previous;

always @(posedge clk or negedge reset_n) begin
    if (!reset_n) begin
        range_i_out     <= {DATA_WIDTH{1'b0}};
        range_q_out     <= {DATA_WIDTH{1'b0}};
        range_valid_out <= 1'b0;
        range_bin_out   <= 6'd0;
        has_previous    <= 1'b0;
        mti_first_chirp <= 1'b1;
    end else begin
        range_valid_out <= 1'b0;
        if (cur_valid) begin
            range_bin_out   <= cur_bin;
            range_valid_out <= 1'b1;
            if (!mti_enable) begin
                range_i_out     <= cur_i;
                range_q_out     <= cur_q;
                has_previous    <= 1'b0;
                mti_first_chirp <= 1'b1;
            end else if (!has_previous) begin
                range_i_out <= {DATA_WIDTH{1'b0}};
                range_q_out <= {DATA_WIDTH{1'b0}};
                if (cur_bin == NUM_RANGE_BINS - 1) begin
                    has_previous    <= 1'b1;
                    mti_first_chirp <= 1'b0;
                end
            end else begin
                range_i_out <= diff_i_sat;
                range_q_out <= diff_q_sat;
            end
        end
    end
end

`ifdef SIMULATION
integer init_k;
initial begin
    for (init_k = 0; init_k < NUM_RANGE_BINS; init_k = init_k + 1) begin
        prev_i[init_k] = 0;
        prev_q[init_k] = 0;
    end
end
`endif

endmodule
