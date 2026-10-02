#!/usr/bin/env python3
"""gen_ddc_golden.py — golden test (a): bit-exact DDC vectors.

Integer replica of ddc.v (NCO -> 12x16 mixer -> CIC R=4 -> FIR):
  nco     : phase[n] = PHASE_INC*n mod 2^32, 8-bit truncation, quarter-wave LUT
  mixer   : mixed = (adc_signed * nco) >> 12   (floor), 16 bits
  cic     : Delta^5 S5[4j-2] >> 10, 26-bit wrap  (see cic_decimator_4x_enhanced.v)
  fir     : direct form, acc = sum c[k]*u[j-k]; output per DDC_OUT_W
Writes ddc_adc_in.hex (12-bit), ddc_golden_i.hex, ddc_golden_q.hex, plus the
full-scale vector set ddc_adc_fs_in.hex / ddc_fs_golden_i/q.hex (20 MHz-IF tone
clipped rail-to-rail, -2048..2047 signed, exercising the mixer/CIC/FIR extremes).
Run from 9_Firmware/9_2_FPGA:  python3 tb/golden/gen_ddc_golden.py
"""
import os
import sys

import numpy as np

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from radar_params import (DDC_OUT_W, FIR_COEFFS, NCO_SINE_LUT, PHASE_INC,
                          FS_ADC, if_chirp_adc, sext, write_hex)

N_IN   = 4096
ACC_W  = 26
MASK26 = (1 << ACC_W) - 1
HERE   = os.path.dirname(os.path.abspath(__file__))


def nco_sin_cos(n_samples):
    phase = (np.uint64(PHASE_INC) * np.arange(n_samples, dtype=np.uint64)) & np.uint64(0xFFFFFFFF)
    addr = (phase >> np.uint64(24)).astype(np.int64)
    quadrant = addr >> 6
    idx = addr & 63
    flip = (quadrant & 1) == 1          # nco.v: index mirrored in odd quadrants
    idx = np.where(flip, (~idx) & 63, idx)
    lut = np.array(NCO_SINE_LUT, dtype=np.int64)
    sin_abs, cos_abs = lut[idx], lut[63 - idx]
    sin = np.where((quadrant == 0) | (quadrant == 1), sin_abs, -sin_abs)
    cos = np.where((quadrant == 0) | (quadrant == 3), cos_abs, -cos_abs)
    return sin, cos


def cic_r4_n5(x):
    s = np.asarray(x, dtype=np.int64)
    for _ in range(5):
        s = np.cumsum(s) & MASK26
    spad = np.concatenate([[0, 0], s])
    v = np.array([spad[4 * j] for j in range(len(x) // 4)], dtype=np.int64)  # S5[4j-2]
    for _ in range(5):
        v = (v - np.concatenate([[0], v[:-1]])) & MASK26
    return np.array([sext(int(a), ACC_W) >> 10 for a in v], dtype=np.int64)


def fir(u, out_w):
    c = np.array(FIR_COEFFS, dtype=np.int64)
    acc = np.convolve(np.asarray(u, dtype=np.int64), c)[:len(u)]   # |acc| < 2^34
    if out_w == 18:   # legacy fir_lowpass_parallel_enhanced: saturate at 2^34, take acc[34:17]
        out = []
        for a in acc:
            if a > (1 << 34) - 1:
                out.append((1 << 17) - 1)
            elif a < -(1 << 34):
                out.append(-(1 << 17))
            else:
                out.append(sext((int(a) >> 17) & 0x3FFFF, 18))
        return out
    # fir_lowpass (Task 7): acc >>> 17, saturate to 16 bits
    return [int(min(max(int(a) >> 17, -32768), 32767)) for a in acc]


def ddc_model(adc):
    adc_signed = np.asarray(adc, dtype=np.int64) - 2048
    sin, cos = nco_sin_cos(len(adc_signed))
    mix_i = (adc_signed * cos) >> 12
    mix_q = (adc_signed * sin) >> 12
    cic_i, cic_q = cic_r4_n5(mix_i), cic_r4_n5(mix_q)
    return fir(cic_i, DDC_OUT_W), fir(cic_q, DDC_OUT_W)


def full_scale_adc(n_samples):
    """20 MHz-IF tone, 2400 LSB amplitude hard-clipped to the ADC rails: both
    -2048 and +2047 (signed) occur many times, the worst case for the mixer
    (|product| near 2^14), the CIC accumulators and the FIR."""
    n = np.arange(n_samples)
    tone = 2400.0 * np.cos(2 * np.pi * 20e6 / FS_ADC * n + 0.3)
    sig = np.clip(np.rint(tone), -2048, 2047).astype(np.int64)
    adc = sig + 2048
    assert adc.min() == 0 and adc.max() == 4095
    return adc


def main():
    adc = if_chirp_adc(N_IN, amp=1000.0, delay=0, seed=1)
    out_i, out_q = ddc_model(adc)
    write_hex(os.path.join(HERE, "ddc_adc_in.hex"), adc, 12)
    write_hex(os.path.join(HERE, "ddc_golden_i.hex"), out_i, DDC_OUT_W)
    write_hex(os.path.join(HERE, "ddc_golden_q.hex"), out_q, DDC_OUT_W)
    print(f"wrote {N_IN} ADC samples, {len(out_i)} baseband samples "  # noqa: T201
          f"(out width {DDC_OUT_W}), peak |I| = {max(abs(v) for v in out_i)}")
    fs = full_scale_adc(N_IN)
    fi, fq = ddc_model(fs)
    write_hex(os.path.join(HERE, "ddc_adc_fs_in.hex"), fs, 12)
    write_hex(os.path.join(HERE, "ddc_fs_golden_i.hex"), fi, DDC_OUT_W)
    write_hex(os.path.join(HERE, "ddc_fs_golden_q.hex"), fq, DDC_OUT_W)
    print(f"full-scale: peak |I| = {max(abs(v) for v in fi)}, "  # noqa: T201
          f"peak |Q| = {max(abs(v) for v in fq)}")


if __name__ == "__main__":
    main()
