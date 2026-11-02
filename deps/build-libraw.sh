#!/bin/sh
# Builds LibRaw (0.22.3, /build/src/LibRaw) as a static library for the
# LumenRaw plug-in, for three targets:
#   riscos   - arm-riscos-gnueabihf (the plug-in)
#   host     - x86-64 Linux (fast tests)
#   linuxarm - arm-linux-gnueabihf (tests under QEMU with RISC OS's alignment rules)
# No threads (LIBRAW_NOTHREADS), no OpenMP; zlib and libjpeg for DNG's
# deflate and lossy compression (libjpeg from deps/build-deps.sh).
# Output: /build/lumen-deps/<target>/lib/libraw.a and include/libraw/.
set -e
RCD=/build/riscos-crossdev
SRC=/build/src/LibRaw
OUT=/build/lumen-deps
OBJS=$(sed -n '/^LIB_OBJECTS=/,/^$/p' $SRC/Makefile.dist | tr ' \\' '\n\n' | grep 'object/.*\.o' | sed 's|object/||; s|\.o$||')
for t in ${TARGETS:-riscos host linuxarm}; do
  P=$OUT/$t
  case $t in
    riscos)   CXX="$RCD/toolchain/env/bin/arm-riscos-gnueabihf-g++ -mfpu=vfpv3 -mfloat-abi=hard -fstack-clash-protection"
              AR=$RCD/toolchain/env/bin/arm-riscos-gnueabihf-ar; ZINC=$RCD/devkit/env/include ;;
    host)     CXX=g++; AR=ar; ZINC=/usr/include ;;
    linuxarm) CXX="arm-linux-gnueabihf-g++ -mcpu=cortex-a8 -mfpu=vfpv3 -mfloat-abi=hard -mno-unaligned-access"
              AR=arm-linux-gnueabihf-ar; ZINC=$P/include ;;
  esac
  B=/tmp/libraw-$t; rm -rf $B; mkdir -p $B
  for o in $OBJS; do
    f=$(find $SRC/src -name "$o.cpp" | head -1)
    [ -n "$f" ] || { echo "no source for $o"; exit 1; }
    $CXX -O2 -w -I$SRC -I$P/include -I$ZINC -DLIBRAW_NOTHREADS -DUSE_ZLIB -DUSE_JPEG -DNO_LCMS \
      -c $f -o $B/$o.o &
    # a few at a time
    [ $(jobs -p | wc -l) -ge 4 ] && wait -n 2>/dev/null || true
  done
  wait
  rm -f $P/lib/libraw.a
  $AR rcs $P/lib/libraw.a $B/*.o
  mkdir -p $P/include/libraw
  cp $SRC/libraw/*.h $P/include/libraw/
  echo "$t: $(ls -la $P/lib/libraw.a | awk '{print $5}') bytes"
done
