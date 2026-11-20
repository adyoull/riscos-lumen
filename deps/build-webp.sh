#!/bin/bash
# Copyright (c) 2026 Andrew Youll
# Released under the MIT licence: see the LICENSE file.
# SPDX-License-Identifier: MIT
# Builds libwebp 1.6.0 (BSD 3-clause) for the LumenWebP plug-in, as one
# static library (decoder, demux and the DSP code), for three targets:
#   riscos   - arm-riscos-gnueabihf (the plug-in)
#   host     - x86-64 Linux (fast tests; plain C)
#   linuxarm - arm-linux-gnueabihf (tests under QEMU with RISC OS's alignment rules)
#
# NEON is chosen at run time, as for Lumen's other decoders: every file is
# built with WEBP_HAVE_NEON and WEBP_DSP_OMIT_C_CODE=0, the plain files for
# VFPv3 only and the *_neon.c files with NEON. libwebp then keeps its C code
# and calls the NEON versions only when VP8GetCPUInfo(kNEON) says so;
# LumenWebP sets VP8GetCPUInfo from VFPSupport_Features (or Lumen$NEON).
# The files are compiled directly (no autotools) so each can have its own
# flags.
# Source: /build/src/libwebp-1.6.0 (git tag v1.6.0).
# Output: /build/lumen-deps/<target>/lib/libwebp-lumen.a and
#         /build/lumen-deps/<target>/include/webp/*.h
set -e
RCD=/build/riscos-crossdev
SRC=/build/src/libwebp-1.6.0
OUT=/build/lumen-deps
JOBS=${JOBS:-$(nproc)}

# Decoding only: the decoder, the container parser (for EXIF and XMP), the
# utilities and the DSP code the decoder uses.
FILES="$(ls $SRC/src/dec/*.c) $SRC/src/demux/demux.c
  $(ls $SRC/src/utils/*.c | grep -v -e _enc -e bit_writer -e quant_levels.c)
  $(for f in alpha_processing cpu dec dec_clip_tables filters lossless rescaler upsampling yuv; do
      echo $SRC/src/dsp/$f.c; done)
  $(for f in alpha_processing dec filters lossless rescaler upsampling yuv; do
      echo $SRC/src/dsp/${f}_neon.c; done)"

for t in ${TARGETS:-riscos host linuxarm}; do
  P=$OUT/$t
  case $t in
    riscos)   CC=$RCD/toolchain/env/bin/arm-riscos-gnueabihf-gcc; AR=$RCD/toolchain/env/bin/arm-riscos-gnueabihf-ar
              NEON="-mfpu=neon-vfpv3"; PLAIN="-mfpu=vfpv3"
              CF="-mfloat-abi=hard -mtune=cortex-a72 -mno-unaligned-access -fstack-clash-protection" ;;
    linuxarm) CC=arm-linux-gnueabihf-gcc; AR=arm-linux-gnueabihf-ar
              NEON="-mfpu=neon-vfpv3"; PLAIN="-mfpu=vfpv3"
              CF="-mfloat-abi=hard -mcpu=cortex-a8 -marm -mno-unaligned-access" ;;
    host)     CC=gcc; AR=ar; NEON=; PLAIN=; CF="-U__SSE2__ -U__SSE4_1__ -U__AVX2__" ;;   # plain C
  esac
  B=/tmp/webp-$t; rm -rf $B; mkdir -p $B
  DEF="-DWEBP_DSP_OMIT_C_CODE=0 -DNDEBUG"
  [ $t = host ] || DEF="$DEF -DWEBP_HAVE_NEON"
  for f in $FILES; do
    o=$B/$(basename $(dirname $f))_$(basename $f .c).o
    case $f in
      *_neon.c) [ $t = host ] && continue; FPU=$NEON ;;
      *)        FPU=$PLAIN ;;
    esac
    echo "$CC -O2 $FPU $CF $DEF -I$SRC -c $f -o $o"
  done | xargs -P $JOBS -I{} sh -c '{}'
  mkdir -p $P/lib $P/include/webp
  rm -f $P/lib/libwebp-lumen.a
  $AR rcs $P/lib/libwebp-lumen.a $B/*.o
  cp $SRC/src/webp/decode.h $SRC/src/webp/demux.h $SRC/src/webp/mux_types.h $SRC/src/webp/types.h $P/include/webp/
  echo "libwebp 1.6.0 for $t: $(ls $B/*.o | wc -l) files"
done
