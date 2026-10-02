`timescale 1ns / 1ps
// ============================================================================
// adc_cmos_interface.v — single-edge CMOS parallel ADC capture
//
// Replaces ad9484_interface_400m.v (LVDS, DDR, 400 MHz) for the 12-bit
// 100 MSPS CMOS ADC.  adc_clk is the ADC data-clock output and is ALSO the
// processing clock of the whole receiver (clk_proc = adc_clk), so no clock
// domain crossing exists between the ADC and the DDC.
//
//   adc_data[DATA_W-1:0]  offset-binary samples, change on adc_clk edges
//   adc_ovr               over-range pin (active high)
//   sample / sample_valid registered sample stream, 2-edge latency
//   overrange             sticky flag, cleared by overrange_clear (set wins)
//
// Resources: 2*DATA_W + 4 flip-flops, no multipliers, no RAM.
// Board-specific: input delay constraints for the data pins; any I/O
// register packing is left to the board wrapper/tool.
// ============================================================================
module adc_cmos_interface #(
    parameter DATA_W = 12
)(
    input  wire              adc_clk,          // ADC DCO, <= 125 MHz
    input  wire              reset_n,
    input  wire [DATA_W-1:0] adc_data,
    input  wire              adc_ovr,
    input  wire              overrange_clear,
    output reg  [DATA_W-1:0] sample,
    output reg               sample_valid,
    output reg               overrange
);

// First capture stage: no reset so the tool may place it in the I/O cell.
reg [DATA_W-1:0] adc_data_q1;
reg              adc_ovr_q1;

always @(posedge adc_clk) begin
    adc_data_q1 <= adc_data;
    adc_ovr_q1  <= adc_ovr;
end

always @(posedge adc_clk or negedge reset_n) begin
    if (!reset_n) begin
        sample       <= {DATA_W{1'b0}};
        sample_valid <= 1'b0;
        overrange    <= 1'b0;
    end else begin
        sample       <= adc_data_q1;
        sample_valid <= 1'b1;
        if (adc_ovr_q1)
            overrange <= 1'b1;
        else if (overrange_clear)
            overrange <= 1'b0;
    end
end

endmodule
