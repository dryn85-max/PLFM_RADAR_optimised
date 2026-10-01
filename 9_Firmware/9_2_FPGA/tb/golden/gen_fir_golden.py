#!/usr/bin/env python3
"""gen_fir_golden.py — golden test (b): direct-form 32-tap FIR, exact integers.

The folded RTL computes sum_{k<16} c[k]*(x[n-k] + x[n-31+k]); because the
coefficients are symmetric this equals the direct form exactly, so the
expected output is  clip(acc >> 17, -32768, 32767)  with acc from np.convolve.
Writes fir_in.hex (16-bit) and fir_golden.hex (16-bit).
"""
import os
import sys

import numpy as np

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from radar_params import FIR_COEFFS, write_hex  # noqa: E402

N = 2048
HERE = os.path.dirname(os.path.abspath(__file__))


def main():
    rng = np.random.default_rng(7)
    n = np.arange(N)
    tone = 12000 * np.cos(2 * np.pi * 0.03 * n)            # in-band
    tone2 = 9000 * np.cos(2 * np.pi * 0.37 * n)            # stop-band
    noise = rng.normal(0, 3000, N)
    x = np.clip(np.rint(tone + tone2 + noise), -32768, 32767).astype(np.int64)
    x[100:110] = 32767                                      # full-scale burst
    x[300:310] = -32768
    acc = np.convolve(x, np.array(FIR_COEFFS, dtype=np.int64))[:N]
    y = np.clip(acc >> 17, -32768, 32767)
    write_hex(os.path.join(HERE, "fir_in.hex"), x, 16)
    write_hex(os.path.join(HERE, "fir_golden.hex"), y, 16)
    sat = int(np.sum((acc >> 17) != y))
    print(f"wrote {N} samples; {sat} outputs saturated; max|y| = {int(np.max(np.abs(y)))}")


if __name__ == "__main__":
    main()
