#!/usr/bin/env python3
# Copyright (c) 2026 Andrew Youll
# Released under the MIT licence: see the LICENSE file.
# SPDX-License-Identifier: MIT
# Makes the WebP test pictures in this directory (with Pillow's libwebp):
# synthetic pictures, so they can be kept here. refs.sha256 has the SHA-256
# of Pillow's own decoding of each (RGB laid on white, a binary PPM): the
# plug-in's must be the same.
#   plain.webp     1200 x 800 lossy, EXIF (camera, date, exposure, GPS)
#   odd.webp       1201 x 799 lossy (odd sizes: rows not a multiple of anything)
#   lossless.webp  640 x 480 lossless
#   alpha.webp     640 x 480 lossy with transparency (a fading circle)
#   alphall.webp   640 x 480 lossless with transparency
#   turned.webp    1200 x 800 lossy, EXIF orientation 6
#   anim.webp      400 x 300, 3 frames (Pillow's own reference: the first)
#   exifhdr.webp   plain.webp's picture, its EXIF chunk starting "Exif\0\0"
import hashlib, os, struct
import numpy as np
from PIL import Image
from PIL.TiffImagePlugin import IFDRational

here = os.path.dirname(os.path.abspath(__file__))

def picture(w, h, seed=1):
    y, x = np.mgrid[0:h, 0:w].astype(np.float32)
    r = 128 + 100 * np.sin(x / 37.0) * np.cos(y / 53.0)
    g = 255 * x / w
    b = 255 * y / h
    rng = np.random.default_rng(seed)
    a = np.stack([r, g, b], -1) + rng.normal(0, 1.5, (h, w, 3))
    a[(x.astype(int) // 40 + y.astype(int) // 40) % 2 == 0] *= 0.85
    return Image.fromarray(np.clip(a, 0, 255).astype(np.uint8), 'RGB')

def with_alpha(im):
    w, h = im.size
    y, x = np.mgrid[0:h, 0:w].astype(np.float32)
    d = np.hypot(x - w / 2, y - h / 2) / (min(w, h) / 2)
    a = np.clip(255 * (1.2 - d), 0, 255).astype(np.uint8)
    im = im.convert('RGBA')
    im.putalpha(Image.fromarray(a, 'L'))
    return im

def exif(orientation=1):
    e = Image.Exif()
    e[0x010F] = 'TestCam'
    e[0x0110] = 'W1'
    e[0x0112] = orientation
    ex = e.get_ifd(0x8769)
    ex[0x9003] = '2026:11:20 09:15:00'
    ex[0x829A] = IFDRational(1, 500)
    ex[0x829D] = IFDRational(4, 1)
    ex[0x8827] = 100
    ex[0x920A] = IFDRational(35, 1)
    gps = e.get_ifd(0x8825)
    gps[1] = 'N'; gps[2] = (IFDRational(54, 1), IFDRational(36, 1), IFDRational(0, 1))
    gps[3] = 'W'; gps[4] = (IFDRational(3, 1), IFDRational(8, 1), IFDRational(30, 1))
    return e.tobytes()

refs = []

def ref(name, im):
    im = im.convert('RGBA')
    bg = Image.new('RGBA', im.size, (255, 255, 255, 255))
    rgb = Image.alpha_composite(bg, im).convert('RGB')
    ppm = b'P6\n%d %d\n255\n' % rgb.size + rgb.tobytes()
    refs.append('%s  %s\n' % (hashlib.sha256(ppm).hexdigest(), name))

def save(name, im, **kw):
    p = os.path.join(here, name + '.webp')
    im.save(p, 'WEBP', **kw)
    ref(name, Image.open(p))

save('plain', picture(1200, 800), quality=85, exif=exif())
save('odd', picture(1201, 799, 2), quality=80)
save('lossless', picture(640, 480, 3), lossless=True)
save('alpha', with_alpha(picture(640, 480, 4)), quality=85)
save('alphall', with_alpha(picture(640, 480, 5)), lossless=True)
save('turned', picture(1200, 800, 6), quality=85, exif=exif(6))
frames = [picture(400, 300, 10 + i) for i in range(3)]
p = os.path.join(here, 'anim.webp')
frames[0].save(p, 'WEBP', save_all=True, append_images=frames[1:], duration=200, loop=0, quality=85)
ref('anim', Image.open(p))      # Pillow opens it at its first frame

# exifhdr: plain.webp with "Exif\0\0" in front of its EXIF chunk's data.
d = open(os.path.join(here, 'plain.webp'), 'rb').read()
out, pos = bytearray(d[:12]), 12
while pos + 8 <= len(d):
    name, size = d[pos:pos + 4], struct.unpack('<I', d[pos + 4:pos + 8])[0]
    body = d[pos + 8:pos + 8 + size]
    if name == b'EXIF' and not body.startswith(b'Exif\0\0'):
        body = b'Exif\0\0' + body
    out += name + struct.pack('<I', len(body)) + body + (b'\0' if len(body) & 1 else b'')
    pos += 8 + size + (size & 1)
out[4:8] = struct.pack('<I', len(out) - 8)
open(os.path.join(here, 'exifhdr.webp'), 'wb').write(out)
ref('exifhdr', Image.open(os.path.join(here, 'exifhdr.webp')))
open(os.path.join(here, 'refs.sha256'), 'w').write(''.join(refs))
