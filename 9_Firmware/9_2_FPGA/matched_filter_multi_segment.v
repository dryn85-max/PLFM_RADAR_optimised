`timescale 1ns / 1ps
// ============================================================================
// matched_filter_multi_segment.v — overlap-save segmenter for the pulse
// compression chain (256-point segments, reference spectra in ROM)
//
// Sample rates / sizes (25 MSPS baseband):
//   long chirp  30 us  -> LONG_CHIRP_SAMPLES  = 750
//   short chirp 0.5 us -> SHORT_CHIRP_SAMPLES = 13
//   N_FFT = 256, OVERLAP = 32, ADVANCE = 224
//   samples covered after k segments = 256 + 224*(k-1):
//     k = 3 -> 704 < 750, k = 4 -> 928 >= 750   => LONG_SEGMENTS = 4
//   (the 4th segment is zero-padded).  Short chirp: 1 segment, zero-padded.
//
// Per segment: collect N_FFT samples (segment 0 fresh; later segments start
// with the last OVERLAP samples of the previous one), run the chain
// (FFT -> conj-multiply with ref ROM segment -> IFFT), stream N_FFT bins.
// ref_segment passed to the chain: long = current_segment (0..3), short = 4.
// The reference ROM segment s holds the spectrum of chirp samples
// [224*s, 224*s+256), i.e. the same window the signal buffer holds when the
// input is contiguous, so a target at delay d peaks at bin d in each segment.
//
// Resources: input buffer 2 x 256 x 16 bit (RAM), overlap cache 2 x 32 x 16,
// no multipliers of its own (chain: 4 + 4).
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

    output reg [3:0] status
);

localparam ADVANCE = N_FFT - OVERLAP;

localparam [3:0] ST_IDLE         = 4'd0,
                 ST_COLLECT_DATA = 4'd1,
                 ST_ZERO_PAD     = 4'd2,
                 ST_PRIME        = 4'd3,   // present read address 0, wait for the RAM
                 ST_PROCESSING   = 4'd4,
                 ST_WAIT_FFT     = 4'd5,
                 ST_OUTPUT       = 4'd6,
                 ST_NEXT_SEGMENT = 4'd7,
                 ST_OVERLAP_COPY = 4'd8;

reg [3:0] state;

// Input buffer (inferable RAM)
reg signed [15:0] input_buffer_i [0:N_FFT-1];
reg signed [15:0] input_buffer_q [0:N_FFT-1];
reg        buf_we;
reg [LOG2N-1:0] buf_waddr;
reg signed [15:0] buf_wdata_i, buf_wdata_q;
reg [LOG2N-1:0] buf_raddr;
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

// Samples presented to the chain (registered copy of buf_rdata)
reg signed [15:0] fft_input_i, fft_input_q;
reg         fft_input_valid;

// Overlap cache (tail of the previous segment), written in PROCESSING
reg signed [15:0] overlap_cache_i [0:OVERLAP-1];
reg signed [15:0] overlap_cache_q [0:OVERLAP-1];
reg        ov_we;
reg [LOG2N-1:0] ov_waddr;
always @(posedge clk) begin
    if (ov_we) begin
        // fft_input_* is the registered copy of buf_rdata (same cycle as ov_we, which is
        // registered too); buf_rdata itself has already advanced to the next sample.
        overlap_cache_i[ov_waddr] <= fft_input_i;
        overlap_cache_q[ov_waddr] <= fft_input_q;
    end
end

reg [LOG2N:0] buffer_write_ptr;
reg [LOG2N:0] buffer_read_ptr;
reg [15:0]    chirp_samples_collected;
reg [2:0]     current_segment;
reg [2:0]     total_segments;
reg           chirp_complete;
reg           saw_chain_output;
reg           primed;
reg [LOG2N-1:0] overlap_copy_count;

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
wire [2:0]  ref_segment = use_long_chirp ? current_segment : 3'd4;

always @(posedge clk or negedge reset_n) begin
    if (!reset_n) begin
        state <= ST_IDLE;
        buffer_write_ptr <= 0; buffer_read_ptr <= 0; chirp_samples_collected <= 0;
        current_segment <= 0; total_segments <= 0; chirp_complete <= 0; saw_chain_output <= 0; primed <= 0;
        buf_we <= 0; buf_waddr <= 0; buf_wdata_i <= 0; buf_wdata_q <= 0; buf_raddr <= 0;
        ov_we <= 0; ov_waddr <= 0; overlap_copy_count <= 0;
        fft_input_i <= 0; fft_input_q <= 0; fft_input_valid <= 0;
        status <= 0;
    end else begin
        buf_we <= 1'b0;
        ov_we  <= 1'b0;
        fft_input_valid <= 1'b0;

        case (state)
        ST_IDLE: begin
            buffer_write_ptr <= 0; buffer_read_ptr <= 0; chirp_samples_collected <= 0;
            current_segment <= 0; chirp_complete <= 0; saw_chain_output <= 0;
            if (chirp_start_pulse) begin
                state <= ST_COLLECT_DATA;
                total_segments <= use_long_chirp ? LONG_SEGMENTS[2:0] : 3'd1;
            end
        end

        ST_COLLECT_DATA: begin
            if (ddc_valid && buffer_write_ptr < N_FFT) begin
                buf_we <= 1'b1;
                buf_waddr <= buffer_write_ptr[LOG2N-1:0];
                buf_wdata_i <= ddc_i;
                buf_wdata_q <= ddc_q;
                buffer_write_ptr <= buffer_write_ptr + 1;
                chirp_samples_collected <= chirp_samples_collected + 1;
                if (!use_long_chirp && chirp_samples_collected >= SHORT_CHIRP_SAMPLES - 1) begin
                    chirp_complete <= 1'b1;      // short chirp = one segment, then IDLE
                    state <= ST_ZERO_PAD;
                end
            end
            if (use_long_chirp) begin
                if (buffer_write_ptr >= N_FFT) begin
                    state <= ST_PRIME; primed <= 1'b0;
                end
                if (chirp_samples_collected >= LONG_CHIRP_SAMPLES && !chirp_complete) begin
                    chirp_complete <= 1'b1;
                    if (buffer_write_ptr < N_FFT) state <= ST_ZERO_PAD;
                end
            end
        end

        ST_ZERO_PAD: begin
            buf_we <= 1'b1;
            buf_waddr <= buffer_write_ptr[LOG2N-1:0];
            buf_wdata_i <= 16'sd0;
            buf_wdata_q <= 16'sd0;
            buffer_write_ptr <= buffer_write_ptr + 1;
            if (buffer_write_ptr >= N_FFT - 1) begin
                buffer_write_ptr <= 0;
                state <= ST_PRIME; primed <= 1'b0;
            end
        end

        ST_PRIME: begin
            // RAM read latency is 1: rdata(t+1) = mem[raddr(t)].  PROCESSING cycle p must
            // see buffer[p], so raddr = p + 1 during that cycle.  Cycle A: raddr <= 0;
            // cycle B: raddr <= 1 (rdata <= buffer[0] at the end of B); PROCESSING then
            // advances raddr <= p + 2.
            buf_raddr <= primed ? {{(LOG2N-1){1'b0}}, 1'b1} : {LOG2N{1'b0}};
            buffer_read_ptr <= 0;
            primed <= 1'b1;
            if (primed) state <= ST_PROCESSING;
        end

        ST_PROCESSING: begin
            if (buffer_read_ptr < N_FFT) begin
                fft_input_i <= buf_rdata_i;
                fft_input_q <= buf_rdata_q;
                fft_input_valid <= 1'b1;
                if (buffer_read_ptr >= ADVANCE) begin
                    ov_we <= 1'b1;
                    ov_waddr <= buffer_read_ptr[LOG2N-1:0] - ADVANCE[LOG2N-1:0];
                end
                buf_raddr <= buffer_read_ptr[LOG2N-1:0] + 2'd2;
                buffer_read_ptr <= buffer_read_ptr + 1;
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
            if (current_segment < total_segments - 1 || !chirp_complete)
                state <= ST_NEXT_SEGMENT;
            else
                state <= ST_IDLE;
        end

        ST_NEXT_SEGMENT: begin
            current_segment <= current_segment + 1;
            if (use_long_chirp) begin
                overlap_copy_count <= 0;
                state <= ST_OVERLAP_COPY;
            end else begin
                buffer_write_ptr <= 0;
                state <= chirp_complete ? ST_IDLE : ST_COLLECT_DATA;
            end
        end

        ST_OVERLAP_COPY: begin
            buf_we <= 1'b1;
            buf_waddr <= overlap_copy_count;
            buf_wdata_i <= overlap_cache_i[overlap_copy_count[4:0]];
            buf_wdata_q <= overlap_cache_q[overlap_copy_count[4:0]];
            if (overlap_copy_count < OVERLAP - 1) begin
                overlap_copy_count <= overlap_copy_count + 1;
            end else begin
                buffer_write_ptr <= OVERLAP;
                state <= chirp_complete ? ST_IDLE : ST_COLLECT_DATA;
            end
        end

        default: state <= ST_IDLE;
        endcase

        status <= {state[2:0], use_long_chirp};
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
    for (init_k = 0; init_k < N_FFT; init_k = init_k + 1) begin input_buffer_i[init_k] = 0; input_buffer_q[init_k] = 0; end
    for (init_k = 0; init_k < OVERLAP; init_k = init_k + 1) begin overlap_cache_i[init_k] = 0; overlap_cache_q[init_k] = 0; end
end
`endif

endmodule
