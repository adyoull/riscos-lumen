#!/usr/bin/env python3
# Copyright (c) 2026 Andrew Youll
# Released under the MIT licence: see the LICENSE file.
# SPDX-License-Identifier: MIT
"""lensdb: the lensfun project's lens database, as LumenLens's Lenses file.

    tools/lensdb.py LENSFUN_DB_DIR OUT [SOURCE_NOTE]

The database (data/db/*.xml in lensfun) is by the lensfun contributors,
under the Creative Commons Attribution-ShareAlike 3.0 licence; this only
puts the same facts in a smaller form that Lumen reads a line at a time,
so the result is under that licence too (plugins/lens/Licence). Nothing
of lensfun's library (LGPL) is used, by this or by Lumen.

The form, a record a line, fields split by "|" (names) or spaces (numbers):

  LumenLens 1                          the first line
  # ...                                comments: where it came from
  M name|compatible|compatible...      a mount, and the mounts its lenses fit
  C maker~maker|model~model|mount|crop a camera: names (each spelling, ~
                                       between), its mount, crop factor
  L maker~...|model~...|mount;mount|crop|aspect|type
                                       a lens: the crop factor and aspect
                                       ratio (long:short as a number) of
                                       the camera it was measured on, and
                                       its type (r rectilinear, f fisheye,
                                       e equisolid, s stereographic,
                                       o orthographic, q equirectangular,
                                       t thoby, p panoramic)
  D focal model a b c                  distortion at a focal length:
                                       p ptlens (a b c), 3 poly3 (k1),
                                       5 poly5 (k1 k2)
  T focal model ...                    lateral colour (fringing):
                                       l linear (kr kb), 3 poly3
                                       (vr cr br vb cb bb)
  V focal aperture k1 k2 k3            vignetting (the pa model), at the
                                       longest focus distance measured

D, T and V lines belong to the L line before them, sorted by focal length
(and aperture). Lenses with nothing measured are left out.
"""
import glob
import os
import sys
import unicodedata
import xml.etree.ElementTree as ET

TYPES = {"rectilinear": "r", "fisheye": "f", "equisolid": "e", "stereographic": "s",
         "orthographic": "o", "equirectangular": "q", "fisheye_thoby": "t", "panoramic": "p"}


def num(v, default=0.0):
    try:
        return float(v)
    except (TypeError, ValueError):
        return default


def fmt(v):
    s = "%.7g" % v
    return "0" if s in ("-0", "0") else s


def latin1(t):
    """RISC OS's character set (Latin-1): what isn't in it, as near as ASCII goes."""
    out = []
    for ch in t:
        try:
            ch.encode("latin-1")
            out.append(ch)
        except UnicodeEncodeError:
            out.append(unicodedata.normalize("NFKD", ch).encode("ascii", "ignore").decode() or "?")
    return "".join(out)


def names(el, tag):
    """Every spelling of a name: the plain one first, then translations."""
    out = []
    for n in el.findall(tag):
        t = latin1(" ".join((n.text or "").split()))
        if t and t not in out and "|" not in t and "~" not in t:
            out.append(t)
    return out


def aspect(el):
    a = el.findtext("aspect-ratio")
    if not a:
        return 1.5
    a = a.strip()
    if ":" in a:
        x, y = a.split(":", 1)
        x, y = num(x, 3), num(y, 2)
        if x > 0 and y > 0:
            return max(x, y) / min(x, y)
        return 1.5
    v = num(a, 1.5)
    return v if v >= 1 else 1 / v if v > 0 else 1.5


def lens_records(lens):
    dist, tca, vig = {}, {}, {}
    for cal in lens.findall("calibration"):
        for d in cal.findall("distortion"):
            f, m = num(d.get("focal")), d.get("model")
            if f <= 0:
                continue
            if m == "ptlens":
                dist[f] = "p %s %s %s" % (fmt(num(d.get("a"))), fmt(num(d.get("b"))), fmt(num(d.get("c"))))
            elif m == "poly3":
                dist[f] = "3 %s" % fmt(num(d.get("k1")))
            elif m == "poly5":
                dist[f] = "5 %s %s" % (fmt(num(d.get("k1"))), fmt(num(d.get("k2"))))
        for t in cal.findall("tca"):
            f, m = num(t.get("focal")), t.get("model")
            if f <= 0:
                continue
            if m == "linear":
                tca[f] = "l %s %s" % (fmt(num(t.get("kr"), 1)), fmt(num(t.get("kb"), 1)))
            elif m == "poly3":
                tca[f] = "3 " + " ".join(fmt(num(t.get(k), dv)) for k, dv in
                                         (("vr", 1), ("cr", 0), ("br", 0), ("vb", 1), ("cb", 0), ("bb", 0)))
        for v in cal.findall("vignetting"):
            f, a, d = num(v.get("focal")), num(v.get("aperture")), num(v.get("distance"), 1000)
            if f <= 0 or a <= 0 or v.get("model") != "pa":
                continue
            key = (f, a)
            if key not in vig or d >= vig[key][0]:
                vig[key] = (d, " ".join(fmt(num(v.get(k))) for k in ("k1", "k2", "k3")))
    out = []
    for f in sorted(dist):
        out.append("D %s %s" % (fmt(f), dist[f]))
    for f in sorted(tca):
        out.append("T %s %s" % (fmt(f), tca[f]))
    for f, a in sorted(vig):
        out.append("V %s %s %s" % (fmt(f), fmt(a), vig[(f, a)][1]))
    return out


def main():
    if len(sys.argv) < 3:
        sys.exit(__doc__)
    src, outpath = sys.argv[1], sys.argv[2]
    note = sys.argv[3] if len(sys.argv) > 3 else ""
    mounts, cameras, lenses = {}, [], []
    for path in sorted(glob.glob(os.path.join(src, "*.xml"))):
        root = ET.parse(path).getroot()
        for m in root.findall("mount"):
            n = latin1((m.findtext("name") or "").strip())
            if n:
                mounts.setdefault(n, [])
                for c in m.findall("compat"):
                    c = latin1((c.text or "").strip())
                    if c and c not in mounts[n]:
                        mounts[n].append(c)
        for c in root.findall("camera"):
            mk, md = names(c, "maker"), names(c, "model")
            mt = latin1((c.findtext("mount") or "").strip())
            if mk and md and mt:
                cameras.append("C %s|%s|%s|%s" % ("~".join(mk), "~".join(md), mt,
                                                  fmt(num(c.findtext("cropfactor"), 1))))
        for l in root.findall("lens"):
            mk, md = names(l, "maker"), names(l, "model")
            mts = [latin1((m.text or "").strip()) for m in l.findall("mount") if (m.text or "").strip()]
            recs = lens_records(l)
            if not mk or not md or not mts or not recs:
                continue
            ty = TYPES.get((l.findtext("type") or "rectilinear").strip(), "r")
            lenses.append(("L %s|%s|%s|%s|%s|%s" % ("~".join(mk), "~".join(md), ";".join(mts),
                                                    fmt(num(l.findtext("cropfactor"), 1)),
                                                    fmt(aspect(l)), ty), recs))
    with open(outpath, "w", newline="\n", encoding="latin-1") as f:
        f.write("LumenLens 1\n")
        f.write("# Lens corrections from the lensfun database (https://lensfun.github.io),\n")
        f.write("# by the lensfun contributors, under the Creative Commons\n")
        f.write("# Attribution-ShareAlike 3.0 licence (see Licence). Converted by\n")
        f.write("# Lumen's tools/lensdb.py: the same facts, in a smaller form.\n")
        if note:
            f.write("# %s\n" % note)
        f.write("# %d lenses, %d cameras, %d mounts\n" % (len(lenses), len(cameras), len(mounts)))
        for n in sorted(mounts):
            f.write("M %s\n" % "|".join([n] + mounts[n]))
        for c in cameras:
            f.write(c + "\n")
        for l, recs in lenses:
            f.write(l + "\n")
            for r in recs:
                f.write(r + "\n")


if __name__ == "__main__":
    main()
