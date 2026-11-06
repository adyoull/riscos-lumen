# Copyright (c) 2026 Andrew Youll
# Released under the MIT licence: see the LICENSE file.
# SPDX-License-Identifier: MIT
# Lumen: a photo library and non-destructive editor for RISC OS 5.
#
#   make            out/Lumen.zip (the !Lumen application, checked)
#   make check      every test: core on x86-64 and on ARM under QEMU with
#                   RISC OS's alignment rules, the desktop against the Fake
#                   RISC OS, and the RISC OS build's checks
#   make clean
#
# Needs riscos-crossdev (RCD_DIR) with its toolchain and devkit, and the
# libraries from deps/build-deps.sh in DEPS (one prefix per target).
RCD_DIR ?= /build/riscos-crossdev
DEPS ?= /build/lumen-deps
include $(RCD_DIR)/riscos.mk
.DEFAULT_GOAL := all

VERSION := 0.0.38
DATE := 04 Nov 2026
OUT := out

CORE := src/core/exif.c src/core/exifout.c src/core/colour.c src/core/rawjob.c src/core/plugins.c src/core/prefs.c src/core/thumbgen.c src/core/tiffmeta.c src/core/recipe.c src/core/develop.c src/core/geometry.c src/core/resample.c src/core/sharpen.c src/core/denoise.c src/core/retouch.c src/core/export.c src/core/autoadj.c src/core/styles.c src/core/presets.c src/core/naming.c src/core/xmp.c src/core/catalog.c src/core/library.c src/core/libxfer.c src/core/libcheck.c src/core/simd.c src/core/sqlite_vfs.c
CORE_H := $(wildcard src/core/*.h)
UI := src/ui/main.c src/ui/app.c src/ui/mainwin.c src/ui/browser.c src/ui/viewer.c src/ui/looks.c src/ui/exportbox.c src/ui/plugwin.c src/ui/choicewin.c src/ui/activity.c src/ui/printbox.c src/ui/tagbox.c src/ui/smartbox.c src/ui/libbox.c src/ui/help.c src/ui/menus.c src/ui/thumbcache.c src/ui/draw.c src/ui/filetypes.c
UI_H := $(wildcard src/ui/*.h)
SQLITE := third_party/sqlite/sqlite3.c
SQLITE_FLAGS := -DSQLITE_OS_OTHER=1 -DSQLITE_THREADSAFE=0 -DSQLITE_OMIT_LOAD_EXTENSION \
  -DSQLITE_OMIT_WAL -DSQLITE_DEFAULT_MEMSTATUS=0 -DSQLITE_DQS=0 -DSQLITE_TEMP_STORE=3 \
  -DSQLITE_OMIT_DEPRECATED -DSQLITE_OMIT_SHARED_CACHE -DSQLITE_MAX_MMAP_SIZE=0 \
  -DSQLITE_DEFAULT_LOCKING_MODE=1 -Ithird_party/sqlite
WARN := -Wall -Wextra -Wno-format-truncation
DEFS := -DLUMEN_VERSION='"$(VERSION)"' -DLUMEN_DATE='"$(DATE)"'

all: $(OUT)/Lumen.zip
check: test-host test-arm test-develop test-develop-arm test-ui test-fsys checks

# --- SQLite, once per target (it is big) -------------------------------------
$(OUT)/riscos/sqlite3.o: $(SQLITE) | riscos-toolchain-present
	@mkdir -p $(@D)
	$(RO_CC) $(RISCOS_CFLAGS) $(SQLITE_FLAGS) -c -o $@ $<
$(OUT)/host/sqlite3.o: $(SQLITE)
	@mkdir -p $(@D)
	gcc -O2 -g $(SQLITE_FLAGS) -c -o $@ $<
$(OUT)/arm/sqlite3.o: $(SQLITE)
	@mkdir -p $(@D)
	$(LINUXARM_CC) $(LINUXARM_CFLAGS) $(SQLITE_FLAGS) -c -o $@ $<
$(OUT)/host32/sqlite3.o: $(SQLITE)
	@mkdir -p $(@D)
	$(FAKE_CC) -m32 -O1 -g -funsigned-char $(SQLITE_FLAGS) -c -o $@ $<

# --- RISC OS build ------------------------------------------------------------
RO_LIBS := -L$(DEPS)/riscos/lib -ltiff -ljpeg -lpng16 $(DEVKIT_LDFLAGS) -lz -lm
$(OUT)/lumen: $(CORE) src/core/fsys_riscos.c $(UI) $(CORE_H) $(UI_H) $(OUT)/riscos/sqlite3.o Makefile
	@mkdir -p $(OUT)
	$(RO_CC) $(RISCOS_CFLAGS) $(WARN) $(DEFS) -Isrc/core -Isrc/ui -Ithird_party/sqlite -I$(DEPS)/riscos/include \
	  $(RISCOS_LDFLAGS) -o $@ $(CORE) src/core/fsys_riscos.c $(UI) $(OUT)/riscos/sqlite3.o $(RO_LIBS)

$(OUT)/bench: tests/bench.c $(CORE) $(CORE_H) $(OUT)/riscos/sqlite3.o
	$(RO_CC) $(RISCOS_CFLAGS) $(WARN) $(DEFS) -Isrc/core -Ithird_party/sqlite -I$(DEPS)/riscos/include \
	  $(RISCOS_LDFLAGS) -o $@ tests/bench.c src/core/develop.c src/core/retouch.c src/core/geometry.c src/core/resample.c src/core/sharpen.c src/core/simd.c src/core/recipe.c src/core/thumbgen.c src/core/tiffmeta.c src/core/exif.c src/core/exifout.c src/core/colour.c src/core/rawjob.c src/core/plugins.c src/core/fsys_riscos.c \
	  src/core/export.c src/core/denoise.c src/core/xmp.c src/core/catalog.c src/core/sqlite_vfs.c $(OUT)/riscos/sqlite3.o \
	  -L$(DEPS)/riscos/lib -ltiff -ljpeg -lpng16 $(DEVKIT_LDFLAGS) -lz -lm

checks: $(OUT)/lumen $(OUT)/bench
	$(CHECK_ALL) $(OUT)/bench
	$(CHECK_ALL) $(OUT)/lumen
	$(CHECK_UNALIGNED) -Isrc/core -Isrc/ui -Ithird_party/sqlite -I$(DEPS)/riscos/include $(CORE) src/core/fsys_riscos.c $(UI)

LABEL_PNGS := $(wildcard app/labels/lumlab*.png)
$(OUT)/!Sprites,ff9: app/lumen.png app/sm-lumen.png $(LABEL_PNGS)
	@mkdir -p $(OUT)
	$(PNG2SPRITE) $@ '!lumen=app/lumen.png' 'sm!lumen=app/sm-lumen.png' $(foreach f,$(LABEL_PNGS),'$(basename $(notdir $(f)))=$(f)')

APPFILES := app/!Run,feb app/!Boot,feb app/!Help,fff
LICENCES := $(wildcard app/Licences/*)
GUIDE := docs/guide/index.html $(wildcard docs/guide/images/*.png)
# Lumen's file icon, for file types it opens that the desktop has none for
# (filetypes.c): templates lmnfile (34 x 34) and lmnsmall (17 x 17).
$(OUT)/FileIcons,ff9: tools/mkfileicon.py tools/riscosdraw.py app/lumen.png
	@mkdir -p $(OUT)
	cd tools && python3 mkfileicon.py ../$(OUT)/fileicon34.png ../$(OUT)/fileicon17.png ../app/lumen.png
	$(PNG2SPRITE) $@ 'lmnfile=$(OUT)/fileicon34.png' 'lmnsmall=$(OUT)/fileicon17.png'

$(OUT)/Lumen.zip: $(OUT)/lumen $(OUT)/!Sprites,ff9 $(OUT)/FileIcons,ff9 $(APPFILES) $(LICENCES) $(GUIDE) | checks
	rm -rf $(OUT)/!Lumen && mkdir -p $(OUT)/!Lumen
	$(RO_STRIP) -o $(OUT)/lumen.stripped $(OUT)/lumen
	$(ELF2AIF) -e $(OUT)/lumen.stripped $(OUT)/!Lumen/!RunImage,ff8 >/dev/null
	$(RO_STRIP) -o $(OUT)/bench.stripped $(OUT)/bench
	$(ELF2AIF) -e $(OUT)/bench.stripped $(OUT)/!Lumen/Bench,ff8 >/dev/null
	cp $(APPFILES) $(OUT)/!Sprites,ff9 $(OUT)/FileIcons,ff9 $(OUT)/!Lumen/
	mkdir -p $(OUT)/!Lumen/Licences
	for f in $(LICENCES); do cp $$f $(OUT)/!Lumen/Licences/$$(basename $$f),fff; done
	mkdir -p $(OUT)/!Lumen/Guide/images
	cp docs/guide/index.html $(OUT)/!Lumen/Guide/index.html,faf
	for f in docs/guide/images/*.png; do cp $$f $(OUT)/!Lumen/Guide/images/$$(basename $$f),b60; done
	$(ROZIP) $@ -C $(OUT) !Lumen
	@echo "$@: unzip on RISC OS and run !Lumen"

# --- Tests ----------------------------------------------------------------------
HOST_DEPS := -I$(DEPS)/host/include $(DEPS)/host/lib/libtiff.a $(DEPS)/host/lib/libjpeg.a $(DEPS)/host/lib/libpng16.a -lz -lm
$(OUT)/test_core_host: tests/test_core.c $(CORE) src/core/fsys_posix.c $(CORE_H) $(OUT)/host/sqlite3.o
	gcc -O1 -g $(WARN) -fsanitize=address,undefined -Ithird_party/sqlite -I$(DEPS)/host/include -o $@ \
	  tests/test_core.c $(CORE) src/core/fsys_posix.c $(OUT)/host/sqlite3.o \
	  $(DEPS)/host/lib/libtiff.a $(DEPS)/host/lib/libjpeg.a $(DEPS)/host/lib/libpng16.a -lz -lm
test-host: $(OUT)/test_core_host $(OUT)/lumenraw-host $(OUT)/lumenheif-host
	rm -rf $(OUT)/work-host && LUMENRAW=$(OUT)/lumenraw-host LUMENHEIF=$(OUT)/lumenheif-host RAW_SAMPLES=$(RAW_SAMPLES) $(OUT)/test_core_host $(OUT)/work-host

$(OUT)/test_develop_host: tests/test_develop.c $(CORE) src/core/fsys_posix.c $(CORE_H) $(OUT)/host/sqlite3.o
	gcc -O2 -g $(WARN) -fsanitize=address,undefined -Ithird_party/sqlite -I$(DEPS)/host/include -o $@ \
	  tests/test_develop.c $(CORE) src/core/fsys_posix.c $(OUT)/host/sqlite3.o \
	  $(DEPS)/host/lib/libtiff.a $(DEPS)/host/lib/libjpeg.a $(DEPS)/host/lib/libpng16.a -lz -lm
test-develop: $(OUT)/test_develop_host
	rm -rf $(OUT)/work-dev && $(OUT)/test_develop_host $(OUT)/work-dev

ARM_LIBS := $(DEPS)/linuxarm/lib/libtiff.a $(DEPS)/linuxarm/lib/libjpeg.a $(DEPS)/linuxarm/lib/libpng16.a $(DEPS)/linuxarm/lib/libz.a -lm
$(OUT)/test_core_arm: tests/test_core.c $(CORE) src/core/fsys_posix.c $(CORE_H) $(OUT)/arm/sqlite3.o
	$(LINUXARM_CC) $(LINUXARM_CFLAGS) $(WARN) -Ithird_party/sqlite -I$(DEPS)/linuxarm/include -o $@ \
	  tests/test_core.c $(CORE) src/core/fsys_posix.c $(OUT)/arm/sqlite3.o $(ARM_LIBS)
test-arm: $(OUT)/test_core_arm
	@# Both of libjpeg-turbo's paths: its NEON routines, and the plain C fallback.
	rm -rf $(OUT)/work-arm && PATH="$(RISCOS_TOOLCHAIN)/bin:$$PATH" JSIMD_FORCENEON=1 LUMEN_NEON=1 $(ALIGNTRAP) $(OUT)/test_core_arm $(OUT)/work-arm
	rm -rf $(OUT)/work-arm && PATH="$(RISCOS_TOOLCHAIN)/bin:$$PATH" JSIMD_FORCENONE=1 LUMEN_NEON=0 $(ALIGNTRAP) $(OUT)/test_core_arm $(OUT)/work-arm

$(OUT)/test_develop_arm: tests/test_develop.c $(CORE) src/core/fsys_posix.c $(CORE_H) $(OUT)/arm/sqlite3.o
	$(LINUXARM_CC) $(LINUXARM_CFLAGS) $(WARN) -Ithird_party/sqlite -I$(DEPS)/linuxarm/include -o $@ \
	  tests/test_develop.c $(CORE) src/core/fsys_posix.c $(OUT)/arm/sqlite3.o $(ARM_LIBS)
test-develop-arm: $(OUT)/test_develop_arm
	rm -rf $(OUT)/work-dev-arm && PATH="$(RISCOS_TOOLCHAIN)/bin:$$PATH" JSIMD_FORCENEON=1 LUMEN_NEON=1 $(ALIGNTRAP) $(OUT)/test_develop_arm $(OUT)/work-dev-arm
	rm -rf $(OUT)/work-dev-arm && PATH="$(RISCOS_TOOLCHAIN)/bin:$$PATH" JSIMD_FORCENONE=1 LUMEN_NEON=0 $(ALIGNTRAP) $(OUT)/test_develop_arm $(OUT)/work-dev-arm

H32 := $(DEPS)/host32
$(OUT)/test_ui: tests/test_ui.c $(CORE) src/core/fsys_posix.c $(filter-out src/ui/main.c,$(UI)) $(CORE_H) $(UI_H) $(OUT)/host32/sqlite3.o $(FAKE_SRC)
	$(FAKE_CC) $(FAKE_CFLAGS) -Wno-format-truncation $(DEFS) -Ithird_party/sqlite -I$(H32)/include -Isrc/core -Isrc/ui -o $@ \
	  tests/test_ui.c $(CORE) src/core/fsys_posix.c $(filter-out src/ui/main.c,$(UI)) $(OUT)/host32/sqlite3.o $(FAKE_SRC) \
	  $(H32)/lib/libtiff.a $(H32)/lib/libjpeg.a $(H32)/lib/libpng16.a $(H32)/lib/libz.a -lm
test-ui: $(OUT)/test_ui $(OUT)/test_core_host
	rm -rf $(OUT)/work-ui && $(OUT)/test_ui $(OUT)/work-ui

$(OUT)/test_fsys: tests/test_fsys.c src/core/fsys_riscos.c src/core/fsys.h $(FAKE_SRC)
	$(FAKE_CC) $(FAKE_CFLAGS) -Isrc/core -o $@ tests/test_fsys.c src/core/fsys_riscos.c $(FAKE_SRC)
test-fsys: $(OUT)/test_fsys
	$(OUT)/test_fsys

clean:
	rm -rf $(OUT)

.PHONY: all check checks test-host test-arm test-develop test-develop-arm test-ui test-fsys clean

# --- The LibRaw plug-in (LumenRaw), a separate program in its own package --
# deps/build-libraw.sh builds LibRaw into $(DEPS)/<target>/lib/libraw.a.
RAW_SRC := plugins/libraw/lumenraw.cpp
RAW_FLAGS := -O2 -Wall -Wextra
RAW_SAMPLES ?= $(HOME)/rawsamples
$(OUT)/LumenRaw,ff8: $(RAW_SRC)
	@mkdir -p $(OUT)
	$(RO_CXX) $(RISCOS_CFLAGS) $(RAW_FLAGS) -I$(DEPS)/riscos/include -o $(OUT)/lumenraw.elf $(RAW_SRC) \
	  $(RISCOS_LDFLAGS) -L$(DEPS)/riscos/lib -lraw -ljpeg $(DEVKIT_LDFLAGS) -lz -lm
	$(CHECK_ALL) $(OUT)/lumenraw.elf
	$(RO_STRIP) -o $(OUT)/lumenraw.stripped $(OUT)/lumenraw.elf
	$(ELF2AIF) -e $(OUT)/lumenraw.stripped $@ >/dev/null
$(OUT)/lumenraw-host: $(RAW_SRC)
	g++ $(RAW_FLAGS) -I$(DEPS)/host/include -o $@ $(RAW_SRC) $(DEPS)/host/lib/libraw.a $(DEPS)/host/lib/libjpeg.a -lz -lm
$(OUT)/lumenraw-arm: $(RAW_SRC)
	arm-linux-gnueabihf-g++ -mcpu=cortex-a8 -mfpu=vfpv3 -mfloat-abi=hard -mno-unaligned-access -no-pie $(RAW_FLAGS) \
	  -I$(DEPS)/linuxarm/include -o $@ $(RAW_SRC) $(DEPS)/linuxarm/lib/libraw.a $(DEPS)/linuxarm/lib/libjpeg.a \
	  $(DEPS)/linuxarm/lib/libz.a -lm
RAW_VERSION := 1.2
LIBRAW_VERSION := 0.22.3
# The plug-in's release zip, laid out as it goes into Lumen (PackMan's
# LumenRaw package maps it into Apps.Graphics.!Lumen.Plugins.LibRaw), and
# the LibRaw source it was built from (the LGPL asks for it with the binary).
RAW_GUIDE := docs/lumenraw-guide/index.html $(wildcard docs/lumenraw-guide/images/*.png)
HEIF_GUIDE := docs/lumenheif-guide/index.html $(wildcard docs/lumenheif-guide/images/*.png)
$(OUT)/LumenRaw-$(RAW_VERSION).zip: $(OUT)/LumenRaw,ff8 plugins/libraw/ReadMe plugins/libraw/Licence plugins/libraw/Plugin $(RAW_GUIDE)
	rm -rf $(OUT)/rawpkg && mkdir -p $(OUT)/rawpkg/!Lumen/Plugins/LibRaw/Guide/images
	cp $(OUT)/LumenRaw,ff8 $(OUT)/rawpkg/!Lumen/Plugins/LibRaw/
	cp plugins/libraw/ReadMe $(OUT)/rawpkg/!Lumen/Plugins/LibRaw/ReadMe,fff
	cp plugins/libraw/Licence $(OUT)/rawpkg/!Lumen/Plugins/LibRaw/Licence,fff
	cp plugins/libraw/Plugin $(OUT)/rawpkg/!Lumen/Plugins/LibRaw/Plugin,fff
	cp docs/lumenraw-guide/index.html $(OUT)/rawpkg/!Lumen/Plugins/LibRaw/Guide/index.html,faf
	for f in docs/lumenraw-guide/images/*.png; do cp $$f $(OUT)/rawpkg/!Lumen/Plugins/LibRaw/Guide/images/$$(basename $$f),b60; done
	$(ROZIP) $@ -C $(OUT)/rawpkg !Lumen
$(OUT)/LibRaw-$(LIBRAW_VERSION)-source.zip:
	cd /build/src/LibRaw && git archive --format=zip --prefix=LibRaw-$(LIBRAW_VERSION)/ -o $(CURDIR)/$@ $(LIBRAW_VERSION)
# The guide on its own, to read on any computer.
$(OUT)/Lumen-User-Guide.zip: $(GUIDE)
	rm -f $@ && cd docs && zip -qr $(CURDIR)/$@ guide
$(OUT)/LumenRaw-User-Guide.zip: $(RAW_GUIDE)
	rm -f $@ && cd docs && zip -qr $(CURDIR)/$@ lumenraw-guide
$(OUT)/LumenHEIF-User-Guide.zip: $(HEIF_GUIDE)
	rm -f $@ && cd docs && zip -qr $(CURDIR)/$@ lumenheif-guide
guide: $(OUT)/Lumen-User-Guide.zip $(OUT)/LumenRaw-User-Guide.zip $(OUT)/LumenHEIF-User-Guide.zip
plugin: $(OUT)/LumenRaw-$(RAW_VERSION).zip $(OUT)/LibRaw-$(LIBRAW_VERSION)-source.zip
# The helper on real raw files (not in the repository: their licences
# vary), on Linux and on ARM with RISC OS's alignment rules.
test-plugin: $(OUT)/lumenraw-host $(OUT)/lumenraw-arm
	RAW_SAMPLES=$(RAW_SAMPLES) tests/test_plugin.sh $(OUT)/lumenraw-host "" $(OUT)/plugin-host
	RAW_SAMPLES=$(RAW_SAMPLES) tests/test_plugin.sh $(OUT)/lumenraw-arm "PATH=$(RISCOS_TOOLCHAIN)/bin:$$PATH $(ALIGNTRAP)" $(OUT)/plugin-arm

# --- The HEIF plug-in (LumenHEIF), a separate program in its own package --
# deps/build-heif.sh builds FFmpeg's HEVC decoder (libavcodec, libavutil)
# and libheif into $(DEPS)/<target>/lib.
HEIF_SRC := plugins/heif/lumenheif.c
HEIF_FLAGS := -O2 -Wall -Wextra
HEIF_LIBS = $(1)/lib/libheif.a $(1)/lib/libavcodec.a $(1)/lib/libavutil.a $(1)/lib/libdav1d.a
$(OUT)/LumenHEIF,ff8: $(HEIF_SRC) src/core/exif.c src/core/exif.h
	@mkdir -p $(OUT)
	$(RO_CC) $(RISCOS_CFLAGS) $(HEIF_FLAGS) -Isrc/core -I$(DEPS)/riscos/include -c $(HEIF_SRC) -o $(OUT)/lumenheif.o
	$(RO_CC) $(RISCOS_CFLAGS) $(HEIF_FLAGS) -Isrc/core -c src/core/exif.c -o $(OUT)/lumenheif-exif.o
	$(RO_CXX) $(RISCOS_CFLAGS) -o $(OUT)/lumenheif.elf $(OUT)/lumenheif.o $(OUT)/lumenheif-exif.o \
	  $(RISCOS_LDFLAGS) $(call HEIF_LIBS,$(DEPS)/riscos) -lm
	$(CHECK_ALL) $(OUT)/lumenheif.elf
	$(RO_STRIP) -o $(OUT)/lumenheif.stripped $(OUT)/lumenheif.elf
	$(ELF2AIF) -e $(OUT)/lumenheif.stripped $@ >/dev/null
$(OUT)/lumenheif-host: $(HEIF_SRC) src/core/exif.c src/core/exif.h
	gcc $(HEIF_FLAGS) -Isrc/core -I$(DEPS)/host/include -c $(HEIF_SRC) -o $(OUT)/lumenheif-host.o
	gcc $(HEIF_FLAGS) -Isrc/core -c src/core/exif.c -o $(OUT)/lumenheif-host-exif.o
	g++ -no-pie -o $@ $(OUT)/lumenheif-host.o $(OUT)/lumenheif-host-exif.o $(call HEIF_LIBS,$(DEPS)/host) -lm
$(OUT)/lumenheif-arm: $(HEIF_SRC) src/core/exif.c src/core/exif.h
	arm-linux-gnueabihf-gcc -mcpu=cortex-a8 -mfpu=vfpv3 -mfloat-abi=hard -marm -mno-unaligned-access $(HEIF_FLAGS) \
	  -Isrc/core -I$(DEPS)/linuxarm/include -c $(HEIF_SRC) -o $(OUT)/lumenheif-arm.o
	arm-linux-gnueabihf-gcc -mcpu=cortex-a8 -mfpu=vfpv3 -mfloat-abi=hard -marm -mno-unaligned-access $(HEIF_FLAGS) \
	  -Isrc/core -c src/core/exif.c -o $(OUT)/lumenheif-arm-exif.o
	arm-linux-gnueabihf-g++ -no-pie -o $@ $(OUT)/lumenheif-arm.o $(OUT)/lumenheif-arm-exif.o \
	  $(call HEIF_LIBS,$(DEPS)/linuxarm) -lm
HEIF_VERSION := 1.2
LIBHEIF_VERSION := 1.23.7
FFMPEG_VERSION := 5.1.10
# The plug-in's release zip, laid out as it goes into Lumen (PackMan's
# LumenHEIF package maps it into Apps.Graphics.!Lumen.Plugins.HEIF), and
# the sources it was built from (the LGPL asks for them with the binary):
# libheif at its tag, FFmpeg's release tarball with the patches applied for
# RISC OS, and the script that builds them.
$(OUT)/LumenHEIF-$(HEIF_VERSION).zip: $(OUT)/LumenHEIF,ff8 plugins/heif/ReadMe plugins/heif/Licence plugins/heif/Plugin plugins/heif/Changes $(HEIF_GUIDE)
	rm -rf $(OUT)/heifpkg && mkdir -p $(OUT)/heifpkg/!Lumen/Plugins/HEIF/Guide/images
	cp $(OUT)/LumenHEIF,ff8 $(OUT)/heifpkg/!Lumen/Plugins/HEIF/
	cp plugins/heif/ReadMe $(OUT)/heifpkg/!Lumen/Plugins/HEIF/ReadMe,fff
	cp plugins/heif/Licence $(OUT)/heifpkg/!Lumen/Plugins/HEIF/Licence,fff
	cp plugins/heif/Plugin $(OUT)/heifpkg/!Lumen/Plugins/HEIF/Plugin,fff
	cp plugins/heif/Changes $(OUT)/heifpkg/!Lumen/Plugins/HEIF/Changes,fff
	cp docs/lumenheif-guide/index.html $(OUT)/heifpkg/!Lumen/Plugins/HEIF/Guide/index.html,faf
	for f in docs/lumenheif-guide/images/*.png; do cp $$f $(OUT)/heifpkg/!Lumen/Plugins/HEIF/Guide/images/$$(basename $$f),b60; done
	$(ROZIP) $@ -C $(OUT)/heifpkg !Lumen
$(OUT)/LumenHEIF-$(HEIF_VERSION)-sources.zip: deps/build-heif.sh $(wildcard deps/ffmpeg/*) $(wildcard deps/dav1d/*) $(HEIF_SRC) plugins/heif/SOURCES
	rm -rf $(OUT)/heifsrc && mkdir -p $(OUT)/heifsrc/LumenHEIF-$(HEIF_VERSION)-sources/ffmpeg-patches $(OUT)/heifsrc/LumenHEIF-$(HEIF_VERSION)-sources/dav1d-patches
	cd /build/src/libheif-$(LIBHEIF_VERSION) && git archive --format=tar --prefix=libheif-$(LIBHEIF_VERSION)/ v$(LIBHEIF_VERSION) | gzip -9 > $(CURDIR)/$(OUT)/heifsrc/LumenHEIF-$(HEIF_VERSION)-sources/libheif-$(LIBHEIF_VERSION).tar.gz
	cp $(FFMPEG_TARBALL) $(DAV1D_TARBALL) $(OUT)/heifsrc/LumenHEIF-$(HEIF_VERSION)-sources/
	cp deps/dav1d/* $(OUT)/heifsrc/LumenHEIF-$(HEIF_VERSION)-sources/dav1d-patches/
	cp deps/ffmpeg/* deps/build-heif.sh $(HEIF_SRC) $(OUT)/heifsrc/LumenHEIF-$(HEIF_VERSION)-sources/ffmpeg-patches/
	cp plugins/heif/SOURCES $(OUT)/heifsrc/LumenHEIF-$(HEIF_VERSION)-sources/ReadMe
	rm -f $@ && cd $(OUT)/heifsrc && zip -qr $(CURDIR)/$@ LumenHEIF-$(HEIF_VERSION)-sources
FFMPEG_TARBALL ?= /build/src/ffmpeg-$(FFMPEG_VERSION).tar.xz
DAV1D_TARBALL ?= /build/src/dav1d_1.5.4.orig.tar.xz
heif-plugin: $(OUT)/LumenHEIF-$(HEIF_VERSION).zip $(OUT)/LumenHEIF-$(HEIF_VERSION)-sources.zip
# The helper on HEIF photos made for the tests (tests/heif), on Linux, and on
# ARM with RISC OS's alignment rules with and without NEON: the pictures
# must match the PC's.
test-heif: $(OUT)/lumenheif-host $(OUT)/lumenheif-arm
	tests/test_heif.sh $(OUT)/lumenheif-host "" $(OUT)/heif-host
	LUMEN_NEON=1 tests/test_heif.sh $(OUT)/lumenheif-arm "PATH=$(RISCOS_TOOLCHAIN)/bin:$$PATH $(ALIGNTRAP)" $(OUT)/heif-arm-neon $(OUT)/heif-host
	LUMEN_NEON=0 tests/test_heif.sh $(OUT)/lumenheif-arm "PATH=$(RISCOS_TOOLCHAIN)/bin:$$PATH $(ALIGNTRAP)" $(OUT)/heif-arm-plain $(OUT)/heif-host

# --- Screenshots: Lumen's own drawing, recorded against the Fake RISC OS ----------
# out/shots/lumen-*.png. The window frame, menus' frames and icon bar are an
# approximation of RISC OS 5's; everything inside is what Lumen drew.
$(OUT)/shot: tests/shots/shot.c tests/shots/widths.h $(CORE) src/core/fsys_posix.c $(filter-out src/ui/main.c,$(UI)) $(CORE_H) $(UI_H) $(OUT)/host32/sqlite3.o $(FAKE_SRC)
	$(FAKE_CC) $(FAKE_CFLAGS) -Wno-format-truncation $(DEFS) -Ithird_party/sqlite -I$(H32)/include -Isrc/core -Isrc/ui -Itests/shots -o $@ \
	  tests/shots/shot.c $(CORE) src/core/fsys_posix.c $(filter-out src/ui/main.c,$(UI)) $(OUT)/host32/sqlite3.o $(FAKE_SRC) \
	  $(H32)/lib/libtiff.a $(H32)/lib/libjpeg.a $(H32)/lib/libpng16.a $(H32)/lib/libz.a -lm
shots: $(OUT)/shot
	rm -rf $(OUT)/shots $(OUT)/photos
	python3 tests/shots/make-photos.py "$(OUT)/photos/Lake District 2026"
	$(OUT)/shot "$(OUT)/photos/Lake District 2026" $(OUT)/shots
	python3 tests/shots/render.py $(OUT)/shots app/lumen.png
.PHONY: shots
