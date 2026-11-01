#!/usr/bin/env python3
"""Draws Lumen's icons: a lens barrel with a six-blade iris, the blades
   twisted like a real aperture, round a hexagonal opening.
   app/make-icons.py -> app/lumen.png (34x34) and app/sm-lumen.png (17x17).
   Drawn at 8x and averaged down, so the edges are smooth."""
import math, os
from PIL import Image

BARREL = (0x2A, 0x2A, 0x2E)
BLADES = [(0xD9, 0x8A, 0x2B), (0xB8, 0x6A, 0x1C)]   # two amber tones, alternating
OPENING = (0x9C, 0xC4, 0xE4)                        # sky seen through the lens
GLINT = (0xF4, 0xF8, 0xFC)


def hexagon_radius(angle, r):
    """Distance to a regular hexagon's edge (circumradius r) at this angle."""
    a = (angle % 60) - 30
    return r * math.cos(math.radians(30)) / math.cos(math.radians(a))


def icon(size):
    big = size * 8
    img = Image.new('RGBA', (big, big), (0, 0, 0, 0))
    px = img.load()
    c = (big - 1) / 2
    outer, rim = big * 0.48, big * 0.40
    hole = big * 0.16
    for y in range(big):
        for x in range(big):
            dx, dy = x - c, c - y
            r = math.hypot(dx, dy)
            if r > outer:
                continue
            ang = (math.degrees(math.atan2(dy, dx)) + 360) % 360
            if r > rim:
                px[x, y] = BARREL + (255,)
            elif r < hexagon_radius(ang + 15, hole):
                # the opening, with a small highlight up and left
                g = math.hypot(dx + hole * 0.35, dy - hole * 0.35)
                px[x, y] = (GLINT if g < hole * 0.25 else OPENING) + (255,)
            else:
                # blades: sectors twisted with the radius, edged in dark
                twisted = ang + (r / rim) * 55
                k = int(twisted // 60)
                edge = (twisted % 60) < 3.5
                px[x, y] = (BARREL if edge else BLADES[k % 2]) + (255,)
    return img.resize((size, size), Image.LANCZOS)


here = os.path.dirname(os.path.abspath(__file__))
icon(34).save(os.path.join(here, 'lumen.png'))
icon(17).save(os.path.join(here, 'sm-lumen.png'))
