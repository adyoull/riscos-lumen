#!/usr/bin/env python3
# Copyright (c) 2026 Andrew Youll
# Released under the MIT licence: see the LICENSE file.
# SPDX-License-Identifier: MIT
# Makes the HEIF test photos in this directory (needs pillow-heif, which has
# libheif with x265): synthetic pictures, so they can be kept here.
#   plain.heic    1200 x 800, EXIF (camera, date, exposure), no thumbnail
#   thumb.heic    2400 x 1600 in 512-pixel tiles (a grid, as iPhones save) with a 320-pixel thumbnail
#   p3.heic       Display P3 (nclx primaries 12), saturated colours
#   turned.heic   stored 1200 x 800, EXIF orientation 6 (libheif's irot: shown 800 x 1200)
#   deep.heic     10 bits a channel
#   late.heic     plain.heic with its meta box after the picture data (make-late.py)
import io, math, os
import numpy as np
from PIL import Image
from PIL.TiffImagePlugin import IFDRational
import pillow_heif

here = os.path.dirname(os.path.abspath(__file__))

def picture(w, h, seed=1):
    y, x = np.mgrid[0:h, 0:w].astype(np.float32)
    r = 128 + 100 * np.sin(x / 37.0) * np.cos(y / 53.0)
    g = 255 * x / w
    b = 255 * y / h
    rng = np.random.default_rng(seed)
    noise = rng.normal(0, 1.5, (h, w, 3))
    a = np.stack([r, g, b], -1) + noise
    # a few hard edges and fine detail
    a[(x.astype(int) // 40 + y.astype(int) // 40) % 2 == 0] *= 0.85
    return Image.fromarray(np.clip(a, 0, 255).astype(np.uint8), 'RGB')

def exif(orientation=1):
    e = Image.Exif()
    e[0x010F] = 'TestCam'          # Make
    e[0x0110] = 'H1'               # Model
    e[0x0112] = orientation
    ex = e.get_ifd(0x8769)
    ex[0x9003] = '2026:11:04 12:34:56'   # DateTimeOriginal
    ex[0x829A] = IFDRational(1, 250)               # ExposureTime
    ex[0x829D] = IFDRational(28, 10)               # FNumber
    ex[0x8827] = 200                    # ISO
    ex[0x920A] = IFDRational(26, 1)                # FocalLength
    return e.tobytes()

def save(img, name, **kw):
    img.save(os.path.join(here, name), format='HEIF', quality=80, **kw)

pillow_heif.register_heif_opener()
save(picture(1200, 800), 'plain.heic', exif=exif())
hf = pillow_heif.from_pillow(picture(2400, 1600, 2))
hf[0].info['thumbnails'] = [320]
hf.save(os.path.join(here, 'thumb.heic'), quality=80, tile_size=512)
# P3: saturated primaries, tagged with nclx primaries 12
p = np.zeros((400, 600, 3), np.uint8)
p[:, 0:200] = (255, 0, 0); p[:, 200:400] = (0, 255, 0); p[:, 400:600] = (0, 0, 255)
p[200:] = (p[200:] * 0.5).astype(np.uint8) + 60
hp = pillow_heif.from_pillow(Image.fromarray(p, 'RGB'))
hp.save(os.path.join(here, 'p3.heic'), quality=90, save_nclx_profile=True,
        nclx_profile={'color_primaries': 12, 'transfer_characteristics': 13,
                      'matrix_coefficients': 6, 'full_range_flag': 1})
ht = pillow_heif.from_pillow(picture(1200, 800, 3))
ht.info['exif'] = exif(6)
ht.save(os.path.join(here, 'turned.heic'), quality=80)
deep = (np.asarray(picture(800, 600, 4)).astype(np.uint16) * 257)
hd = pillow_heif.from_bytes(mode='RGB;16', size=(800, 600), data=deep.tobytes())
hd.save(os.path.join(here, 'deep.heic'), quality=80)
for n in sorted(os.listdir(here)):
    if n.endswith('.heic'):
        print(n, os.path.getsize(os.path.join(here, n)))

# Then late.heic, from plain.heic.
import runpy
runpy.run_path(os.path.join(here, 'make-late.py'))
