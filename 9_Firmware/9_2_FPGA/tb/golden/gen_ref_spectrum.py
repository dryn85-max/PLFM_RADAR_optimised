#!/usr/bin/env python3
"""gen_ref_spectrum.py — precomputed reference spectra for ref_spectrum_rom.v.

ROM layout (addr = {seg[2:0], k[7:0]}):
  seg 0..3 : FFT_256( long_chirp[224*seg : 224*seg + 256] )   (750-sample 30 us chirp
             at 25 MSPS, zero-padded; segment windows match the overlap-save
             advance of matched_filter_multi_segment.v so a target at delay d
             peaks at bin d in every segment)
  seg 4    : FFT_256( short_chirp[0:13] zero-padded )
Scaling: long segments share one factor (peak = 0.9*32767 over all four),
the short segment has its own factor.  Values are rounded, 16-bit two's
complement, written with numpy's forward FFT (e^{-j...}) convention, which is
the convention of fft_engine.v.
Writes ../../ref_spectrum_i.mem and ../../ref_spectrum_q.mem (1280 lines each).
"""
import os
import sys

import numpy as np

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
from radar_params import (ADVANCE, LONG_CHIRP_SAMPLES, LONG_SEGMENTS, N_FFT,  # noqa: E402
                          SHORT_CHIRP_SAMPLES, T_LONG, T_SHORT, baseband_chirp, write_hex)

N_SEG = LONG_SEGMENTS + 1
Q15_PEAK = 0.9 * 32767


def long_chirp_padded():
    ci, cq = baseband_chirp(LONG_CHIRP_SAMPLES, T_LONG)
    total = ADVANCE * (LONG_SEGMENTS - 1) + N_FFT          # 928
    c = np.zeros(total, dtype=complex)
    c[:LONG_CHIRP_SAMPLES] = ci + 1j * cq
    return c


def short_chirp_padded():
    ci, cq = baseband_chirp(SHORT_CHIRP_SAMPLES, T_SHORT)
    c = np.zeros(N_FFT, dtype=complex)
    c[:SHORT_CHIRP_SAMPLES] = ci + 1j * cq
    return c


def ref_segment_spectra():
    """Return list of N_SEG arrays of complex ints (the ROM contents)."""
    lc = long_chirp_padded()
    long_specs = [np.fft.fft(lc[ADVANCE * s: ADVANCE * s + N_FFT]) for s in range(LONG_SEGMENTS)]
    short_spec = np.fft.fft(short_chirp_padded())
    scale_long = Q15_PEAK / max(np.abs(S).max() for S in long_specs)
    scale_short = Q15_PEAK / np.abs(short_spec).max()
    out = [np.rint(S * scale_long) for S in long_specs] + [np.rint(short_spec * scale_short)]
    return [np.clip(S.real, -32768, 32767).astype(int) + 1j * np.clip(S.imag, -32768, 32767).astype(int)
            for S in out]


def main():
    specs = ref_segment_spectra()
    rom_i = np.concatenate([S.real.astype(int) for S in specs])
    rom_q = np.concatenate([S.imag.astype(int) for S in specs])
    assert len(rom_i) == N_SEG * N_FFT
    root = os.path.join(HERE, "..", "..")
    write_hex(os.path.join(root, "ref_spectrum_i.mem"), rom_i, 16)
    write_hex(os.path.join(root, "ref_spectrum_q.mem"), rom_q, 16)
    for s, S in enumerate(specs):
        print(f"seg {s}: peak |S| = {int(np.abs(S).max())}, nonzero bins = {int(np.sum(np.abs(S) > 0))}")
    print(f"wrote {N_SEG * N_FFT} entries to ref_spectrum_i.mem / ref_spectrum_q.mem")


if __name__ == "__main__":
    main()
