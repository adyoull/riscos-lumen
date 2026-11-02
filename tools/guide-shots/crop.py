#!/usr/bin/env python3
# crop.py SHOT NAME x0 y0 x1 y1 [WIDTH] -> /build/lumen/docs/guide/images/NAME.png (scaled down to WIDTH if given)
import sys
from PIL import Image
s, n = sys.argv[1], sys.argv[2]
x0, y0, x1, y1 = (int(v) for v in sys.argv[3:7])
im = Image.open(s).convert('RGB').crop((x0, y0, x1, y1))
if len(sys.argv) > 7 and im.size[0] > int(sys.argv[7]):
    w = int(sys.argv[7]); im = im.resize((w, round(im.size[1] * w / im.size[0])), Image.LANCZOS)
im.save('/build/lumen/docs/guide/images/%s.png' % n, optimize=True)
print(n, im.size)
