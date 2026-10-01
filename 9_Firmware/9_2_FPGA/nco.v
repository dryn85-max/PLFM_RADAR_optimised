`timescale 1ns / 1ps
// ============================================================================
// nco.v — numerically controlled oscillator for the DDC (vendor-neutral)
//
// f_out = PHASE_INC * f_clk / 2^32.  Default PHASE_INC = 0x3333_3333 gives
// 20 MHz at f_clk = 100 MHz (0x3333_3333 / 2^32 = 0.19999999995).
//
// Pipeline (every stage advances only while phase_valid = 1):
//   S1  phase_acc <= phase_acc + (PHASE_INC + dither)          32-bit '+'
//   S2  phase_off <= phase_acc(old) + {phase_offset, 16'b0}
//   S3  lut_idx / quadrant registered from phase_off[31:24]
//   S4  sin_abs / cos_abs <= quarter-wave LUT (64 x 16)
//   S5  sin_out / cos_out <= quadrant sign mux
// Timing contract: the accumulator is 0 at the first valid edge e0, so the
// n-th valid edge uses phase PHASE_INC*n (DITHER_EN = 0) and the matching
// sin/cos pair is readable at edge e0+n+4.  ddc.v delays the ADC sample by
// NCO_LAT = 4 registers to meet it.
//
// LUT[k] = round(32767 * sin(pi/2 * k / 64)), cos uses LUT[63-k].  Phase is
// truncated to 8 bits (2 quadrant + 6 index) before the LUT.
//
// Dither: an 8-bit LFSR (x^8+x^6+x^5+x^4+1) adds 0..255 to the tuning word
// each valid cycle when DITHER_EN = 1 to spread phase-truncation spurs.
// Golden tests instantiate DITHER_EN = 0 for bit-exact comparison.
//
// Resources: ~110 flip-flops, one 32-bit adder, 1 kbit LUT ROM, 0 multipliers.
// ============================================================================
module nco #(
    parameter PHASE_INC = 32'h3333_3333,
    parameter DITHER_EN = 1
)(
    input  wire               clk,
    input  wire               reset_n,
    input  wire               phase_valid,
    input  wire [15:0]        phase_offset,
    output reg  signed [15:0] sin_out,
    output reg  signed [15:0] cos_out,
    output reg                dds_ready
);

// Quarter-wave sine LUT (0..90 degrees)
reg [15:0] sin_lut [0:63];
initial begin
    sin_lut[ 0] = 16'h0000; sin_lut[ 1] = 16'h0324; sin_lut[ 2] = 16'h0648; sin_lut[ 3] = 16'h096A;
    sin_lut[ 4] = 16'h0C8C; sin_lut[ 5] = 16'h0FAB; sin_lut[ 6] = 16'h12C8; sin_lut[ 7] = 16'h15E2;
    sin_lut[ 8] = 16'h18F9; sin_lut[ 9] = 16'h1C0B; sin_lut[10] = 16'h1F1A; sin_lut[11] = 16'h2223;
    sin_lut[12] = 16'h2528; sin_lut[13] = 16'h2826; sin_lut[14] = 16'h2B1F; sin_lut[15] = 16'h2E11;
    sin_lut[16] = 16'h30FB; sin_lut[17] = 16'h33DF; sin_lut[18] = 16'h36BA; sin_lut[19] = 16'h398C;
    sin_lut[20] = 16'h3C56; sin_lut[21] = 16'h3F17; sin_lut[22] = 16'h41CE; sin_lut[23] = 16'h447A;
    sin_lut[24] = 16'h471C; sin_lut[25] = 16'h49B4; sin_lut[26] = 16'h4C3F; sin_lut[27] = 16'h4EBF;
    sin_lut[28] = 16'h5133; sin_lut[29] = 16'h539B; sin_lut[30] = 16'h55F5; sin_lut[31] = 16'h5842;
    sin_lut[32] = 16'h5A82; sin_lut[33] = 16'h5CB3; sin_lut[34] = 16'h5ED7; sin_lut[35] = 16'h60EB;
    sin_lut[36] = 16'h62F1; sin_lut[37] = 16'h64E8; sin_lut[38] = 16'h66CF; sin_lut[39] = 16'h68A6;
    sin_lut[40] = 16'h6A6D; sin_lut[41] = 16'h6C23; sin_lut[42] = 16'h6DC9; sin_lut[43] = 16'h6F5E;
    sin_lut[44] = 16'h70E2; sin_lut[45] = 16'h7254; sin_lut[46] = 16'h73B5; sin_lut[47] = 16'h7504;
    sin_lut[48] = 16'h7641; sin_lut[49] = 16'h776B; sin_lut[50] = 16'h7884; sin_lut[51] = 16'h7989;
    sin_lut[52] = 16'h7A7C; sin_lut[53] = 16'h7B5C; sin_lut[54] = 16'h7C29; sin_lut[55] = 16'h7CE3;
    sin_lut[56] = 16'h7D89; sin_lut[57] = 16'h7E1D; sin_lut[58] = 16'h7E9C; sin_lut[59] = 16'h7F09;
    sin_lut[60] = 16'h7F61; sin_lut[61] = 16'h7FA6; sin_lut[62] = 16'h7FD8; sin_lut[63] = 16'h7FF5;
end

// ---- Dither ----
wire [7:0]  dither_bits;
wire        dither_on = (DITHER_EN != 0);

lfsr_dither #(.WIDTH(8)) u_dither (
    .clk(clk),
    .reset_n(reset_n),
    .enable(phase_valid & dither_on),
    .dither_out(dither_bits)
);

wire [31:0] ftw = PHASE_INC + (dither_on ? {24'b0, dither_bits} : 32'b0);

// ---- S1/S2: accumulator and offset ----
reg [31:0] phase_acc;
reg [31:0] phase_off;

always @(posedge clk or negedge reset_n) begin
    if (!reset_n) begin
        phase_acc <= 32'h0000_0000;
        phase_off <= 32'h0000_0000;
    end else if (phase_valid) begin
        phase_acc <= phase_acc + ftw;
        phase_off <= phase_acc + {phase_offset, 16'b0};
    end
end

// ---- S3: quadrant / index decode ----
wire [7:0] lut_address = phase_off[31:24];
wire [1:0] quadrant_w  = lut_address[7:6];
wire [5:0] lut_idx_w   = quadrant_w[0] ? ~lut_address[5:0] : lut_address[5:0];

reg [5:0]  lut_idx_r;
reg [1:0]  quadrant_r;
reg [15:0] sin_abs_r, cos_abs_r;
reg [1:0]  quadrant_r2;
reg [4:0]  valid_pipe;

always @(posedge clk or negedge reset_n) begin
    if (!reset_n) begin
        lut_idx_r   <= 6'd0;
        quadrant_r  <= 2'b00;
        sin_abs_r   <= 16'h0000;
        cos_abs_r   <= 16'h7FFF;
        quadrant_r2 <= 2'b00;
        sin_out     <= 16'sh0000;
        cos_out     <= 16'sh7FFF;
    end else begin
        if (valid_pipe[0]) begin                      // S3
            lut_idx_r  <= lut_idx_w;
            quadrant_r <= quadrant_w;
        end
        if (valid_pipe[1]) begin                      // S4: LUT read
            sin_abs_r   <= sin_lut[lut_idx_r];
            cos_abs_r   <= sin_lut[6'd63 - lut_idx_r];
            quadrant_r2 <= quadrant_r;
        end
        if (valid_pipe[2]) begin                      // S5: sign mux
            case (quadrant_r2)
                2'b00: begin sin_out <=  $signed(sin_abs_r); cos_out <=  $signed(cos_abs_r); end
                2'b01: begin sin_out <=  $signed(sin_abs_r); cos_out <= -$signed(cos_abs_r); end
                2'b10: begin sin_out <= -$signed(sin_abs_r); cos_out <= -$signed(cos_abs_r); end
                default: begin sin_out <= -$signed(sin_abs_r); cos_out <=  $signed(cos_abs_r); end
            endcase
        end
    end
end

always @(posedge clk or negedge reset_n) begin
    if (!reset_n) begin
        valid_pipe <= 5'b00000;
        dds_ready  <= 1'b0;
    end else begin
        valid_pipe <= {valid_pipe[3:0], phase_valid};
        dds_ready  <= valid_pipe[3];
    end
end

endmodule

// ============================================================================
// lfsr_dither — Fibonacci LFSR, polynomial x^8+x^6+x^5+x^4+1 for WIDTH = 8
// ============================================================================
`timescale 1ns / 1ps
module lfsr_dither #(
    parameter WIDTH = 8
)(
    input  wire             clk,
    input  wire             reset_n,
    input  wire             enable,
    output wire [WIDTH-1:0] dither_out
);

reg  [WIDTH-1:0] lfsr_reg;
wire             feedback;

generate
    if (WIDTH == 8) begin : g_poly8
        assign feedback = lfsr_reg[7] ^ lfsr_reg[5] ^ lfsr_reg[4] ^ lfsr_reg[3];
    end else begin : g_poly_generic
        assign feedback = lfsr_reg[WIDTH-1] ^ lfsr_reg[WIDTH-2];
    end
endgenerate

always @(posedge clk or negedge reset_n) begin
    if (!reset_n)
        lfsr_reg <= {WIDTH{1'b1}};
    else if (enable)
        lfsr_reg <= {lfsr_reg[WIDTH-2:0], feedback};
end

assign dither_out = lfsr_reg;

endmodule
