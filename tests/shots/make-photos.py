#!/usr/bin/env python3
# Copyright (c) 2026 Andrew Youll
# Released under the MIT licence: see the LICENSE file.
# SPDX-License-Identifier: MIT
"""Makes a folder of landscape 'photos' (JPEG) for the screenshot run:
   painted skies, sun or moon, layered hills with texture, water, trees.
   make-photos.py OUTDIR"""
import math, os, sys
import numpy as np
from PIL import Image, ImageFilter

rng = np.random.default_rng(7)
W, H = 1200, 800


def lerp(a, b, t):
    return a + (b - a) * t


def sky(w, h, top, mid, low):
    y = np.linspace(0, 1, h)[:, None, None]
    top, mid, low = (np.array(c, float) for c in (top, mid, low))
    upper = lerp(top, mid, np.clip(y / 0.6, 0, 1))
    lower = lerp(mid, low, np.clip((y - 0.6) / 0.4, 0, 1))
    img = np.where(y < 0.6, upper, lower)
    return np.broadcast_to(img, (h, w, 3)).copy()


def noise1d(n, octaves=5, seed=0):
    r = np.random.default_rng(seed)
    out = np.zeros(n)
    for o in range(octaves):
        k = 2 ** (o + 2)
        pts = r.uniform(-1, 1, k + 1)
        xs = np.linspace(0, k, n)
        i = np.floor(xs).astype(int)
        f = xs - i
        f = f * f * (3 - 2 * f)
        out += (pts[np.minimum(i, k)] * (1 - f) + pts[np.minimum(i + 1, k)] * f) / (1.8 ** o)
    return out


def ridge(img, base, amp, colour, seed, haze=0.0, haze_col=None, texture=0.06):
    h, w, _ = img.shape
    line = base * h + amp * h * noise1d(w, seed=seed)
    ys = np.arange(h)[:, None]
    mask = ys > line[None, :]
    col = np.array(colour, float)
    tex = 1 + texture * rng.standard_normal((h, w))[..., None]
    shade = 1 - 0.25 * np.clip((ys - line[None, :]) / (0.35 * h), 0, 1)[..., None]
    layer = col * tex * shade
    if haze and haze_col is not None:
        layer = lerp(layer, np.array(haze_col, float), haze)
    img[mask] = layer[mask]
    return line


def disc(img, cx, cy, r, colour, glow=0.0):
    h, w, _ = img.shape
    yy, xx = np.mgrid[0:h, 0:w]
    d = np.hypot(xx - cx, yy - cy)
    col = np.array(colour, float)
    if glow:
        g = np.clip(1 - (d - r) / (glow * w), 0, 1)[..., None] ** 2 * 0.55
        img[:] = lerp(img, col, g)
    m = d < r
    img[m] = col


def clouds(img, seed):
    """Soft flat-bottomed cumulus: sums of wide gaussian blobs."""
    h, w, _ = img.shape
    r = np.random.default_rng(seed)
    yy, xx = np.mgrid[0:h, 0:w]
    cover = np.zeros((h, w))
    for _ in range(4):
        cx, cy = r.uniform(0.1, 0.9) * w, r.uniform(0.10, 0.28) * h
        for _ in range(7):
            bx, by = cx + r.normal(0, 70), cy + r.normal(0, 14)
            sx, sy = r.uniform(50, 110), r.uniform(18, 34)
            blob = np.exp(-(((xx - bx) / sx) ** 2 + ((yy - by) / sy) ** 2))
            blob[yy > cy + 26] *= np.exp(-((yy - cy - 26) / 8.0) ** 2)[yy > cy + 26]
            cover += blob
    a = np.clip(cover, 0, 1)[..., None] * 0.92
    shade = (1 - 0.12 * np.clip((yy - 0.12 * h) / (0.2 * h), 0, 1))[..., None]
    img[:] = lerp(img, np.array((246, 247, 250), float) * shade, a)


def water(img, top, tint, reflect=True):
    h, w, _ = img.shape
    t = int(top * h)
    if reflect:
        src = img[max(0, 2 * t - h):t][::-1]
        img[t:t + len(src)] = src
    y = np.linspace(0, 1, h - t)[:, None, None]
    ripple = 1 + 0.05 * np.sin(np.arange(h - t)[:, None, None] * 0.9 + rng.uniform(0, 6, (1, w, 1)))
    img[t:] = lerp(img[t:] * ripple, np.array(tint, float), 0.35 + 0.35 * y)


def trees(img, base, count, colour, seed, size=0.08):
    h, w, _ = img.shape
    r = np.random.default_rng(seed)
    for _ in range(count):
        x = r.uniform(0, w)
        th = r.uniform(0.6, 1.0) * size * h
        tw = th * 0.35
        yb = base * h + r.uniform(-0.01, 0.02) * h
        yy, xx = np.mgrid[int(yb - th):int(yb), int(max(0, x - tw)):int(min(w, x + tw))]
        if yy.size == 0:
            continue
        inside = np.abs(xx - x) < tw * (yy - (yb - th)) / th
        sub = img[int(yb - th):int(yb), int(max(0, x - tw)):int(min(w, x + tw))]
        sub[inside] = colour


def finish(img, path, blur=0.6, size=(W, H), rotate=False):
    a = np.clip(img, 0, 255).astype(np.uint8)
    im = Image.fromarray(a).filter(ImageFilter.GaussianBlur(blur))
    if rotate:
        im = im.transpose(Image.ROTATE_90)
    im.save(path, quality=90)


def scene(name, kind, seed, out):
    global rng
    rng = np.random.default_rng(seed)
    p = os.path.join(out, name + '.jpg')
    if kind == 'dawn':
        img = sky(W, H, (90, 110, 170), (240, 170, 120), (250, 210, 160))
        disc(img, 820, 330, 34, (255, 240, 200), glow=0.25)
        ridge(img, 0.45, 0.08, (110, 100, 130), seed, haze=0.5, haze_col=(220, 160, 140))
        ridge(img, 0.55, 0.07, (70, 80, 90), seed + 1, haze=0.3, haze_col=(200, 140, 120))
        ridge(img, 0.66, 0.05, (40, 55, 45), seed + 2)
        water(img, 0.74, (60, 70, 100))
    elif kind == 'glow':
        img = sky(W, H, (60, 70, 120), (230, 120, 90), (255, 190, 120))
        disc(img, 400, 470, 40, (255, 220, 160), glow=0.3)
        ridge(img, 0.52, 0.12, (80, 50, 70), seed, haze=0.4, haze_col=(200, 110, 90))
        ridge(img, 0.68, 0.06, (40, 30, 45), seed + 1)
        trees(img, 0.72, 40, (25, 20, 30), seed, 0.09)
    elif kind == 'mist':
        img = sky(W, H, (180, 190, 200), (210, 215, 220), (225, 228, 230))
        ridge(img, 0.42, 0.1, (150, 160, 170), seed, haze=0.6, haze_col=(215, 220, 225))
        ridge(img, 0.55, 0.08, (110, 125, 125), seed + 1, haze=0.45, haze_col=(210, 215, 220))
        ridge(img, 0.70, 0.05, (60, 80, 65), seed + 2, haze=0.15, haze_col=(200, 205, 210))
        trees(img, 0.74, 60, (40, 55, 45), seed, 0.1)
    elif kind == 'lake':
        img = sky(W, H, (70, 120, 190), (140, 180, 220), (200, 220, 235))
        disc(img, 1020, 140, 30, (255, 252, 235), glow=0.12)
        clouds(img, seed)
        ridge(img, 0.40, 0.1, (90, 110, 120), seed, haze=0.35, haze_col=(170, 195, 215))
        ridge(img, 0.52, 0.06, (60, 95, 60), seed + 1)
        water(img, 0.60, (40, 80, 120))
    elif kind == 'stones':
        img = sky(W, H, (120, 140, 170), (190, 190, 190), (210, 205, 195))
        ridge(img, 0.45, 0.07, (120, 120, 140), seed, haze=0.4, haze_col=(190, 190, 195))
        ridge(img, 0.62, 0.03, (95, 120, 70), seed + 1, texture=0.1)
        r = np.random.default_rng(seed)
        for i in range(9):
            x = 120 + i * 120 + r.uniform(-20, 20)
            hgt = r.uniform(60, 130)
            yb = 0.66 * H + r.uniform(0, 80)
            y0, y1, x0 = int(yb - hgt), int(yb), int(x - 22)
            img[y0:y1, x0:x0 + 44] = np.array((115, 110, 105)) * (1 + 0.08 * rng.standard_normal((y1 - y0, 44, 1)))
    elif kind == 'pines':
        img = sky(W, H, (100, 150, 200), (170, 200, 220), (210, 225, 230))
        ridge(img, 0.35, 0.12, (110, 120, 140), seed, haze=0.4, haze_col=(180, 200, 215))
        ridge(img, 0.58, 0.05, (60, 90, 60), seed + 1)
        trees(img, 0.62, 70, (30, 55, 40), seed, 0.18)
        water(img, 0.66, (50, 90, 110))
    elif kind == 'night':
        img = sky(W, H, (10, 14, 35), (25, 35, 70), (60, 70, 100))
        r = np.random.default_rng(seed)
        for _ in range(300):
            x, y = int(r.uniform(0, W)), int(r.uniform(0, H * 0.5))
            img[y, x] = 230
        disc(img, 900, 160, 26, (240, 238, 220), glow=0.06)
        ridge(img, 0.55, 0.08, (25, 30, 45), seed)
        r2 = np.random.default_rng(seed + 9)
        for _ in range(70):
            x, y = int(r2.uniform(200, 900)), int(r2.uniform(0.64 * H, 0.72 * H))
            img[y:y + 3, x:x + 3] = (255, 210, 120)
        water(img, 0.74, (15, 20, 40))
    elif kind == 'sunset':
        img = sky(W, H, (80, 60, 120), (240, 110, 60), (255, 200, 90))
        disc(img, 600, 520, 60, (255, 230, 150), glow=0.35)
        ridge(img, 0.62, 0.1, (60, 40, 55), seed)
        ridge(img, 0.75, 0.05, (30, 22, 30), seed + 1)
    elif kind == 'path':
        img = sky(W, H, (120, 170, 210), (180, 210, 225), (220, 230, 225))
        ridge(img, 0.40, 0.08, (120, 150, 120), seed, haze=0.3, haze_col=(190, 210, 210))
        ridge(img, 0.55, 0.05, (90, 130, 70), seed + 1, texture=0.12)
        trees(img, 0.58, 30, (45, 75, 45), seed, 0.14)
        yy, xx = np.mgrid[0:H, 0:W]
        path = (yy > 0.6 * H) & (np.abs(xx - 600 - (yy - 0.6 * H) * 0.9 * np.sin(yy / 90)) < (yy - 0.6 * H) * 0.35 + 6)
        img[path] = (175, 150, 110)
    elif kind == 'steamer':
        img = sky(W, H, (110, 150, 200), (170, 195, 215), (200, 215, 225))
        ridge(img, 0.42, 0.12, (90, 105, 115), seed, haze=0.4, haze_col=(170, 190, 205))
        ridge(img, 0.56, 0.04, (70, 100, 70), seed + 1)
        water(img, 0.60, (40, 75, 105))
        img[int(0.62 * H):int(0.68 * H), 500:760] = (230, 230, 225)
        img[int(0.58 * H):int(0.62 * H), 560:700] = (200, 60, 50)
        img[int(0.52 * H):int(0.58 * H), 610:630] = (30, 30, 30)
    elif kind == 'rydal':
        img = sky(W, H, (150, 175, 200), (215, 210, 195), (230, 220, 200))
        ridge(img, 0.40, 0.12, (120, 130, 140), seed, haze=0.55, haze_col=(215, 210, 200))
        ridge(img, 0.56, 0.05, (75, 95, 70), seed + 1, haze=0.2, haze_col=(200, 200, 190))
        trees(img, 0.60, 18, (50, 70, 50), seed, 0.11)
        water(img, 0.63, (90, 105, 115))
    elif kind == 'screes':
        img = sky(W, H, (90, 110, 140), (150, 160, 170), (180, 185, 190))
        ridge(img, 0.25, 0.15, (110, 100, 95), seed, texture=0.18)
        ridge(img, 0.55, 0.05, (80, 90, 80), seed + 1)
        water(img, 0.62, (30, 45, 60))
    elif kind == 'evening':
        img = sky(W, H, (70, 80, 140), (200, 140, 160), (240, 190, 170))
        ridge(img, 0.5, 0.1, (90, 80, 110), seed, haze=0.4, haze_col=(200, 150, 160))
        trees(img, 0.62, 25, (35, 30, 40), seed, 0.12)
        water(img, 0.66, (60, 60, 90))
    elif kind == 'terrace':
        img = sky(W, H, (100, 160, 210), (160, 200, 225), (205, 225, 235))
        ridge(img, 0.38, 0.1, (100, 130, 110), seed, haze=0.3, haze_col=(170, 200, 215))
        ridge(img, 0.48, 0.06, (80, 120, 70), seed + 1, texture=0.1)
        water(img, 0.58, (50, 90, 110))
        ridge(img, 0.80, 0.03, (90, 110, 60), seed + 2, texture=0.15)
    if kind in ('pines', 'rydal'):            # portrait: the middle of the frame
        img = img[:, 300:300 + 534]
    finish(img, p)


SCENES = [
    ('Catbells ridge, dawn', 'dawn'), ('Blencathra glow', 'glow'), ('Ashness mist', 'mist'),
    ('Derwentwater jetty', 'lake'), ('Castlerigg stones', 'stones'), ('Buttermere pines', 'pines'),
    ('Night over Keswick', 'night'), ('Honister sunset', 'sunset'), ('Tarn Hows path', 'path'),
    ('Ullswater steamer', 'steamer'), ('Rydal Water', 'rydal'), ('Wastwater screes', 'screes'),
    ('Grasmere evening', 'evening'), ('Loughrigg terrace', 'terrace'), ('Friars Crag', 'lake'),
]

if __name__ == '__main__':
    out = sys.argv[1]
    os.makedirs(out, exist_ok=True)
    for i, (name, kind) in enumerate(SCENES):
        scene(name, kind, 100 + i * 3, out)
    print(f'{len(SCENES)} photos in {out}')
