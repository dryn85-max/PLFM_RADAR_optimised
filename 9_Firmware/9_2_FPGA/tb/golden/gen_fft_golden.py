#!/usr/bin/env python3
"""gen_fft_golden.py — golden test (c): 256-point fft_engine vs numpy.

Forward: input = two tones + noise, amplitude chosen so |X| < 2^15 (the engine
saturates its 16-bit output without scaling).  Expected = numpy FFT rounded.
Tolerance (documented in tb_fft256_golden.v): the engine truncates (>>> 15)
after every butterfly.  Each floor adds an error of <= 1 LSB per component
(<= sqrt(2) as a complex vector), |twiddle| <= 1, and a stage-s error is
carried by 2^(7-s) outputs, so the deterministic worst case on the complex
error is sqrt(2) * (2^8 - 1) = 360 (+ ~1 for the 2^-15 twiddle quantisation).
The statistical expectation is much lower: floor noise is var 1/12 per
component per butterfly, 2^(7-s) paths per stage, so RMS ~ sqrt(255/12 * 2)
= 6.5 plus a truncation bias (-0.5 per butterfly, adds coherently in the
low bins) that brings the measured value to ~21.  The test limits (max 256
per component, RMS 32) are the measured values for this fixed-seed vector
(max 187, RMS 21) plus ~35 % margin; the script asserts that the bit-accurate
model (fpga_model.FFTEngine, internal_w=24) satisfies them, so the RTL
(bit-identical to the model) does too.
Inverse: input = the rounded numpy spectrum, expected = original samples,
tolerance +-4 per component (1/N scaling truncation).
Writes fft256_in_i/q.hex, fft256_fwd_i/q.hex (numpy), fft256_mdl_i/q.hex (bit-exact
model output) and the full-scale fft256_fs_* set (16-bit).
"""
import os
import sys

import numpy as np

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
sys.path.insert(0, os.path.join(HERE, "..", "cosim"))
from fpga_model import FFTEngine  # noqa: E402
from radar_params import write_hex  # noqa: E402

N = 256
MAX_ABS_ERR = 256
MAX_RMS_ERR = 32


def main():
    rng = np.random.default_rng(3)
    n = np.arange(N)
    tones = 100 * np.cos(2 * np.pi * 17 * n / N) + 60 * np.sin(2 * np.pi * 41.3 * n / N)
    xi = np.rint(tones + rng.normal(0, 40, N)).astype(int)
    xq = np.rint(rng.normal(0, 40, N)).astype(int)
    X = np.fft.fft(xi + 1j * xq)
    assert np.abs(X).max() < 30000, "spectrum must not saturate the 16-bit output"
    Xi, Xq = np.rint(X.real).astype(int), np.rint(X.imag).astype(int)

    twiddle = os.path.join(HERE, "..", "..", "fft_twiddle_256.mem")
    model = FFTEngine(n=N, twiddle_file=twiddle, internal_w=24)
    mi, mq = model.compute(list(xi), list(xq), inverse=False)
    err = np.hypot(np.array(mi) - Xi, np.array(mq) - Xq)
    print(f"model vs numpy (forward): max {err.max():.1f}  rms {np.sqrt((err**2).mean()):.1f}")  # noqa: T201
    rms = np.sqrt((err ** 2).mean())
    assert err.max() <= MAX_ABS_ERR and rms <= MAX_RMS_ERR, "raise the documented tolerance"
    ri, rq = model.compute(list(Xi), list(Xq), inverse=True)
    inv_err = max(max(abs(np.array(ri) - xi)), max(abs(np.array(rq) - xq)))
    print(f"model inverse round-trip max err: {inv_err}")  # noqa: T201
    assert inv_err <= 4

    # Bit-exact reference for the RTL: the same vector through the model.
    write_hex(os.path.join(HERE, "fft256_mdl_i.hex"), mi, 16)
    write_hex(os.path.join(HERE, "fft256_mdl_q.hex"), mq, 16)

    # Adversarial full-scale vector (square-wave I/Q at the worst phase): the
    # 24-bit internal words can wrap by up to sqrt(2) over 2^23; the RTL must
    # wrap exactly like the model (no saturation inside the butterflies).
    fi = np.where(np.cos(2 * np.pi * 5 * n / N) >= 0, 32767, -32768)
    fq = np.where(np.sin(2 * np.pi * 5 * n / N) >= 0, 32767, -32768)
    fmi, fmq = model.compute(list(fi), list(fq), inverse=False)
    write_hex(os.path.join(HERE, "fft256_fs_in_i.hex"), fi, 16)
    write_hex(os.path.join(HERE, "fft256_fs_in_q.hex"), fq, 16)
    write_hex(os.path.join(HERE, "fft256_fs_mdl_i.hex"), fmi, 16)
    write_hex(os.path.join(HERE, "fft256_fs_mdl_q.hex"), fmq, 16)

    write_hex(os.path.join(HERE, "fft256_in_i.hex"), xi, 16)
    write_hex(os.path.join(HERE, "fft256_in_q.hex"), xq, 16)
    write_hex(os.path.join(HERE, "fft256_fwd_i.hex"), Xi, 16)
    write_hex(os.path.join(HERE, "fft256_fwd_q.hex"), Xq, 16)
    print("wrote fft256_*.hex")  # noqa: T201


if __name__ == "__main__":
    main()
