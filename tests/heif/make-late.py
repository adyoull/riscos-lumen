#!/usr/bin/env python3
# Copyright (c) 2026 Andrew Youll
# Released under the MIT licence: see the LICENSE file.
# SPDX-License-Identifier: MIT
# late.heic: plain.heic with its meta box moved after the picture data, as
# phones (Samsung's, for one) write it: ftyp, mdat, meta. The iloc offsets
# are moved to match. (make-samples.py runs this after plain.heic.)
import os, struct, sys

here = os.path.dirname(os.path.abspath(__file__))
d = open(os.path.join(here, 'plain.heic'), 'rb').read()

def top(buf):
    pos, out = 0, []
    while pos + 8 <= len(buf):
        size, kind = struct.unpack('>I4s', buf[pos:pos + 8])
        if size < 8:
            sys.exit('late: unexpected box size')
        out.append((kind, buf[pos:pos + size]))
        pos += size
    return out

def be(b, n):
    return int.from_bytes(b, 'big') if n else 0

def shift_iloc(meta, delta):
    meta = bytearray(meta)
    pos = 12                                   # meta: header + version/flags
    while pos + 8 <= len(meta):
        size, kind = struct.unpack('>I4s', meta[pos:pos + 8])
        if kind == b'iloc':
            b = pos + 8
            v = meta[b]
            osz, lsz, bsz = meta[b + 4] >> 4, meta[b + 4] & 15, meta[b + 5] >> 4
            isz = meta[b + 5] & 15 if v >= 1 else 0
            q = b + 6
            count = be(meta[q:q + (2 if v < 2 else 4)], 2 if v < 2 else 4)
            q += 2 if v < 2 else 4
            for _ in range(count):
                q += 2 if v < 2 else 4
                method = 0
                if v >= 1:
                    method = be(meta[q:q + 2], 2) & 15
                    q += 2
                q += 2
                bq = q
                base = be(meta[q:q + bsz], bsz)
                q += bsz
                ext = be(meta[q:q + 2], 2)
                q += 2
                if method == 0 and bsz:
                    meta[bq:bq + bsz] = (base + delta).to_bytes(bsz, 'big')
                for _ in range(ext):
                    q += isz
                    if method == 0 and not bsz and osz:
                        off = be(meta[q:q + osz], osz)
                        meta[q:q + osz] = (off + delta).to_bytes(osz, 'big')
                    q += osz + lsz
            return bytes(meta)
        pos += size
    sys.exit('late: no iloc')

boxes = top(d)
kinds = [k for k, _ in boxes]
if kinds[:3] != [b'ftyp', b'meta', b'mdat']:
    sys.exit('late: plain.heic is not ftyp, meta, mdat: %r' % kinds)
ftyp, meta, mdat = boxes[0][1], boxes[1][1], boxes[2][1]
rest = b''.join(b for _, b in boxes[3:])
meta = shift_iloc(meta, -len(meta))            # the data comes len(meta) sooner
open(os.path.join(here, 'late.heic'), 'wb').write(ftyp + mdat + rest + meta)
