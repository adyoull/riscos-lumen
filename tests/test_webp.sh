#!/bin/bash
# Copyright (c) 2026 Andrew Youll
# Released under the MIT licence: see the LICENSE file.
# SPDX-License-Identifier: MIT
# The LumenWebP helper on the WebP pictures in tests/webp (made by
# make-samples.py): tests/test_webp.sh HELPER WRAPPER WORKDIR [REFDIR]
# WRAPPER: how to run it, e.g. under QEMU with the alignment trap; "" for
# none. Every full-size picture must be exactly Pillow's (refs.sha256).
# REFDIR: another run's WORKDIR (the PC's): the details must be the same,
# and the scaled pictures too, to within 2 levels (libwebp's NEON scaling
# rounds a few pixels differently at the edges of odd sizes).
set -u
helper=$1; wrap=$2; work=$3; ref=${4:-}
here=$(cd "$(dirname "$0")" && pwd)
checks=0; fails=0
check () { checks=$((checks + 1)); if ! eval "$1"; then fails=$((fails + 1)); echo "FAIL: $2"; fi; }
mkdir -p "$work"
near () { python3 -c "
import sys
def rd(p):
    d = open(p, 'rb').read().split(b'\\n', 3)
    return d[1], d[3]
(sa, a), (sb, b) = rd(sys.argv[1]), rd(sys.argv[2])
sys.exit(sa != sb or len(a) != len(b) or max(abs(x - y) for x, y in zip(a, b)) > 2)" "$1" "$2"; }
dims () { head -c 20 "$1" | tr '\n' ' ' | awk '{print $2"x"$3}'; }
for n in plain odd lossless alpha alphall turned anim exifhdr; do
  f=$here/webp/$n.webp; b=$work/$n
  rm -f "$b".*
  out=$(eval $wrap "$helper" decode "\"$f\"" "\"$b.ppm\"" "\"$b.info\"" 0 2>&1); rc=$?
  check "[ $rc -eq 0 ] && [ -z \"\$out\" ]" "$n: decode exits 0 and quietly ($rc: $out)"
  check "[ ! -e \"$b.ppm_t\" ] && [ ! -e \"$b.info_t\" ]" "$n: no temporary files left"
  want=$(sed -n "s/  $n\$//p" "$here/webp/refs.sha256")
  check "[ \"\$(sha256sum < \"$b.ppm\" | cut -c1-64)\" = $want ]" "$n: exactly the picture Pillow's libwebp decodes"
  out=$(eval $wrap "$helper" thumb "\"$f\"" "\"$b.thumb\"" "\"$b.tinfo\"" 2>&1); rc=$?
  check "[ $rc -eq 0 ] && [ -z \"\$out\" ] && [ \"\$(head -c 2 \"$b.thumb\")\" = P6 ]" "$n: thumb exits 0 and quietly, a PPM ($rc: $out)"
  out=$(eval $wrap "$helper" decode "\"$f\"" "\"$b.small\"" "\"$b.sinfo\"" 300 2>&1); rc=$?
  check "[ $rc -eq 0 ] && [ -z \"\$out\" ]" "$n: decode 300 exits 0 ($rc: $out)"
  if [ -n "$ref" ]; then
    check "near '$b.thumb' '$ref/$n.thumb' && near '$b.small' '$ref/$n.small'" "$n: the scaled pictures as on the PC, to within 2 levels"
    check "cmp -s '$b.info' '$ref/$n.info' && cmp -s '$b.tinfo' '$ref/$n.tinfo'" "$n: the same details as on the PC"
  fi
done
check "[ \$(dims $work/plain.thumb) = 512x341 ] && [ \$(dims $work/plain.small) = 300x200 ]" "plain: thumb 512 x 341, decode 300 makes 300 x 200"
check "[ \$(dims $work/odd.small) = 300x200 ] && [ \$(dims $work/lossless.thumb) = 512x384 ]" "odd and lossless: scaled to fit"
check "[ \$(dims $work/anim.thumb) = 400x300 ] && [ \$(dims $work/anim.small) = 300x225 ]" "anim: smaller than a thumbnail: as it is; 300 x 225 when asked"
check "grep -q '^width=1200' $work/plain.sinfo && grep -q '^height=800' $work/plain.sinfo" "decode 300: the details give the picture's own size"
check "grep -q '^make=TestCam' $work/plain.info && grep -q '^model=W1' $work/plain.info && grep -q '^iso=100' $work/plain.info && grep -q '^exposure=0.002' $work/plain.info && grep -q '^fnumber=4' $work/plain.info && grep -q '^focal=35' $work/plain.info && grep -q '^taken=2026:11:20 09:15:00' $work/plain.info" "plain: the camera, exposure and date from its EXIF"
tiff_at () { python3 -c "import sys; f=open(sys.argv[1],'rb'); f.seek(int(sys.argv[2])); sys.exit(f.read(4) not in (b'II*\\0', b'MM\\0*'))" "$1" "$2"; }
for n in plain exifhdr; do
  e=$(sed -n 's/^exif=//p' "$work/$n.info")
  check "[ -n \"$e\" ] && tiff_at \"$here/webp/$n.webp\" ${e%% *}" "$n: exif= says where its EXIF (a TIFF header) is ($e)"
done
check "grep -q '^make=TestCam' $work/exifhdr.info" "exifhdr: EXIF data starting \"Exif\\0\\0\" read too"
check "! grep -q '^exif=' $work/lossless.info && grep -q '^make=$' $work/lossless.info" "lossless: no EXIF, none claimed"
check "grep -q '^orientation=6' $work/turned.info && grep -q '^width=1200' $work/turned.info" "turned: orientation 6 passed on (Lumen turns it)"
check "grep -qx 'encoding=WebP lossy' $work/plain.info" "plain: encoding WebP lossy"
check "grep -qx 'encoding=WebP lossless' $work/lossless.info" "lossless: encoding WebP lossless"
check "grep -qx 'encoding=WebP lossy, with transparency' $work/alpha.info" "alpha: encoding with transparency"
check "grep -qx 'encoding=WebP lossless, with transparency' $work/alphall.info" "alphall: encoding lossless with transparency"
check "grep -qx 'encoding=WebP lossy, animated (3 frames)' $work/anim.info && grep -q '^width=400' $work/anim.info" "anim: encoding animated, 3 frames, the canvas's size"
b=$work/mem; rm -f "$b".*
eval $wrap "$helper" decode "\"$here/webp/plain.webp\"" "\"$b.ppm\"" "\"$b.info\"" 0 0 0 1 >/dev/null 2>&1
check "[ \"\$(head -c 2 \"$b.ppm\")\" = P6 ] && ! grep -q '^memory=' $b.info" "MEMORY 1 where there are no dynamic areas: the file all the same"
echo "not a WebP picture" > "$work/text.webp"
out=$(eval $wrap "$helper" decode "\"$work/text.webp\"" "\"$work/text.ppm\"" "\"$work/text.info\"" 0 2>&1); rc=$?
check "grep -q '^error=' \"$work/text.info\" && [ ! -e \"$work/text.ppm\" ] && [ -z \"\$out\" ]" "a text file: error= in info, no picture, quiet"
for cut in 30 3000 15000; do
  head -c $cut "$here/webp/plain.webp" > "$work/cut.webp"; rm -f "$work/cut.ppm" "$work/cut.info"
  out=$(eval $wrap "$helper" decode "\"$work/cut.webp\"" "\"$work/cut.ppm\"" "\"$work/cut.info\"" 0 2>&1); rc=$?
  check "grep -q '^error=' \"$work/cut.info\" && [ ! -e \"$work/cut.ppm\" ] && [ $rc -ne 0 ] && [ $rc -lt 128 ]" "cut short at $cut bytes: error= in info, no picture, no crash ($rc)"
done
echo "test_webp ($(basename "$helper")${LUMEN_NEON:+, LUMEN_NEON=$LUMEN_NEON}): $checks checks, $fails failed"
[ $fails -eq 0 ]
