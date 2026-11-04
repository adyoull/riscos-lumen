#!/usr/bin/env python3
# Copyright (c) 2026 Andrew Youll
# Released under the MIT licence: see the LICENSE file.
# SPDX-License-Identifier: MIT
"""Draws Lumen's icons: a lens with a six-blade iris, the blades twisted
   like a real aperture round a hexagonal opening, in the RISC OS desktop's
   own colours: the Wimp's dark blue (colour 8, #004499) and light blue
   (colour 15, #00BBFF) for the blades, its cream (colour 12) for the light
   coming through, a grey metal barrel. Lit from the top left, like the
   rest of the desktop's icons, with a dark outline.
   app/make-icons.py -> app/lumen.png (34x34) and app/sm-lumen.png (17x17).
   Drawn at 8x and averaged down, so the edges are smooth."""
import math, os
from PIL import Image

OUTLINE = (0x10, 0x18, 0x30)
BARREL_LIT, BARREL_DARK = (0xE8, 0xE8, 0xE8), (0x70, 0x74, 0x7C)
BLADE_A = ((0x1E, 0x6E, 0xC8), (0x00, 0x2E, 0x6E))   # lit and shaded, alternate blades
BLADE_B = ((0x00, 0x56, 0xAA), (0x00, 0x22, 0x58))
LIGHT_IN, LIGHT_OUT = (0xFF, 0xFF, 0xF0), (0xEE, 0xE0, 0x9A)   # cream, glowing in the middle
GLINT = (0xFF, 0xFF, 0xFF)


def mix(a, b, t):
    t = max(0.0, min(1.0, t))
    return tuple(int(round(a[i] + (b[i] - a[i]) * t)) for i in range(3))


def hexagon_radius(angle, r):
    """Distance to a regular hexagon's edge (circumradius r) at this angle."""
    a = (angle % 60) - 30
    return r * math.cos(math.radians(30)) / math.cos(math.radians(a))


def icon(size):
    big = size * 8
    img = Image.new('RGBA', (big, big), (0, 0, 0, 0))
    px = img.load()
    c = (big - 1) / 2
    outer = big * 0.48
    line = big * 0.035                   # outline width
    rim = big * 0.38                     # inside of the barrel
    hole = big * 0.15
    for y in range(big):
        for x in range(big):
            dx, dy = x - c, c - y
            r = math.hypot(dx, dy)
            if r > outer:
                continue
            ang = (math.degrees(math.atan2(dy, dx)) + 360) % 360
            # light from the top left: 1 there, 0 bottom right
            lit = 0.5 + 0.5 * (-dx + dy) / (math.sqrt(2) * max(r, 1e-6)) if r > 0 else 0.5
            if r > outer - line or rim - line * 0.6 < r <= rim:
                col = OUTLINE
            elif r > rim:
                # the barrel: a bevelled metal ring, bright at top left
                across = (r - rim) / (outer - line - rim)          # 0 inner .. 1 outer
                bevel = lit if across > 0.5 else 1 - lit           # raised outer edge, sunk inner edge
                col = mix(BARREL_DARK, BARREL_LIT, 0.25 + 0.75 * bevel)
            elif r < hexagon_radius(ang + 15, hole):
                # light through the lens, brightest in the middle, a glint up and left
                g = math.hypot(dx + hole * 0.38, dy - hole * 0.38)
                col = GLINT if g < hole * 0.22 else mix(LIGHT_IN, LIGHT_OUT, r / hole)
            elif r < hexagon_radius(ang + 15, hole) + line * 0.7:
                col = OUTLINE                                      # the opening's edge
            else:
                # blades: sectors twisted with the radius, each shaded across
                twisted = ang + (r / rim) * 55
                k = int(twisted // 60)
                within = (twisted % 60) / 60                       # 0 at a blade's leading edge
                if within < 0.06:
                    col = OUTLINE
                else:
                    lo, hi = (BLADE_A if k % 2 == 0 else BLADE_B)[1], (BLADE_A if k % 2 == 0 else BLADE_B)[0]
                    shade = 0.55 * lit + 0.45 * (1 - within)       # the light, and each blade's curve
                    col = mix(lo, hi, shade)
            px[x, y] = col + (255,)
    return img.resize((size, size), Image.LANCZOS)


here = os.path.dirname(os.path.abspath(__file__))
icon(34).save(os.path.join(here, 'lumen.png'))
icon(17).save(os.path.join(here, 'sm-lumen.png'))
if os.environ.get('LUMEN_ICON_PREVIEW'):
    icon(256).save(os.environ['LUMEN_ICON_PREVIEW'])
