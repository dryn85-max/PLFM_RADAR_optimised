#!/usr/bin/env python3
"""gen_fullchain_golden.py — golden test (d): ADC vectors for a target echo.

Two 4096-sample 12-bit ADC vectors: the 30 us IF chirp (10..30 MHz) starting
D1 = 100 and D2 = 260 ADC samples after the chirp-start pulse, amplitude
0.25 FS.  Through the DDC (x4 decimation) the delays differ by
(D2 - D1) / 4 = 40 range bins, independent of the pipeline latency, so the
testbench checks  peak_bin(D2) - peak_bin(D1) == 40 (+-1).
Writes fullchain_adc_d1.hex / fullchain_adc_d2.hex.
"""
import os
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
from radar_params import if_chirp_adc, write_hex  # noqa: E402

N_IN = 4096
D1, D2 = 100, 260
AMP = 512.0


def main():
    for name, d in (("d1", D1), ("d2", D2)):
        adc = if_chirp_adc(N_IN, AMP, d, seed=11)
        write_hex(os.path.join(HERE, f"fullchain_adc_{name}.hex"), adc, 12)
    print(f"wrote fullchain_adc_d1/d2.hex; expected peak displacement = {(D2 - D1) // 4} bins")  # noqa: T201


if __name__ == "__main__":
    main()
