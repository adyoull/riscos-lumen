#!/bin/sh
# Builds libjpeg-turbo and libpng statically for three targets:
#   riscos   - arm-riscos-gnueabihf (the app)
#   host     - x86-64 Linux (fast unit tests)
#   linuxarm - arm-linux-gnueabihf (tests under qemu with RISC OS alignment rules)
set -e
RCD=/build/riscos-crossdev
SRC=/build/src
OUT=/build/lumen-deps
for t in riscos host linuxarm; do
  P=$OUT/$t; mkdir -p $P
  case $t in
    riscos)   TC="-DCMAKE_TOOLCHAIN_FILE=$RCD/cmake/riscos.cmake"; ZINC=$RCD/devkit/env/include; ZLIB=$RCD/devkit/env/lib/libz.a ;;
    host)     TC=""; ZINC=/usr/include; ZLIB=$(ls /usr/lib/x86_64-linux-gnu/libz.a 2>/dev/null || echo /usr/lib/x86_64-linux-gnu/libz.so) ;;
    linuxarm) TC="-DCMAKE_TOOLCHAIN_FILE=/build/lumen-deps/linuxarm.cmake"; ZINC=$P/include; ZLIB=$P/lib/libz.a ;;
  esac
  if [ $t = linuxarm ] && [ ! -f $P/lib/libz.a ]; then
    rm -rf /tmp/zb && cp -r $SRC/zlib /tmp/zb && (cd /tmp/zb && CC=arm-linux-gnueabihf-gcc CFLAGS="-O2 -mcpu=cortex-a8 -mfpu=vfpv3 -mfloat-abi=hard -mno-unaligned-access" ./configure --static --prefix=$P >/dev/null && make -s install >/dev/null)
  fi
  # libjpeg-turbo's NEON code on the two ARM targets, with a fallback: the
  # library is built for VFPv3 like the rest of Lumen, only its NEON routines
  # with -mfpu=neon, and it decides at run time. On RISC OS that's
  # patches/libjpeg-turbo-riscos-neon.patch (VFPSupport_Features 0; it also
  # makes the NEON Huffman encoder store bytes, not unaligned words); on Linux
  # /proc/cpuinfo. *Set JSIMD_FORCENONE 1 (or JSIMD_FORCENEON 1) overrides it.
  case $t in
    riscos|linuxarm) SIMD="-DWITH_SIMD=ON -DREQUIRE_SIMD=ON" ;;
    *)               SIMD="-DWITH_SIMD=OFF" ;;
  esac
  (cd $SRC/libjpeg-turbo && git checkout -q simd/arm/aarch32/jsimd.c simd/arm/jchuff.h && git apply $OUT/patches/libjpeg-turbo-riscos-neon.patch)
  rm -rf /tmp/bj-$t && cmake -S $SRC/libjpeg-turbo -B /tmp/bj-$t $TC -DCMAKE_BUILD_TYPE=Release -DENABLE_SHARED=OFF $SIMD -DWITH_TURBOJPEG=OFF -DCMAKE_INSTALL_PREFIX=$P -DCMAKE_POSITION_INDEPENDENT_CODE=OFF >/tmp/bj-$t.log 2>&1
  cmake --build /tmp/bj-$t -j2 --target jpeg-static >>/tmp/bj-$t.log 2>&1
  mkdir -p $P/lib $P/include
  cp /tmp/bj-$t/libjpeg.a $P/lib/ && cp $SRC/libjpeg-turbo/jpeglib.h $SRC/libjpeg-turbo/jmorecfg.h $SRC/libjpeg-turbo/jerror.h /tmp/bj-$t/jconfig.h $P/include/
  rm -rf /tmp/bp-$t && cmake -S $SRC/libpng -B /tmp/bp-$t $TC -DCMAKE_BUILD_TYPE=Release -DPNG_SHARED=OFF -DPNG_TESTS=OFF -DPNG_TOOLS=OFF -DPNG_ARM_NEON=off -DZLIB_INCLUDE_DIR=$ZINC -DZLIB_LIBRARY=$ZLIB -DCMAKE_INSTALL_PREFIX=$P >/tmp/bp-$t.log 2>&1
  cmake --build /tmp/bp-$t -j2 >>/tmp/bp-$t.log 2>&1
  cmake --install /tmp/bp-$t >>/tmp/bp-$t.log 2>&1
  echo "$t: $(ls $P/lib)"
done
