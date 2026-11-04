#!/bin/bash
# Copyright (c) 2026 Andrew Youll
# Released under the MIT licence: see the LICENSE file.
# SPDX-License-Identifier: MIT
# Builds what the LumenHEIF plug-in is made of, as static libraries, for
# three targets:
#   riscos   - arm-riscos-gnueabihf (the plug-in)
#   host     - x86-64 Linux (fast tests)
#   linuxarm - arm-linux-gnueabihf (tests under QEMU with RISC OS's alignment rules)
#
# 1. FFmpeg 5.1.10 cut down to its HEVC decoder (libavcodec + libavutil,
#    LGPL options only), with riscos-ffmpeg's patches for the HEVC decoder
#    and its ARM code (deps/ffmpeg/*.patch) and its NEON alignment rewrite
#    (RISC OS traps unaligned loads). C is compiled for VFPv3 only; the NEON
#    assembly is used only when the processor has NEON (LumenHEIF tells
#    libavutil with av_force_cpu_flags), so ARMv7 machines without NEON
#    still work.
# 2. dav1d 1.5.4 (BSD 2-clause), the AV1 decoder for AVIF, with
#    riscos-ffmpeg's RISC OS patch (byte loads for the bitstream refill, a
#    halfword store in cdef, the stack probe) and its NEON alignment
#    rewrite (deps/dav1d), and Lumen's own patch so NEON is chosen at run
#    time (0101: the program sets dav1d_riscos_arm_cpu_flags). C as VFPv3,
#    the assembly with NEON, as for FFmpeg.
# 3. libheif 1.23.7 (LGPL 3) with its FFmpeg decoder (HEIC) and its dav1d
#    decoder (AVIF) built in, no plug-in loading, no other codecs.
# Sources: /build/src/ffmpeg-5.1.10 (from the tarball; patched here once),
# /build/src/dav1d-1.5.4 (dav1d_1.5.4.orig.tar.xz; patched here once) and
# /build/src/libheif-1.23.7 (git tag v1.23.7).
# Output: /build/lumen-deps/<target>/lib/{libavcodec,libavutil,libdav1d,libheif}.a
set -e
RCD=/build/riscos-crossdev
SRC=/build/src
OUT=/build/lumen-deps
HERE=$(cd "$(dirname "$0")" && pwd)
FF=$SRC/ffmpeg-5.1.10
HEIF=$SRC/libheif-1.23.7
JOBS=${JOBS:-$(nproc)}
DAV=$SRC/dav1d-1.5.4

if [ ! -f $FF/.lumen-patched ]; then
  (cd $FF && for p in $HERE/ffmpeg/*.patch; do patch -s -p1 < $p; done
   find libavcodec/arm libavutil/arm -name '*.S' -print0 |
     xargs -0 sh $HERE/ffmpeg/neon-align-apply.sh $HERE/ffmpeg/neon-align-ffmpeg.allow
   touch .lumen-patched)
fi

if [ ! -f $DAV/.lumen-patched ]; then
  [ -d $DAV ] || tar -C $SRC -xf $SRC/dav1d_1.5.4.orig.tar.xz
  (cd $DAV && patch -s -p1 < $HERE/dav1d/dav1d-1.5.4-riscos.patch
   patch -s -p1 < $HERE/dav1d/0101-lumen-riscos-cpu-flags.patch
   sh $HERE/ffmpeg/neon-align-apply.sh $HERE/dav1d/neon-align-dav1d.allow src/arm/32/*.S
   touch .lumen-patched)
fi

# dav1d with meson: a cross file for each target.
dav1d_build () {   # target prefix cc ar strip system cflags asm
  local t=$1 P=$2 CC=$3 AR=$4 STRIP=$5 SYS=$6 CFL=$7 ASM=$8
  local D=/tmp/dav1d-$1 cf=/tmp/dav1d-$1.cross cross=
  rm -rf $D
  # GCCSDK's gcc rejects -pthread (pthreads are UnixLib's): a wrapper drops it
  printf '#!/bin/sh\nfor a; do shift; [ "$a" = -pthread ] || set -- "$@" "$a"; done\nexec %s "$@"\n' "$CC" > /tmp/dav1d-$t-cc
  chmod +x /tmp/dav1d-$t-cc
  if [ $t != host ]; then
    {
      echo "[binaries]"
      echo "c = '/tmp/dav1d-$t-cc'"
      echo "ar = '$AR'"
      echo "strip = '$STRIP'"
      echo "[built-in options]"
      echo "c_args = [$(for f in $CFL; do printf "'%s', " $f; done)'-D_GNU_SOURCE']"
      echo "default_library = 'static'"
      echo "b_staticpic = false"
      echo "[host_machine]"
      echo "system = '$SYS'"
      echo "cpu_family = 'arm'"
      echo "cpu = 'armv7'"
      echo "endian = 'little'"
    } > $cf
    cross="--cross-file $cf"
  fi
  meson setup $D $DAV $cross --prefix=$P --libdir=lib --buildtype=release -Ddefault_library=static \
    -Denable_tools=false -Denable_tests=false -Denable_examples=false -Denable_asm=$ASM \
    > /tmp/dav1d-$t.log 2>&1 || { tail -30 /tmp/dav1d-$t.log; exit 1; }
  grep -E "HAVE_ASM|HAVE_AS_FUNC|ARCH_ARM " $D/config.h | tr '\n' ' '; echo
  { ninja -C $D && ninja -C $D install; } >> /tmp/dav1d-$t.log 2>&1 || { tail -30 /tmp/dav1d-$t.log; exit 1; }
  sed -i 's/ -pthread//' $P/lib/pkgconfig/dav1d.pc
}

for t in ${TARGETS:-riscos host linuxarm}; do
  P=$OUT/$t; HEIFX=
  case $t in
    riscos)   CROSS=$RCD/toolchain/env/bin/arm-riscos-gnueabihf-
              FFARCH="--enable-cross-compile --cross-prefix=$CROSS --arch=arm --cpu=armv7-a --target-os=linux"
              NEON="-mfpu=neon-vfpv3"; PLAIN="-mfpu=vfpv3"
              CF="-mfloat-abi=hard -mtune=cortex-a72 -fstack-clash-protection"
              ASM="--enable-neon --enable-vfp --disable-armv5te --disable-armv6 --enable-armv6t2 --disable-fast-unaligned"
              # UnixLib has no powl (long double is double on ARM EABI anyway)
              HEIFX="-Dpowl=pow"
              CMAKE_TC=$RCD/cmake/riscos.cmake ;;
    linuxarm) CROSS=arm-linux-gnueabihf-
              FFARCH="--enable-cross-compile --cross-prefix=$CROSS --arch=arm --cpu=armv7-a --target-os=linux"
              NEON="-mfpu=neon-vfpv3"; PLAIN="-mfpu=vfpv3"
              CF="-mfloat-abi=hard -mcpu=cortex-a8 -marm -mno-unaligned-access -DFF_STRICT_ALIGN"
              ASM="--enable-neon --enable-vfp --disable-armv5te --disable-armv6 --enable-armv6t2 --disable-fast-unaligned"
              CMAKE_TC=$RCD/cmake/linux-arm-aligntrap.cmake ;;
    host)     CROSS=; FFARCH=; NEON=; PLAIN=; CF=; ASM="--disable-x86asm"; CMAKE_TC= ;;
  esac
  B=/tmp/ffmpeg-$t; rm -rf $B; mkdir -p $B
  (cd $B && $FF/configure $FFARCH --prefix=$P \
     --extra-cflags="$NEON $CF" --enable-static --disable-shared --disable-pic $ASM \
     --disable-everything --enable-decoder=hevc --enable-parser=hevc \
     --disable-programs --disable-doc --disable-avdevice --disable-avformat --disable-avfilter \
     --disable-swscale --disable-swresample --disable-postproc --disable-network \
     --disable-pthreads --disable-w32threads --disable-os2threads \
     --disable-autodetect --disable-iconv --disable-zlib --disable-linux-perf --disable-debug \
     > configure.log)
  if [ -n "$NEON" ]; then
    # C as VFPv3 only (no NEON from the compiler, and no inline NEON), the
    # .S files still assembled with NEON (they say .fpu neon themselves).
    sed -i "s/^\(CFLAGS=.*\)$NEON/\1$PLAIN/" $B/ffbuild/config.mak
    sed -i 's/^#define HAVE_NEON_INLINE 1/#define HAVE_NEON_INLINE 0/' $B/config.h
  fi
  grep -E '^#define HAVE_(NEON|NEON_INLINE|VFP|ARMV6|FAST_UNALIGNED) ' $B/config.h | tr '\n' ' '; echo
  (cd $B && make -j$JOBS >make.log 2>&1 || { tail -30 make.log; exit 1; }
   make install >/dev/null)

  case $t in
    riscos)   dav1d_build $t $P ${CROSS}gcc ${CROSS}ar ${CROSS}strip riscos "$PLAIN $CF" true ;;
    linuxarm) dav1d_build $t $P ${CROSS}gcc ${CROSS}ar ${CROSS}strip linux "$PLAIN $CF" true ;;
    host)     dav1d_build $t $P gcc ar strip linux "" false ;;   # C only
  esac

  H=/tmp/libheif-$t; rm -rf $H; mkdir -p $H
  TC=; [ -n "$CMAKE_TC" ] && TC="-DCMAKE_TOOLCHAIN_FILE=$CMAKE_TC"
  CXXF="$PLAIN $CF ${HEIFX:-}"
  cmake -S $HEIF -B $H $TC -DCMAKE_BUILD_TYPE=Release -DBUILD_SHARED_LIBS=OFF \
    -DCMAKE_C_FLAGS="$CXXF" -DCMAKE_CXX_FLAGS="$CXXF" \
    -DCMAKE_INSTALL_PREFIX=$P -DFFMPEG_ROOT=$P -DFFMPEG_avcodec_INCLUDE_DIR=$P/include -DFFMPEG_avcodec_LIBRARY=$P/lib/libavcodec.a -DFFMPEG_avutil_INCLUDE_DIR=$P/include -DFFMPEG_avutil_LIBRARY=$P/lib/libavutil.a \
    -DENABLE_PLUGIN_LOADING=OFF -DWITH_LIBDE265=OFF -DWITH_X265=OFF -DWITH_AOM_DECODER=OFF -DWITH_AOM_ENCODER=OFF \
    -DWITH_DAV1D=ON -DWITH_DAV1D_PLUGIN=OFF -DDAV1D_INCLUDE_DIR=$P/include -DDAV1D_LIBRARY=$P/lib/libdav1d.a -DWITH_RAV1E=OFF -DWITH_SvtEnc=OFF -DWITH_X264=OFF -DWITH_OpenH264_DECODER=OFF \
    -DWITH_JPEG_DECODER=OFF -DWITH_JPEG_ENCODER=OFF -DWITH_OpenJPEG_DECODER=OFF -DWITH_OpenJPEG_ENCODER=OFF \
    -DWITH_OPENJPH_ENCODER=OFF -DWITH_KVAZAAR=OFF -DWITH_UVG266=OFF -DWITH_VVDEC=OFF -DWITH_VVENC=OFF \
    -DWITH_FFMPEG_DECODER=ON -DWITH_FFMPEG_DECODER_PLUGIN=OFF -DWITH_UNCOMPRESSED_CODEC=OFF \
    -DWITH_EXAMPLES=OFF -DWITH_GDK_PIXBUF=OFF -DWITH_LIBSHARPYUV=OFF -DBUILD_TESTING=OFF -DWITH_FUZZERS=OFF \
    -DENABLE_MULTITHREADING_SUPPORT=OFF -DENABLE_PARALLEL_TILE_DECODING=OFF \
    > $H/cmake.log 2>&1 || { tail -30 $H/cmake.log; exit 1; }
  cmake --build $H -j$JOBS --target heif > $H/build.log 2>&1 || { grep -m5 -B2 -A8 'error' $H/build.log; exit 1; }
  cmake --install $H > /dev/null 2>&1 || cp $H/libheif/libheif.a $P/lib/
  echo "$t: $(ls -la $P/lib/libavcodec.a $P/lib/libavutil.a $P/lib/libdav1d.a $P/lib/libheif.a | awk '{print $5}' | tr '\n' ' ')"
done
