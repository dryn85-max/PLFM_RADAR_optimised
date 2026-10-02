#!/usr/bin/env python3
"""gen_twiddle_rom.py N — quarter-wave cosine ROM for fft_engine.v.

Writes ../../fft_twiddle_<N>.mem with N/4 entries, round(32767*cos(2*pi*k/N)),
16-bit two's complement, matching the existing fft_twiddle_1024.mem/16.mem.
"""
import math
import os
import sys


def main():
    n = int(sys.argv[1]) if len(sys.argv) > 1 else 256
    assert n & (n - 1) == 0 and n >= 16, "N must be a power of two >= 16"
    here = os.path.dirname(os.path.abspath(__file__))
    path = os.path.join(here, "..", "..", f"fft_twiddle_{n}.mem")
    with open(path, "w") as f:
        f.write(f"// Quarter-wave cosine ROM for {n}-point FFT\n")
        f.write(f"// {n // 4} entries, 16-bit signed Q15 ($readmemh format)\n")
        f.write(f"// cos(2*pi*k/{n}) for k = 0..{n // 4 - 1}\n")
        for k in range(n // 4):
            f.write(f"{round(32767 * math.cos(2 * math.pi * k / n)) & 0xFFFF:04X}\n")
    print(f"wrote {os.path.normpath(path)}")  # noqa: T201


if __name__ == "__main__":
    main()
