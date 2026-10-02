module dac_interface_enhanced (
    input wire clk_120m,
    input wire reset_n,
    input wire [7:0] chirp_data,
    input wire chirp_valid,
    output wire [7:0] dac_data,
    output wire dac_clk,
    output wire dac_sleep
);

// ============================================================================
// DAC data register
// ============================================================================
reg [7:0] dac_data_reg;

always @(posedge clk_120m or negedge reset_n) begin
    if (!reset_n) begin
        dac_data_reg <= 8'd128;  // Center value
    end else if (chirp_valid) begin
        dac_data_reg <= chirp_data;
    end else begin
        dac_data_reg <= 8'd128;  // Default to center when no chirp
    end
end

// Clock forwarding to the DAC pin is board-specific (PLL output pin on Intel,
// clock-output primitive on Xilinx).  The core exports the clock as a plain
// signal; the board wrapper owns the output primitive.
assign dac_clk  = clk_120m;
assign dac_data = dac_data_reg;

assign dac_sleep = 1'b0;

endmodule