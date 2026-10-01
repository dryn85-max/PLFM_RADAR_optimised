#!/usr/bin/env python3
"""Float reference for the ADAR1000 beam table.

Elements are spaced lambda/2, so the per-element phase step is 180 deg * sin(el).
The ADAR1000 phase LSB is 360/128 = 2.8125 deg, hence

    idx(n) = round(n * 180 * sin(el) / 2.8125) mod 128,  n = 0..15

Usage: ref_beam_table.py [el_deg ...]   (default: -45 -20 10 30 60)
Prints one line per angle: "<el> <idx n=0> ... <idx n=15>".
"""
import math
import sys


def table(el_deg):
    s = math.sin(math.radians(el_deg))
    return [round(n * 180.0 * s / 2.8125) % 128 for n in range(16)]


def main(argv):
    angles = [int(a) for a in argv] or [-45, -20, 10, 30, 60]
    for a in angles:
        print(a, *table(a))


if __name__ == "__main__":
    main(sys.argv[1:])
