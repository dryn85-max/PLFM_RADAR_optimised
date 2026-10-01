`timescale 1ns / 1ps

// ============================================================================
// CDC FOR SINGLE BIT SIGNALS
// Plain multi-stage synchronizer with synchronous reset.  Multi-bit data must
// use cdc_handshake below — never a per-bit synchronizer.
// ============================================================================
module cdc_single_bit #(
    parameter STAGES = 3
)(
    input wire src_clk,
    input wire dst_clk,
    input wire reset_n,
    input wire src_signal,
    output wire dst_signal
);

    reg [STAGES-1:0] sync_chain;
    
    always @(posedge dst_clk) begin
        if (!reset_n) begin
            sync_chain <= 0;
        end else begin
            sync_chain <= {sync_chain[STAGES-2:0], src_signal};
        end
    end
    
    assign dst_signal = sync_chain[STAGES-1];
    
endmodule

// ============================================================================
// CDC FOR MULTI-BIT WITH HANDSHAKE
// Uses synchronous reset to avoid metastability on reset deassertion.
// ============================================================================
module cdc_handshake #(
    parameter WIDTH = 32
)(
    input wire src_clk,
    input wire dst_clk,
    input wire reset_n,
    input wire [WIDTH-1:0] src_data,
    input wire src_valid,
    output wire src_ready,
    output wire [WIDTH-1:0] dst_data,
    output wire dst_valid,
    input wire dst_ready
`ifdef FORMAL
    ,output wire              fv_src_busy,
    output wire              fv_dst_ack,
    output wire              fv_dst_req_sync,
    output wire [1:0]        fv_src_ack_sync_chain,
    output wire [1:0]        fv_dst_req_sync_chain,
    output wire [WIDTH-1:0]  fv_src_data_reg_hs
`endif
);

    // Source domain
    reg [WIDTH-1:0] src_data_reg;
    reg src_busy = 0;
    reg src_ack_sync = 0;
    reg [1:0] src_ack_sync_chain = 2'b00;
    
    // Destination domain
    reg [WIDTH-1:0] dst_data_reg;
    reg dst_valid_reg = 0;
    reg dst_req_sync = 0;
    reg [1:0] dst_req_sync_chain = 2'b00;
    reg dst_ack = 0;

`ifdef FORMAL
    assign fv_src_busy           = src_busy;
    assign fv_dst_ack            = dst_ack;
    assign fv_dst_req_sync       = dst_req_sync;
    assign fv_src_ack_sync_chain = src_ack_sync_chain;
    assign fv_dst_req_sync_chain = dst_req_sync_chain;
    assign fv_src_data_reg_hs    = src_data_reg;
`endif
    
    // Source clock domain — synchronous reset
    always @(posedge src_clk) begin
        if (!reset_n) begin
            src_data_reg <= 0;
            src_busy <= 0;
            src_ack_sync <= 0;
            src_ack_sync_chain <= 2'b00;
        end else begin
            // Sync acknowledge from destination
            src_ack_sync_chain <= {src_ack_sync_chain[0], dst_ack};
            src_ack_sync <= src_ack_sync_chain[1];
            
            if (!src_busy && src_valid) begin
                src_data_reg <= src_data;
                src_busy <= 1'b1;
            end else if (src_busy && src_ack_sync) begin
                src_busy <= 1'b0;
            end
        end
    end
    
    // Destination clock domain — synchronous reset
    always @(posedge dst_clk) begin
        if (!reset_n) begin
            dst_data_reg <= 0;
            dst_valid_reg <= 0;
            dst_req_sync <= 0;
            dst_req_sync_chain <= 2'b00;
            dst_ack <= 0;
        end else begin
            // Sync request from source
            dst_req_sync_chain <= {dst_req_sync_chain[0], src_busy};
            dst_req_sync <= dst_req_sync_chain[1];
            
            // Capture data when request arrives
            if (dst_req_sync && !dst_valid_reg) begin
                dst_data_reg <= src_data_reg;
                dst_valid_reg <= 1'b1;
                dst_ack <= 1'b1;
            end else if (dst_valid_reg && dst_ready) begin
                dst_valid_reg <= 1'b0;
            end
            
            // Clear acknowledge after source sees it
            if (dst_ack && !dst_req_sync) begin
                dst_ack <= 1'b0;
            end
        end
    end
    
    assign src_ready = !src_busy;
    assign dst_data = dst_data_reg;
    assign dst_valid = dst_valid_reg;
    
endmodule
