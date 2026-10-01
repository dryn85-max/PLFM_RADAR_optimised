`timescale 1ns / 1ps
// ============================================================================
// matched_filter_multi_segment.v — overlap-save segmenter for the pulse
// compression chain (256-point segments, reference spectra in ROM)
//
// Sample rates / sizes (25 MSPS baseband, 1 sample per 4 clk @ 100 MHz):
//   long chirp  30 us  -> LONG_CHIRP_SAMPLES  = 750
//   short chirp 0.5 us -> SHORT_CHIRP_SAMPLES = 13
//   N_FFT = 256, OVERLAP = 32, ADVANCE = 224
//   samples covered after k segments = 256 + 224*(k-1):
//     k = 3 -> 704 < 750, k = 4 -> 928 >= 750   => LONG_SEGMENTS = 4
//
// Receive-window buffering (owner-approved design):
//   One segment (FFT + conj-multiply + IFFT on a single fft_engine) takes
//   ~10k clk, the DDC delivers a sample every 4 clk (224 new samples per
//   896 clk), so samples cannot be consumed segment by segment.  Instead the
//   segmenter writes EVERY incoming sample of the chirp's receive window into
//   one RAM without gaps:
//     long chirp : LONG_WINDOW = ADVANCE*(LONG_SEGMENTS-1) + N_FFT = 928
//                  samples (echo tail of the 750-sample chirp included),
//     short chirp: SHORT_CHIRP_SAMPLES = 13 samples,
//   and only afterwards processes the segments, reading segment s from RAM
//   address [224*s, 224*s + 256).  Reads at or beyond the window length return
//   zero (short chirp: 13 samples then zeros).  Samples that arrive after the
//   window (or while the segments are processed) are ignored.
//   The reference ROM segment s holds the spectrum of chirp samples
//   [224*s, 224*s+256), the same window the buffer feeds, so a target at
//   delay d (<= 178 for a full echo in segment 3) peaks at bin d in each
//   segment.  ref_segment passed to the chain: long = s (0..3), short = 4.
//
// Timing budget @ 100 MHz (measured by tb_mf_segmenter, synthesizable chain):
//   collect 928 samples ................ 928*4  =  3.7k clk (the chirp's receive window)
//   process 4 segments ................. 4*~10.0k = ~40k clk = ~0.40 ms
//   total busy per long chirp .......... 43768 clk = 0.44 ms  < PRI = 1 ms
//
// Overrun: a chirp-start toggle that arrives while the segmenter is busy
// (collecting or processing) is IGNORED (the running chirp completes intact,
// nothing is overwritten) and the sticky flag mf_overrun is set; it is
// cleared only by reset.  A chirp that arrives in IDLE starts normally.
// mf_overrun is not yet routed to a host status register (protocol change,
// owner's decision).
//
// Resources: input buffer 2 x 1024 x 16 bit = 32 kbit inferable RAM (one write
// and one read port, sync read, no reset on the contents); the former
// 2 x 32 x 16 overlap cache is gone.  No multipliers of its own (chain: 4 + 4).
// ============================================================================
module matched_filter_multi_segment #(
    parameter N_FFT               = 256,
    parameter LOG2N               = 8,
    parameter OVERLAP             = 32,
    parameter LONG_CHIRP_SAMPLES  = 750,
    parameter SHORT_CHIRP_SAMPLES = 13,
    parameter LONG_SEGMENTS       = 4
)(
    input wire clk,
    input wire reset_n,

    // Baseband from the DDC / gain control (25 MSPS)
    input wire signed [15:0] ddc_i,
    input wire signed [15:0] ddc_q,
    input wire ddc_valid,

    // Chirp control
    input wire use_long_chirp,
    input wire [5:0] chirp_counter,
    input wire mc_new_chirp,
    input wire mc_new_elevation,
    input wire mc_new_azimuth,

    // Pulse-compressed output
    output wire signed [15:0] pc_i_w,
    output wire signed [15:0] pc_q_w,
    output wire pc_valid_w,

    output reg [3:0] status,

    // Sticky: a chirp arrived while the previous one was still being processed
    output reg mf_overrun
);

localparam ADVANCE     = N_FFT - OVERLAP;
localparam LONG_WINDOW = ADVANCE * (LONG_SEGMENTS - 1) + N_FFT;   // 928
localparam RAM_AW      = 10;                                      // 1024 words >= LONG_WINDOW

// State encoding (2 and 8 of the former per-segment-collect FSM are unused)
localparam [3:0] ST_IDLE         = 4'd0,
                 ST_COLLECT_DATA = 4'd1,   // write the whole receive window into the RAM
                 ST_PRIME        = 4'd3,   // present read address, wait for the RAM
                 ST_PROCESSING   = 4'd4,
                 ST_WAIT_FFT     = 4'd5,
                 ST_OUTPUT       = 4'd6,
                 ST_NEXT_SEGMENT = 4'd7;

reg [3:0] state;

// Receive-window buffer (inferable RAM: sync read, one write port per always block)
reg signed [15:0] input_buffer_i [0:(1<<RAM_AW)-1];
reg signed [15:0] input_buffer_q [0:(1<<RAM_AW)-1];
reg        buf_we;
reg [RAM_AW-1:0] buf_waddr;
reg signed [15:0] buf_wdata_i, buf_wdata_q;
reg [RAM_AW-1:0] buf_raddr;
reg signed [15:0] buf_rdata_i, buf_rdata_q;

always @(posedge clk) begin
    if (buf_we) begin
        input_buffer_i[buf_waddr] <= buf_wdata_i;
        input_buffer_q[buf_waddr] <= buf_wdata_q;
    end
end
always @(posedge clk) begin
    buf_rdata_i <= input_buffer_i[buf_raddr];
    buf_rdata_q <= input_buffer_q[buf_raddr];
end

// Samples presented to the chain (registered copy of buf_rdata, zero past the window)
reg signed [15:0] fft_input_i, fft_input_q;
reg         fft_input_valid;

reg [RAM_AW:0]  buffer_write_ptr;    // samples stored so far
reg [LOG2N:0]   buffer_read_ptr;     // position inside the current segment
reg [RAM_AW:0]  seg_base;            // 224 * current_segment
reg [RAM_AW:0]  window_len;          // samples of the receive window (latched per chirp)
reg [2:0]       current_segment;
reg [2:0]       total_segments;
reg             long_q;              // chirp type latched at chirp start
reg             saw_chain_output;
reg             primed;

// mc_new_chirp is a TOGGLE (radar_mode_controller.v:9), so any edge starts a
// chirp.  (The previous rising-edge-only detection silently dropped every
// second chirp.)
reg mc_new_chirp_prev, mc_new_elevation_prev, mc_new_azimuth_prev;
wire chirp_start_pulse = mc_new_chirp ^ mc_new_chirp_prev;

always @(posedge clk or negedge reset_n) begin
    if (!reset_n) begin
        mc_new_chirp_prev <= 1'b0; mc_new_elevation_prev <= 1'b0; mc_new_azimuth_prev <= 1'b0;
    end else begin
        mc_new_chirp_prev <= mc_new_chirp; mc_new_elevation_prev <= mc_new_elevation; mc_new_azimuth_prev <= mc_new_azimuth;
    end
end

// Chain interface
wire [15:0] fft_pc_i, fft_pc_q;
wire        fft_pc_valid;
wire [3:0]  fft_chain_state;
wire [2:0]  ref_segment = long_q ? current_segment : 3'd4;

always @(posedge clk or negedge reset_n) begin
    if (!reset_n) begin
        state <= ST_IDLE;
        buffer_write_ptr <= 0; buffer_read_ptr <= 0; seg_base <= 0; window_len <= 0;
        current_segment <= 0; total_segments <= 0; long_q <= 1'b0; saw_chain_output <= 0; primed <= 0;
        buf_we <= 0; buf_waddr <= 0; buf_wdata_i <= 0; buf_wdata_q <= 0; buf_raddr <= 0;
        fft_input_i <= 0; fft_input_q <= 0; fft_input_valid <= 0;
        status <= 0; mf_overrun <= 1'b0;
    end else begin
        buf_we <= 1'b0;
        fft_input_valid <= 1'b0;

        // A chirp that arrives while busy is ignored (see header); remember it.
        if (chirp_start_pulse && state != ST_IDLE) mf_overrun <= 1'b1;

        case (state)
        ST_IDLE: begin
            buffer_write_ptr <= 0; buffer_read_ptr <= 0; seg_base <= 0;
            current_segment <= 0; saw_chain_output <= 0;
            if (chirp_start_pulse) begin
                state <= ST_COLLECT_DATA;
                long_q         <= use_long_chirp;
                total_segments <= use_long_chirp ? LONG_SEGMENTS[2:0] : 3'd1;
                window_len     <= use_long_chirp ? LONG_WINDOW[RAM_AW:0] : SHORT_CHIRP_SAMPLES[RAM_AW:0];
            end
        end

        // Every sample of the window goes into the RAM, no gaps; nothing is
        // read while collecting, so the write port is never contended.
        ST_COLLECT_DATA: begin
            if (ddc_valid) begin
                buf_we <= 1'b1;
                buf_waddr <= buffer_write_ptr[RAM_AW-1:0];
                buf_wdata_i <= ddc_i;
                buf_wdata_q <= ddc_q;
                buffer_write_ptr <= buffer_write_ptr + 1'b1;
                if (buffer_write_ptr == window_len - 1'b1) begin
                    state <= ST_PRIME; primed <= 1'b0;
                end
            end
        end

        ST_PRIME: begin
            // RAM read latency is 1: rdata(t+1) = mem[raddr(t)].  PROCESSING cycle p must
            // see buffer[seg_base+p], so raddr = seg_base+p+1 during that cycle.  Cycle A:
            // raddr <= seg_base; cycle B: raddr <= seg_base+1 (rdata <= buffer[seg_base] at
            // the end of B); PROCESSING then advances raddr <= seg_base+p+2.
            buf_raddr <= primed ? (seg_base[RAM_AW-1:0] + 1'b1) : seg_base[RAM_AW-1:0];
            buffer_read_ptr <= 0;
            primed <= 1'b1;
            if (primed) state <= ST_PROCESSING;
        end

        ST_PROCESSING: begin
            if (buffer_read_ptr < N_FFT) begin
                // zero past the end of the receive window (short chirp: after 13 samples)
                if (seg_base + buffer_read_ptr < window_len) begin
                    fft_input_i <= buf_rdata_i;
                    fft_input_q <= buf_rdata_q;
                end else begin
                    fft_input_i <= 16'sd0;
                    fft_input_q <= 16'sd0;
                end
                fft_input_valid <= 1'b1;
                buf_raddr <= seg_base[RAM_AW-1:0] + {{(RAM_AW-LOG2N){1'b0}}, buffer_read_ptr[LOG2N-1:0]} + 2'd2;
                buffer_read_ptr <= buffer_read_ptr + 1'b1;
            end else begin
                saw_chain_output <= 1'b0;
                state <= ST_WAIT_FFT;
            end
        end

        ST_WAIT_FFT: begin
            if (fft_pc_valid) saw_chain_output <= 1'b1;
            if (saw_chain_output && fft_chain_state == 4'd0) begin
                saw_chain_output <= 1'b0;
                state <= ST_OUTPUT;
            end
        end

        ST_OUTPUT: begin
            if (current_segment < total_segments - 1'b1)
                state <= ST_NEXT_SEGMENT;
            else
                state <= ST_IDLE;
        end

        ST_NEXT_SEGMENT: begin
            current_segment <= current_segment + 1'b1;
            seg_base <= seg_base + ADVANCE[RAM_AW:0];
            primed <= 1'b0;
            state <= ST_PRIME;
        end

        default: state <= ST_IDLE;
        endcase

        status <= {state[2:0], long_q};
    end
end

matched_filter_processing_chain #(.N_FFT(N_FFT), .LOG2N(LOG2N)) m_f_p_c (
    .clk(clk), .reset_n(reset_n),
    .adc_data_i(fft_input_i), .adc_data_q(fft_input_q), .adc_valid(fft_input_valid),
    .chirp_counter(chirp_counter), .ref_segment(ref_segment),
    .range_profile_i(fft_pc_i), .range_profile_q(fft_pc_q), .range_profile_valid(fft_pc_valid),
    .chain_state(fft_chain_state));

assign pc_i_w = fft_pc_i;
assign pc_q_w = fft_pc_q;
assign pc_valid_w = fft_pc_valid;

`ifdef SIMULATION
integer init_k;
initial begin
    for (init_k = 0; init_k < (1 << RAM_AW); init_k = init_k + 1) begin input_buffer_i[init_k] = 0; input_buffer_q[init_k] = 0; end
end
`endif

endmodule
