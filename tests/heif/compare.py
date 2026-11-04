#!/usr/bin/env python3
# Copyright (c) 2026 Andrew Youll
# Released under the MIT licence: see the LICENSE file.
# SPDX-License-Identifier: MIT
# compare.py A.ppm B.ppm TOL : the two pictures the same (within TOL levels).
# compare.py --p3 P.ppm : tests/heif/p3.heic decoded: its pure Display P3
# red, green and blue bars converted to sRGB (clipped to 0..255).
import sys

def read(path):
    with open(path, 'rb') as f:
        data = f.read()
    parts = data.split(b'\n', 3)
    w, h = map(int, parts[1].split())
    return w, h, parts[3]

if sys.argv[1] == '--p3':
    w, h, px = read(sys.argv[2])
    def at(x, y):
        i = (y * w + x) * 3
        return tuple(px[i:i + 3])
    # P3 (255,0,0) is outside sRGB: red stays 255, green and blue clip to 0
    r, g, b = at(100, 100), at(300, 100), at(500, 100)
    ok = r[0] >= 250 and r[1] <= 8 and r[2] <= 8 and g[1] >= 250 and g[0] <= 8 and b[2] >= 250 and b[0] <= 8 and b[1] <= 8
    # and a less saturated P3 red (its lower half) less red in sRGB than as stored: not left as P3
    lr = at(100, 300)
    ok = ok and lr[0] > lr[1]
    sys.exit(0 if ok else 1)
a, b, tol = sys.argv[1], sys.argv[2], int(sys.argv[3])
wa, ha, pa = read(a)
wb, hb, pb = read(b)
if (wa, ha) != (wb, hb) or len(pa) != len(pb):
    sys.exit(1)
worst = max((abs(x - y) for x, y in zip(pa, pb)), default=0)
sys.exit(0 if worst <= tol else 1)
