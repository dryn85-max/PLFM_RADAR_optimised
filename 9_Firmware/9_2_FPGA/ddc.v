`timescale 1ns / 1ps
// ============================================================================
// ddc.v — digital down-converter for the 12-bit, 100 MSPS, 20 MHz-IF front end
//
//   adc_data (12-bit offset binary, 100 MSPS)
//     -> 4-register delay (matches NCO latency)        12 bit
//     -> offset-binary to two's complement             12 bit signed
//     -> mixer  I = adc * cos, Q = adc * sin           12 x 16 -> 28 bit
//     -> truncate [27:12]                               16 bit  (|.| <= 2^14)
//     -> CIC N=5, R=4                                   16 bit @ 25 MSPS
//     -> FIR 32 taps (symmetric)                        16 bit @ 25 MSPS
//
// Mixer truncation: adc_signed is an integer in [-2048, 2047]; the NCO is Q15
// (+-32767).  The 28-bit product has 15 fractional bits; keeping bits [27:12]
// drops 12 of them, so full-scale ADC x full-scale NCO = +-16384 (bit 14) and
// one bit of headroom is left for the CIC pass-band ripple.
//
// NCO phase alignment: the NCO advances only on accepted samples and starts
// at phase 0, so sample n is mixed with phase PHASE_INC*n (DITHER_EN = 0).
// The sample is delayed NCO_LAT = 4 registers to meet the NCO's latency
// (see nco.v "Timing contract").
//
// Latency (clk edges, sample presented -> baseband_valid):
//   NCO/delay 4 + mixer 3 + CIC 6 (per decimated output) + FIR + 1.
//
// Resources: 2 (mixer) + 2 x 4 (FIR) = 10 multipliers, 0 RAM.  Sub-blocks:
// nco (0 mult), 2 x cic_decimator_4x_enhanced (0 mult), 2 x fir_lowpass (4 mult).
// ============================================================================
module ddc #(
    parameter ADC_W     = 12,
    parameter NCO_W     = 16,
    parameter OUT_W     = 16,
    parameter PHASE_INC = 32'h3333_3333,
    parameter DITHER_EN = 1
)(
    input  wire                    clk,
    input  wire                    reset_n,
    input  wire                    mixers_enable,
    input  wire [ADC_W-1:0]        adc_data,
    input  wire                    adc_valid,
    output reg  signed [OUT_W-1:0] baseband_i,
    output reg  signed [OUT_W-1:0] baseband_q,
    output reg                     baseband_valid
);

localparam PROD_W  = ADC_W + NCO_W;   // 28
localparam CIC_W   = 16;
localparam NCO_LAT = 4;

// ---- mixers_enable synchronizer (async GPIO from the STM32) ----
reg [1:0] en_sync;
always @(posedge clk or negedge reset_n) begin
    if (!reset_n) en_sync <= 2'b00;
    else          en_sync <= {en_sync[0], mixers_enable};
end
wire sample_en = adc_valid & en_sync[1];

// ---- NCO ----
wire signed [NCO_W-1:0] sin_w, cos_w;
nco #(
    .PHASE_INC(PHASE_INC),
    .DITHER_EN(DITHER_EN)
) nco_inst (
    .clk(clk),
    .reset_n(reset_n),
    .phase_valid(sample_en),
    .phase_offset(16'h0000),
    .sin_out(sin_w),
    .cos_out(cos_w),
    .dds_ready()
);

// ---- ADC delay line matching the NCO latency ----
reg [ADC_W-1:0]   adc_dly [0:NCO_LAT-1];
reg [NCO_LAT-1:0] valid_dly;
integer d;
always @(posedge clk or negedge reset_n) begin
    if (!reset_n) begin
        for (d = 0; d < NCO_LAT; d = d + 1) adc_dly[d] <= {ADC_W{1'b0}};
        valid_dly <= {NCO_LAT{1'b0}};
    end else begin
        adc_dly[0] <= adc_data;
        for (d = 1; d < NCO_LAT; d = d + 1) adc_dly[d] <= adc_dly[d-1];
        valid_dly <= {valid_dly[NCO_LAT-2:0], sample_en};
    end
end

// Offset binary -> two's complement (invert the MSB)
wire signed [ADC_W-1:0] adc_signed_w = {~adc_dly[NCO_LAT-1][ADC_W-1], adc_dly[NCO_LAT-1][ADC_W-2:0]};

// ---- Mixer: operand regs -> product regs -> truncated output regs ----
reg signed [ADC_W-1:0]  adc_r;
reg signed [NCO_W-1:0]  cos_r, sin_r;
reg signed [PROD_W-1:0] prod_i_r, prod_q_r;
reg signed [CIC_W-1:0]  mixed_i, mixed_q;
reg [2:0]               mix_valid;

always @(posedge clk or negedge reset_n) begin
    if (!reset_n) begin
        adc_r     <= {ADC_W{1'b0}};
        cos_r     <= {NCO_W{1'b0}};
        sin_r     <= {NCO_W{1'b0}};
        prod_i_r  <= {PROD_W{1'b0}};
        prod_q_r  <= {PROD_W{1'b0}};
        mixed_i   <= {CIC_W{1'b0}};
        mixed_q   <= {CIC_W{1'b0}};
        mix_valid <= 3'b000;
    end else begin
        adc_r     <= adc_signed_w;
        cos_r     <= cos_w;
        sin_r     <= sin_w;
        prod_i_r  <= adc_r * cos_r;
        prod_q_r  <= adc_r * sin_r;
        mixed_i   <= prod_i_r[PROD_W-1:PROD_W-CIC_W];
        mixed_q   <= prod_q_r[PROD_W-1:PROD_W-CIC_W];
        mix_valid <= {mix_valid[1:0], valid_dly[NCO_LAT-1]};
    end
end

// ---- CIC decimators (100 MSPS -> 25 MSPS) ----
wire signed [CIC_W-1:0] cic_i_out, cic_q_out;
wire cic_valid_i, cic_valid_q;

cic_decimator_4x_enhanced #(.DATA_W(CIC_W)) cic_i_inst (
    .clk(clk), .reset_n(reset_n),
    .data_in(mixed_i), .data_valid(mix_valid[2]),
    .data_out(cic_i_out), .data_out_valid(cic_valid_i));

cic_decimator_4x_enhanced #(.DATA_W(CIC_W)) cic_q_inst (
    .clk(clk), .reset_n(reset_n),
    .data_in(mixed_q), .data_valid(mix_valid[2]),
    .data_out(cic_q_out), .data_out_valid(cic_valid_q));

// ---- FIR low-pass (one valid input every 4 clocks) ----
wire signed [OUT_W-1:0] fir_i_out, fir_q_out;
wire fir_valid_i, fir_valid_q;

fir_lowpass #(.DATA_W(OUT_W)) fir_i_inst (
    .clk(clk), .reset_n(reset_n),
    .data_in(cic_i_out), .data_valid(cic_valid_i),
    .data_out(fir_i_out), .data_out_valid(fir_valid_i),
    .fir_ready(), .filter_overflow());

fir_lowpass #(.DATA_W(OUT_W)) fir_q_inst (
    .clk(clk), .reset_n(reset_n),
    .data_in(cic_q_out), .data_valid(cic_valid_q),
    .data_out(fir_q_out), .data_out_valid(fir_valid_q),
    .fir_ready(), .filter_overflow());

// ---- Output register ----
always @(posedge clk or negedge reset_n) begin
    if (!reset_n) begin
        baseband_i     <= {OUT_W{1'b0}};
        baseband_q     <= {OUT_W{1'b0}};
        baseband_valid <= 1'b0;
    end else begin
        baseband_valid <= fir_valid_i & fir_valid_q;
        if (fir_valid_i & fir_valid_q) begin
            baseband_i <= fir_i_out;
            baseband_q <= fir_q_out;
        end
    end
end

endmodule
