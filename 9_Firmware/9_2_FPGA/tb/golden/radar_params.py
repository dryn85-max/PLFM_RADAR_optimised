#!/usr/bin/env python3
"""Shared radar/DDC parameters and helpers for the golden generators.

Every constant here mirrors a parameter in the RTL; the RTL header that owns
it is named in the comment.
"""
import math
import numpy as np

FS_ADC   = 100e6          # adc_cmos_interface: ADC DCO = clk_proc
FS_BB    = 25e6           # cic_decimator_4x_enhanced: R = 4
F_IF     = 20e6           # nco.v PHASE_INC default
CHIRP_BW = 20e6           # 10..30 MHz IF chirp
T_LONG   = 30e-6          # radar_mode_controller LONG_CHIRP_CYCLES = 3000 @ 100 MHz
T_SHORT  = 0.5e-6         # SHORT_CHIRP_CYCLES = 50
N_FFT    = 256            # matched_filter_processing_chain N_FFT
LOG2N    = 8
OVERLAP  = 32             # matched_filter_multi_segment OVERLAP
ADVANCE  = N_FFT - OVERLAP
LONG_CHIRP_SAMPLES  = int(round(T_LONG * FS_BB))        # 750
SHORT_CHIRP_SAMPLES = int(math.ceil(T_SHORT * FS_BB))   # 13
LONG_SEGMENTS = 4           # 256 + 224*3 = 928 >= 750 (3 segments cover only 704)
PHASE_INC = 0x33333333      # nco.v: round(0.2 * 2^32) truncated to 0x33333333
DDC_OUT_W = 16              # ddc.v OUT_W (16-bit folded FIR output)

# nco.v quarter-wave LUT: round(32767*sin(pi/2*k/64))
NCO_SINE_LUT = [
    0x0000, 0x0324, 0x0648, 0x096A, 0x0C8C, 0x0FAB, 0x12C8, 0x15E2,
    0x18F9, 0x1C0B, 0x1F1A, 0x2223, 0x2528, 0x2826, 0x2B1F, 0x2E11,
    0x30FB, 0x33DF, 0x36BA, 0x398C, 0x3C56, 0x3F17, 0x41CE, 0x447A,
    0x471C, 0x49B4, 0x4C3F, 0x4EBF, 0x5133, 0x539B, 0x55F5, 0x5842,
    0x5A82, 0x5CB3, 0x5ED7, 0x60EB, 0x62F1, 0x64E8, 0x66CF, 0x68A6,
    0x6A6D, 0x6C23, 0x6DC9, 0x6F5E, 0x70E2, 0x7254, 0x73B5, 0x7504,
    0x7641, 0x776B, 0x7884, 0x7989, 0x7A7C, 0x7B5C, 0x7C29, 0x7CE3,
    0x7D89, 0x7E1D, 0x7E9C, 0x7F09, 0x7F61, 0x7FA6, 0x7FD8, 0x7FF5,
]

# fir_lowpass.v coefficients (18-bit two's complement, symmetric, Q1.17)
_FIR_HEX = [
    0x000AD, 0x000CE, 0x3FD87, 0x002A6, 0x000E0, 0x3F8C0, 0x00A45, 0x3FD82,
    0x3F0B5, 0x01CAD, 0x3EE59, 0x3E821, 0x04841, 0x3B340, 0x3E299, 0x1FFFF,
    0x1FFFF, 0x3E299, 0x3B340, 0x04841, 0x3E821, 0x3EE59, 0x01CAD, 0x3F0B5,
    0x3FD82, 0x00A45, 0x3F8C0, 0x000E0, 0x002A6, 0x3FD87, 0x000CE, 0x000AD,
]


def sext(value, bits):
    value &= (1 << bits) - 1
    return value - (1 << bits) if value & (1 << (bits - 1)) else value


FIR_COEFFS = [sext(c, 18) for c in _FIR_HEX]
assert FIR_COEFFS == FIR_COEFFS[::-1], "FIR coefficients must be symmetric"


def write_hex(path, values, width):
    """One value per line, two's complement, width bits, zero-padded hex."""
    digits = (width + 3) // 4
    mask = (1 << width) - 1
    with open(path, "w") as f:
        for v in values:
            f.write(f"{int(v) & mask:0{digits}X}\n")


def read_hex(path, width):
    out = []
    with open(path) as f:
        for line in f:
            line = line.strip()
            if not line or line.startswith("//"):
                continue
            out.append(sext(int(line, 16), width))
    return out


def if_chirp_adc(n_samples, amp, delay, seed=1):
    """12-bit offset-binary ADC samples: 10->30 MHz IF chirp (T_LONG) starting
    at sample `delay`, mid-scale elsewhere, plus +-2 LSB noise."""
    rng = np.random.default_rng(seed)
    n = np.arange(n_samples)
    t = (n - delay) / FS_ADC
    f0 = F_IF - CHIRP_BW / 2
    k = CHIRP_BW / T_LONG
    phase = 2 * np.pi * (f0 * t + 0.5 * k * t * t)
    active = (n >= delay) & (n < delay + int(T_LONG * FS_ADC))
    sig = np.where(active, amp * np.cos(phase), 0.0)
    noise = rng.normal(0.0, 1.0, n_samples)
    adc = np.rint(sig + noise).astype(np.int64) + 2048
    return np.clip(adc, 0, 4095)


def baseband_chirp(n_samples, t_chirp):
    """Complex baseband chirp at FS_BB as float arrays (i, q), unit amplitude,
    as produced by ddc.v for the 10->30 MHz IF chirp of if_chirp_adc().

    ddc.v mixes with I = x*cos, Q = x*sin, i.e. I + jQ = x*exp(+j*w0*t); the
    low-pass keeps the exp(-j*phi_IF) half of the real chirp, so the baseband
    phase is  -(pi*k*t^2 - pi*BW*t)  with k = BW/T (frequency +BW/2 -> -BW/2).
    """
    n = np.arange(n_samples)
    t = n / FS_BB
    rate = CHIRP_BW / t_chirp
    phase = np.pi * CHIRP_BW * t - np.pi * rate * t * t
    return np.cos(phase), np.sin(phase)
