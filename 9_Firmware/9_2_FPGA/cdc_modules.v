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
    input wire src_reset_n,   // synchronous reset, released in the src_clk domain
    input wire dst_reset_n,   // synchronous reset, released in the dst_clk domain
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
    output wire              fv_src_ack_sync,
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
    reg dst_req_prev = 0;
    reg dst_pending = 0;
    wire dst_req_rise = dst_req_sync && !dst_req_prev;

`ifdef FORMAL
    assign fv_src_busy           = src_busy;
    assign fv_dst_ack            = dst_ack;
    assign fv_dst_req_sync       = dst_req_sync;
    assign fv_src_ack_sync       = src_ack_sync;
    assign fv_src_ack_sync_chain = src_ack_sync_chain;
    assign fv_dst_req_sync_chain = dst_req_sync_chain;
    assign fv_src_data_reg_hs    = src_data_reg;
`endif
    
    // Source clock domain — synchronous reset
    always @(posedge src_clk) begin
        if (!src_reset_n) begin
            src_data_reg <= 0;
            src_busy <= 0;
            src_ack_sync <= 0;
            src_ack_sync_chain <= 2'b00;
        end else begin
            // Sync acknowledge from destination
            src_ack_sync_chain <= {src_ack_sync_chain[0], dst_ack};
            src_ack_sync <= src_ack_sync_chain[1];
            
            // Four-phase return-to-zero: a new request may start only when the
            // request line is low AND the (synchronised) acknowledge has also
            // returned low, i.e. the previous handshake has fully completed.
            if (!src_busy && !src_ack_sync && src_valid) begin
                src_data_reg <= src_data;
                src_busy <= 1'b1;
            end else if (src_busy && src_ack_sync) begin
                src_busy <= 1'b0;
            end
        end
    end
    
    // Destination clock domain — synchronous reset
    always @(posedge dst_clk) begin
        if (!dst_reset_n) begin
            dst_data_reg <= 0;
            dst_valid_reg <= 0;
            dst_req_sync <= 0;
            dst_req_sync_chain <= 2'b00;
            dst_req_prev <= 0;
            dst_pending <= 0;
            dst_ack <= 0;
        end else begin
            // Sync request from source
            dst_req_sync_chain <= {dst_req_sync_chain[0], src_busy};
            dst_req_sync <= dst_req_sync_chain[1];
            dst_req_prev <= dst_req_sync;

            // Capture exactly once per request, on the RISING edge of the
            // synchronised request.  If the previous word has not been taken
            // yet (dst_valid_reg still high) the capture is deferred
            // (dst_pending) and the acknowledge is withheld, so the source
            // keeps src_data_reg stable and is back-pressured.
            if ((dst_req_rise || dst_pending) && !dst_valid_reg) begin
                dst_data_reg <= src_data_reg;
                dst_valid_reg <= 1'b1;
                dst_pending <= 1'b0;
                dst_ack <= 1'b1;
            end else begin
                if (dst_req_rise)
                    dst_pending <= 1'b1;
                if (dst_valid_reg && dst_ready)
                    dst_valid_reg <= 1'b0;
            end

            // Return-to-zero: drop the acknowledge once the request is low
            if (dst_ack && !dst_req_sync) begin
                dst_ack <= 1'b0;
            end
        end
    end
    
    assign src_ready = !src_busy && !src_ack_sync;
    assign dst_data = dst_data_reg;
    assign dst_valid = dst_valid_reg;
    
endmodule
