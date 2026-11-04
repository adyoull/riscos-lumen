#!/usr/bin/env python3
# Copyright (c) 2026 Andrew Youll
# Released under the MIT licence: see the LICENSE file.
# SPDX-License-Identifier: MIT
# Makes the AVIF test photos in this directory (Pillow's AVIF support, and
# FFmpeg with libaom for the 10-bit one): synthetic pictures, so they can
# be kept here.
#   plain.avif    1200 x 800, 8-bit 4:2:0, EXIF (camera, date, exposure)
#   turned.avif   stored 1200 x 800, EXIF orientation 6 (written as irot:
#                 shown 800 x 1200)
#   deep.avif     800 x 600, 10 bits a channel (libaom, still picture)
import os, subprocess, tempfile
from PIL import Image

here = os.path.dirname(os.path.abspath(__file__))
# make-samples.py makes the HEIC files when run; only its helpers are wanted
src = open(os.path.join(here, 'make-samples.py')).read()
helpers = src[:src.index('def save(')]
ns = {'__file__': os.path.join(here, 'make-samples.py')}
exec(compile(helpers, 'make-samples.py', 'exec'), ns)
picture, exif = ns['picture'], ns['exif']

picture(1200, 800).save(os.path.join(here, 'plain.avif'), quality=80, exif=exif())
picture(1200, 800, 3).save(os.path.join(here, 'turned.avif'), quality=80, exif=exif(6))
with tempfile.TemporaryDirectory() as t:
    png = os.path.join(t, 'deep.png')
    picture(800, 600, 4).save(png)
    subprocess.run(['ffmpeg', '-v', 'error', '-y', '-i', png, '-c:v', 'libaom-av1', '-still-picture', '1',
                    '-pix_fmt', 'yuv420p10le', '-crf', '30', '-cpu-used', '6',
                    os.path.join(here, 'deep.avif')], check=True)
for n in sorted(os.listdir(here)):
    if n.endswith('.avif'):
        print(n, os.path.getsize(os.path.join(here, n)))
