#!/bin/bash
# Copyright (c) 2026 Andrew Youll
# Released under the MIT licence: see the LICENSE file.
# SPDX-License-Identifier: MIT
# The LumenRaw helper on real raw files: tests/test_plugin.sh HELPER WRAPPER WORKDIR
# (WRAPPER: how to run it, e.g. under QEMU with the alignment trap; "" for
# none). The raw files come from $RAW_SAMPLES (not kept in the repository:
# their licences vary); with none there, it says so and passes.
set -u
helper=$1; wrap=$2; work=$3
checks=0; fails=0
check () { checks=$((checks + 1)); if ! eval "$1"; then fails=$((fails + 1)); echo "FAIL: $2"; fi; }
mkdir -p "$work"
shopt -s nullglob
samples=("${RAW_SAMPLES:-/nonexistent}"/*)
if [ ${#samples[@]} -eq 0 ]; then
  echo "test_plugin: no raw files in RAW_SAMPLES, nothing tested"
  exit 0
fi
for f in "${samples[@]}"; do
  b=$work/$(basename "$f")
  rm -f "$b".*
  out=$(eval $wrap "$helper" thumb "\"$f\"" "\"$b.thumb\"" "\"$b.tinfo\"" 2>&1); rc=$?
  check "[ $rc -eq 0 ]" "$f: thumb exits 0 ($rc)"
  check "[ -z \"\$out\" ]" "$f: thumb says nothing (output would open a task window): $out"
  check "head -c 2 \"$b.thumb\" | od -An -tx1 | grep -q 'ff d8' || head -c 2 \"$b.thumb\" | grep -q P6" "$f: the preview is a JPEG or PPM"
  check "grep -q '^width=[1-9]' \"$b.tinfo\" && grep -q '^make=.' \"$b.tinfo\"" "$f: info has the size and camera"
  check "[ ! -e \"$b.thumb_t\" ] && [ ! -e \"$b.tinfo_t\" ]" "$f: no temporary files left"
  out=$(eval $wrap "$helper" decode "\"$f\"" "\"$b.ppm\"" "\"$b.info\"" 1000 2>&1); rc=$?
  check "[ $rc -eq 0 ] && [ -z \"\$out\" ]" "$f: decode exits 0 and quietly ($rc: $out)"
  w=$(sed -n 's/^width=//p' "$b.info"); h=$(sed -n 's/^height=//p' "$b.info")
  read -r magic dims < <(head -c 20 "$b.ppm" | tr '\n' ' ' | awk '{print $1, $2"x"$3}')
  check "[ \"$magic\" = P6 ]" "$f: decode writes a PPM"
  pw=${dims%x*}; ph=${dims#*x}
  # Half size when that is still >= 1000 on the longer side (within a few pixels: LibRaw trims odd edges).
  check "[ $(( (w > h ? w : h) / 2 >= 1000 )) -eq 0 ] || [ \$(( ($pw > $ph ? $pw : $ph) - (w > h ? w : h) / 2 )) -le 4 ]" "$f: half size ($dims from ${w}x$h)"
  # With the noise reduction argument (API: a 6th, 1 or 2): the same size, a different picture.
  out=$(eval $wrap "$helper" decode "\"$f\"" "\"$b.nr.ppm\"" "\"$b.nr.info\"" 1000 2 2>&1); rc=$?
  check "[ $rc -eq 0 ] && [ -z \"\$out\" ] && [ \"\$(head -c 20 \"$b.nr.ppm\" | tr '\\n' ' ' | awk '{print \$2\"x\"\$3}')\" = \"$dims\" ] && ! cmp -s \"$b.ppm\" \"$b.nr.ppm\"" "$f: decode with noise reduction (2): same size, a different picture"
done
# Not a raw file: an error line, no picture, nothing printed.
echo "not a raw file" > "$work/plain.txt"
out=$(eval $wrap "$helper" decode "\"$work/plain.txt\"" "\"$work/plain.ppm\"" "\"$work/plain.info\"" 1000 2>&1)
check "grep -q '^error=' \"$work/plain.info\" && [ ! -e \"$work/plain.ppm\" ] && [ -z \"\$out\" ]" "a text file: error= in info, no picture, quiet"
echo "test_plugin ($(basename "$helper")): $checks checks, $fails failed"
[ $fails -eq 0 ]
