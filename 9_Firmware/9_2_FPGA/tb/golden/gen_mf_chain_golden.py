#!/usr/bin/env python3
"""gen_mf_chain_golden.py — vectors for tb/tb_mf_chain.v.

Signal = long-chirp segment 0 (first 256 baseband samples), amplitude 400
(no forward-FFT saturation), delayed by 0 / 20 / 100 samples (zero-filled).
Expected output = IFFT( FFT(sig) * conj(ROM seg 0) ) computed with the
bit-accurate model (fpga_model.FFTEngine internal_w=25 + FreqMatchedFilter),
which the synthesizable branch must match exactly.  The peak bin equals the
delay; tb_mf_chain.v checks that for the behavioral branch too.
Writes mf_sig_d{D}_i/q.hex (256 x 16-bit) and mf_gold_d{D}_i/q.hex.
"""
import os
import sys

import numpy as np

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
sys.path.insert(0, os.path.join(HERE, "..", "cosim"))
from fpga_model import FFTEngine, FreqMatchedFilter  # noqa: E402
from gen_ref_spectrum import long_chirp_padded, ref_segment_spectra  # noqa: E402
from radar_params import N_FFT, write_hex  # noqa: E402

AMP = 400
DELAYS = (0, 20, 100)
MS_DELAYS = (20, 100, 150)   # continuous 4-segment stream (tb_mf_multiseg.v)
MS_LEN = 1024


def main():
    rom0 = ref_segment_spectra()[0]
    rom_i = [int(v) for v in rom0.real]
    rom_q = [int(v) for v in rom0.imag]
    chirp = long_chirp_padded()
    twiddle = os.path.join(HERE, "..", "..", "fft_twiddle_256.mem")
    fft = FFTEngine(n=N_FFT, twiddle_file=twiddle, internal_w=25)
    for d in DELAYS:
        sig = np.zeros(N_FFT, dtype=complex)
        sig[d:] = chirp[:N_FFT - d]
        si = [round(AMP * v.real) for v in sig]
        sq = [round(AMP * v.imag) for v in sig]
        fi, fq = fft.compute(si, sq, inverse=False)
        assert max(max(map(abs, fi)), max(map(abs, fq))) < 32767, "forward FFT saturates; lower AMP"
        pi, pq = FreqMatchedFilter.process_block(fi, fq, rom_i, rom_q)
        oi, oq = fft.compute(pi, pq, inverse=True)
        mag = np.hypot(oi, oq)
        peak = int(np.argmax(mag))
        print(f"delay {d:3d}: peak bin {peak}, peak {mag.max():.0f}, mean {mag.mean():.0f}")  # noqa: T201
        assert peak == d
        write_hex(os.path.join(HERE, f"mf_sig_d{d}_i.hex"), si, 16)
        write_hex(os.path.join(HERE, f"mf_sig_d{d}_q.hex"), sq, 16)
        write_hex(os.path.join(HERE, f"mf_gold_d{d}_i.hex"), oi, 16)
        write_hex(os.path.join(HERE, f"mf_gold_d{d}_q.hex"), oq, 16)
    print("wrote mf_sig_*/mf_gold_* vectors")  # noqa: T201
    fullscale_vector(rom_i, rom_q, twiddle)
    multiseg_vectors()


def fullscale_vector(rom_i, rom_q, twiddle):
    """Full-scale chain-level vector; must distinguish INTERNAL_W 25 from 24."""
    n = np.arange(N_FFT)
    si = [int(v) for v in np.where(np.cos(2 * np.pi * 5 * n / N_FFT) >= 0, 32767, -32768)]
    sq = [int(v) for v in np.where(np.sin(2 * np.pi * 5 * n / N_FFT) >= 0, 32767, -32768)]

    def chain(internal_w):
        eng = FFTEngine(n=N_FFT, twiddle_file=twiddle, internal_w=internal_w)
        fi, fq = eng.compute(si, sq, inverse=False)
        pi, pq = FreqMatchedFilter.process_block(fi, fq, rom_i, rom_q)
        return eng.compute(pi, pq, inverse=True)

    oi, oq = chain(25)
    wi, wq = chain(24)
    diff = max(
        max(abs(a - b) for a, b in zip(oi, wi, strict=True)),
        max(abs(a - b) for a, b in zip(oq, wq, strict=True)),
    )
    print(f"full-scale chain: 24-bit model differs from 25-bit by up to {diff} LSB")  # noqa: T201
    assert diff > 0, "full-scale chain vector does not detect a 24-bit internal width"
    write_hex(os.path.join(HERE, "mf_sig_fs_i.hex"), si, 16)
    write_hex(os.path.join(HERE, "mf_sig_fs_q.hex"), sq, 16)
    write_hex(os.path.join(HERE, "mf_gold_fs_i.hex"), oi, 16)
    write_hex(os.path.join(HERE, "mf_gold_fs_q.hex"), oq, 16)
    print("wrote mf_sig_fs/mf_gold_fs vectors")  # noqa: T201


def multiseg_vectors():
    """Whole receive window (928 samples = 4 overlap-save segments; chirp delayed
    by d, so its tail lies in the later segments) for the segmenter test.
    Segment s sees chirp samples [224s, 224s+256) in its
    window and the ROM holds the same window of the chirp, so for d <= 178
    (= 928 - 750, the echo tail stays inside the window) the peak of every
    segment is at bin d.  Checked here with the ideal float
    correlation; the testbench checks the same on the RTL."""
    chirp = long_chirp_padded()
    for d in MS_DELAYS:
        x = np.zeros(MS_LEN, dtype=complex)
        n = np.arange(928)               # whole receive window, echo tail included
        src = n - d
        ok = src >= 0
        x[:928][ok] = chirp[src[ok]]     # chirp[] is zero past sample 749
        si = [round(AMP * v.real) for v in x]
        sq = [round(AMP * v.imag) for v in x]
        for s in range(4):
            win = x[224 * s: 224 * s + N_FFT]
            ref = chirp[224 * s: 224 * s + N_FFT]
            c = np.fft.ifft(np.fft.fft(win) * np.conj(np.fft.fft(ref)))
            assert int(np.argmax(np.abs(c))) == d, (d, s)
        write_hex(os.path.join(HERE, f"mf_ms_d{d}_i.hex"), si, 16)
        write_hex(os.path.join(HERE, f"mf_ms_d{d}_q.hex"), sq, 16)
    print("wrote mf_ms_* vectors")  # noqa: T201


if __name__ == "__main__":
    main()
