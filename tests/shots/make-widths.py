#!/usr/bin/env python3
# Copyright (c) 2026 Andrew Youll
# Released under the MIT licence: see the LICENSE file.
# SPDX-License-Identifier: MIT
"""Writes widths.h: the advance of each Latin-1 character in OS units, for
   the font render.py draws text with (standing in for the desktop font),
   so Lumen's text measuring in the harness matches the picture."""
from PIL import ImageFont
FONT = '/usr/share/fonts/truetype/dejavu/DejaVuSans.ttf'
SIZE = 26                      # in OS units (13 px at 2 OS units a pixel)
f = ImageFont.truetype(FONT, SIZE)
w = []
for c in range(256):
    ch = bytes([c]).decode('latin-1')
    w.append(int(round(f.getlength(ch))) if c >= 32 else 0)
with open('widths.h', 'w') as o:
    o.write('/* Made by make-widths.py: DejaVu Sans at %d OS units. */\n' % SIZE)
    o.write('static const unsigned char char_width[256] = {\n')
    for i in range(0, 256, 16):
        o.write('  ' + ', '.join(str(x) for x in w[i:i + 16]) + ',\n')
    o.write('};\n')
