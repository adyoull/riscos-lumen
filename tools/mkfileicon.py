#!/usr/bin/env python3
# Copyright (c) 2026 Andrew Youll
# Released under the MIT licence: see the LICENSE file.
# SPDX-License-Identifier: MIT
"""mkfileicon.py OUT_LARGE.png OUT_SMALL.png LUMEN.png

Lumen's file icon, in the style of the RISC OS desktop's (riscosdraw.py,
shared with riscos-gtk's icons): a sheet with its corner folded, a framed
picture on it, and Lumen's own icon in the corner. Lumen gives it to the
file types it opens that the desktop has no icon for (filetypes.c), as
file_xxx (34 x 34) and small_xxx (17 x 17)."""
import sys
from PIL import Image
from riscosdraw import Icon, sheet, K, GREEN, GREEN2, YELLOW, ORANGE, ORANGE2


def picture(c, x0, y0, x1, y1):
    """a framed picture: sky, a hill and the sun"""
    c.rect(x0, y0, x1, y1, ((150, 200, 255, 255), (220, 240, 255, 255)))
    w, h = x1 - x0, y1 - y0
    c.poly([(x0 + 0.5, y1 - 0.5), (x0 + w * 0.35, y0 + h * 0.45), (x0 + w * 0.6, y0 + h * 0.7),
            (x0 + w * 0.75, y0 + h * 0.55), (x1 - 0.5, y1 - 0.5)], (GREEN, GREEN2), GREEN2, 0.8)
    c.ellipse(x1 - w * 0.32, y0 + h * 0.14, x1 - w * 0.12, y0 + h * 0.14 + w * 0.2, (YELLOW, ORANGE), ORANGE2, 0.8)
    c.rect(x0, y0, x1, y1, None, K)


def make(side, logo, small):
    c = Icon(24, 24, side / 24.0, minline=True)
    sheet(c, 4.5, 2.5, 19.5, 21.5, lines=False)
    picture(c, 6.5, 6.5, 17.5, 14.5)
    im = c.image()
    # Lumen's icon, bottom right: who opens it
    m = side * (8 if small else 7) // 17
    l = logo.resize((m, m), Image.LANCZOS)
    im.alpha_composite(l, (side - m, side - m))
    return im


if __name__ == "__main__":
    logo = Image.open(sys.argv[3]).convert("RGBA")
    make(34, logo, False).save(sys.argv[1])
    make(17, logo, True).save(sys.argv[2])
