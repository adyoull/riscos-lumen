#!/bin/sh
# Copyright (c) 2026 Andrew Youll
# Released under the MIT licence: see the LICENSE file.
# SPDX-License-Identifier: MIT
# Builds libtiff 4.7.1 (libtiff licence, BSD-like) statically for Lumen's
# three targets, after build-deps.sh (it uses that libjpeg and zlib):
#   riscos   - arm-riscos-gnueabihf (the app)
#   host     - x86-64 Linux (fast unit tests)
#   linuxarm - arm-linux-gnueabihf (tests under qemu with RISC OS alignment rules)
#   host32   - 32-bit x86 (the desktop tests against the Fake RISC OS)
# Codecs: none, PackBits, LZW, Deflate (zlib) and JPEG (libjpeg-turbo): what
# cameras, scanners and editors write. No others, no tools, no C++.
# Source: /build/src/libtiff-4.7.1 (git tag v4.7.1).
set -e
HERE=$(cd "$(dirname "$0")" && pwd)
RCD=/build/riscos-crossdev
SRC=/build/src/libtiff-4.7.1
OUT=/build/lumen-deps
for t in ${TARGETS:-riscos host linuxarm host32}; do
  P=$OUT/$t
  case $t in
    riscos)   TC="-DCMAKE_TOOLCHAIN_FILE=$RCD/cmake/riscos.cmake"; ZINC=$RCD/devkit/env/include; ZLIB=$RCD/devkit/env/lib/libz.a ;;
    host)     TC=""; ZINC=/usr/include; ZLIB=$(ls /usr/lib/x86_64-linux-gnu/libz.a 2>/dev/null || echo /usr/lib/x86_64-linux-gnu/libz.so) ;;
    linuxarm) TC="-DCMAKE_TOOLCHAIN_FILE=$OUT/linuxarm.cmake -DCMath_HAVE_LIBC_POW=ON -DCMath_LIBRARY=m"; ZINC=$P/include; ZLIB=$P/lib/libz.a ;;
    host32)   TC="-DCMAKE_TOOLCHAIN_FILE=$HERE/host32.cmake -DCMAKE_CXX_COMPILER_WORKS=ON -DCMath_HAVE_LIBC_POW=ON -DCMath_LIBRARY=m"; ZINC=$P/include; ZLIB=$P/lib/libz.a ;;
  esac
  B=/tmp/bt-$t; rm -rf $B
  cmake -S $SRC -B $B $TC -DCMAKE_BUILD_TYPE=Release -DBUILD_SHARED_LIBS=OFF -DCMAKE_POSITION_INDEPENDENT_CODE=OFF \
    -Dtiff-tools=OFF -Dtiff-tests=OFF -Dtiff-contrib=OFF -Dtiff-docs=OFF -Dtiff-cxx=OFF -Dtiff-opengl=OFF \
    -Dzlib=ON -DZLIB_INCLUDE_DIR=$ZINC -DZLIB_LIBRARY=$ZLIB -Dlibdeflate=OFF \
    -Djpeg=ON -DJPEG_INCLUDE_DIR=$P/include -DJPEG_LIBRARY=$P/lib/libjpeg.a -Dold-jpeg=OFF -Djpeg12=OFF \
    -Djbig=OFF -Dlerc=OFF -Dlzma=OFF -Dwebp=OFF -Dzstd=OFF -Dpixarlog=OFF \
    -Dccitt=OFF -Dthunder=OFF -Dnext=OFF -Dlogluv=OFF -Dmdi=OFF -Dpackbits=ON -Dlzw=ON \
    -Dld-version-script=OFF -DCMAKE_INSTALL_PREFIX=$P > $B.log 2>&1 || { tail -30 $B.log; exit 1; }
  grep -E '^ *(Support for|Deflate|JPEG|LZW|PackBits)' $B.log | head -8 || true
  cmake --build $B -j4 --target tiff >> $B.log 2>&1 || { tail -30 $B.log; exit 1; }
  mkdir -p $P/lib $P/include
  cp $B/libtiff/libtiff.a $P/lib/
  cp $SRC/libtiff/tiff.h $SRC/libtiff/tiffio.h $SRC/libtiff/tiffvers.h $B/libtiff/tiffconf.h $P/include/ 2>/dev/null \
    || cp $SRC/libtiff/tiff.h $SRC/libtiff/tiffio.h $B/libtiff/tiffvers.h $B/libtiff/tiffconf.h $P/include/
  echo "$t: libtiff.a $(stat -c %s $P/lib/libtiff.a) bytes"
done
