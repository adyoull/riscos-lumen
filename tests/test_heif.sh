#!/bin/bash
# Copyright (c) 2026 Andrew Youll
# Released under the MIT licence: see the LICENSE file.
# SPDX-License-Identifier: MIT
# The LumenHEIF helper on the HEIF photos in tests/heif (made by
# make-samples.py): tests/test_heif.sh HELPER WRAPPER WORKDIR [REFDIR]
# WRAPPER: how to run it, e.g. under QEMU with the alignment trap; "" for
# none. REFDIR: another run's WORKDIR (the PC's): the pictures must be the
# same (HEVC decoding is exact; Display P3's conversion to within 1 level).
set -u
helper=$1; wrap=$2; work=$3; ref=${4:-}
here=$(cd "$(dirname "$0")" && pwd)
checks=0; fails=0
check () { checks=$((checks + 1)); if ! eval "$1"; then fails=$((fails + 1)); echo "FAIL: $2"; fi; }
mkdir -p "$work"
dims () { head -c 20 "$1" | tr '\n' ' ' | awk '{print $2"x"$3}'; }
for n in plain thumb p3 turned deep late; do
  f=$here/heif/$n.heic; b=$work/$n
  rm -f "$b".*
  out=$(eval $wrap "$helper" decode "\"$f\"" "\"$b.ppm\"" "\"$b.info\"" 0 2>&1); rc=$?
  check "[ $rc -eq 0 ] && [ -z \"\$out\" ]" "$n: decode exits 0 and quietly ($rc: $out)"
  check "[ \"\$(head -c 2 \"$b.ppm\")\" = P6 ]" "$n: a PPM"
  check "[ ! -e \"$b.ppm_t\" ] && [ ! -e \"$b.info_t\" ]" "$n: no temporary files left"
  out=$(eval $wrap "$helper" thumb "\"$f\"" "\"$b.thumb\"" "\"$b.tinfo\"" 2>&1); rc=$?
  check "[ $rc -eq 0 ] && [ -z \"\$out\" ]" "$n: thumb exits 0 and quietly ($rc: $out)"
  if [ -n "$ref" ]; then
    check "python3 '$here/heif/compare.py' '$b.ppm' '$ref/$n.ppm' $( [ $n = p3 ] && echo 1 || echo 0 )" "$n: the same picture as on the PC"
    check "cmp -s '$b.info' '$ref/$n.info'" "$n: the same details as on the PC"
  fi
done
check "[ \$(dims $work/plain.ppm) = 1200x800 ] && grep -q '^width=1200' $work/plain.info && grep -q '^height=800' $work/plain.info" "plain: 1200 x 800"
check "grep -q '^make=TestCam' $work/plain.info && grep -q '^model=H1' $work/plain.info && grep -q '^iso=200' $work/plain.info && grep -q '^exposure=0.004' $work/plain.info && grep -q '^fnumber=2.8' $work/plain.info && grep -q '^focal=26' $work/plain.info && grep -q '^taken=2026:11:04 12:34:56' $work/plain.info" "plain: the camera, exposure and date from its EXIF"
check "[ \$(dims $work/plain.thumb) = 512x341 ]" "plain: no thumbnail of its own: made from the photo, 512 wide ($(dims $work/plain.thumb))"
check "[ \$(dims $work/thumb.ppm) = 2400x1600 ]" "thumb: the whole picture from its 512-pixel tiles"
check "[ \$(dims $work/thumb.thumb) = 320x212 ]" "thumb: its own 320-pixel thumbnail ($(dims $work/thumb.thumb))"
check "[ \$(dims $work/turned.ppm) = 800x1200 ] && grep -q '^orientation=1' $work/turned.info && grep -q '^width=800' $work/turned.info" "turned: upright (800 x 1200), orientation 1"
check "[ \$(dims $work/deep.ppm) = 800x600 ]" "deep: 10 bits a channel, decoded to 8"
tiff_at () { python3 -c "import sys; f=open(sys.argv[1],'rb'); f.seek(int(sys.argv[2])); sys.exit(f.read(4) not in (b'II*\\0', b'MM\\0*'))" "$1" "$2"; }
for n in plain late; do
  e=$(sed -n 's/^exif=//p' "$work/$n.info")
  check "[ -n \"$e\" ] && tiff_at \"$here/heif/$n.heic\" ${e%% *}" "$n: exif= says where its EXIF (a TIFF header) is ($e)"
done
check "cmp -s $work/late.ppm $work/plain.ppm" "late: meta after the picture data (as phones write it): the same picture"
check "python3 '$here/heif/compare.py' --p3 '$work/p3.ppm'" "p3: Display P3 red, green and blue converted to sRGB (outside sRGB: clipped)"
out=$(eval $wrap "$helper" decode "\"$here/heif/plain.heic\"" "\"$work/small.ppm\"" "\"$work/small.info\"" 600 2>&1)
check "[ \$(dims $work/small.ppm) = 600x400 ] && grep -q '^width=1200' $work/small.info" "decode 600: made 600 x 400, the details the photo's own"
echo "not a HEIF photo" > "$work/plain.txt"
out=$(eval $wrap "$helper" decode "\"$work/plain.txt\"" "\"$work/plain2.ppm\"" "\"$work/plain2.info\"" 0 2>&1)
check "grep -q '^error=' \"$work/plain2.info\" && [ ! -e \"$work/plain2.ppm\" ] && [ -z \"\$out\" ]" "a text file: error= in info, no picture, quiet"
head -c 3000 "$here/heif/plain.heic" > "$work/cut.heic"
out=$(eval $wrap "$helper" decode "\"$work/cut.heic\"" "\"$work/cut.ppm\"" "\"$work/cut.info\"" 0 2>&1); rc=$?
check "grep -q '^error=' \"$work/cut.info\" && [ ! -e \"$work/cut.ppm\" ] && [ $rc -ne 0 ] && [ $rc -lt 128 ]" "a cut-short file: error= in info, no picture, no crash ($rc)"
echo "test_heif ($(basename "$helper")${LUMEN_NEON:+, LUMEN_NEON=$LUMEN_NEON}): $checks checks, $fails failed"
[ $fails -eq 0 ]
