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

VERSION := 0.0.1
DATE := 01 Nov 2026
OUT := out

CORE := src/core/exif.c src/core/thumbgen.c src/core/catalog.c src/core/library.c src/core/sqlite_vfs.c
CORE_H := $(wildcard src/core/*.h)
UI := src/ui/main.c src/ui/app.c src/ui/browser.c src/ui/menus.c src/ui/thumbcache.c src/ui/draw.c
UI_H := $(wildcard src/ui/*.h)
SQLITE := third_party/sqlite/sqlite3.c
SQLITE_FLAGS := -DSQLITE_OS_OTHER=1 -DSQLITE_THREADSAFE=0 -DSQLITE_OMIT_LOAD_EXTENSION \
  -DSQLITE_OMIT_WAL -DSQLITE_DEFAULT_MEMSTATUS=0 -DSQLITE_DQS=0 -DSQLITE_TEMP_STORE=3 \
  -DSQLITE_OMIT_DEPRECATED -DSQLITE_OMIT_SHARED_CACHE -DSQLITE_MAX_MMAP_SIZE=0 \
  -DSQLITE_DEFAULT_LOCKING_MODE=1 -Ithird_party/sqlite
WARN := -Wall -Wextra -Wno-format-truncation
DEFS := -DLUMEN_VERSION='"$(VERSION)"' -DLUMEN_DATE='"$(DATE)"'

all: $(OUT)/Lumen.zip
check: test-host test-arm test-ui test-fsys checks

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
RO_LIBS := -L$(DEPS)/riscos/lib -ljpeg -lpng16 $(DEVKIT_LDFLAGS) -lz -lm
$(OUT)/lumen: $(CORE) src/core/fsys_riscos.c $(UI) $(CORE_H) $(UI_H) $(OUT)/riscos/sqlite3.o
	@mkdir -p $(OUT)
	$(RO_CC) $(RISCOS_CFLAGS) $(WARN) $(DEFS) -Ithird_party/sqlite -I$(DEPS)/riscos/include \
	  $(RISCOS_LDFLAGS) -o $@ $(CORE) src/core/fsys_riscos.c $(UI) $(OUT)/riscos/sqlite3.o $(RO_LIBS)

checks: $(OUT)/lumen
	$(CHECK_ALL) $(OUT)/lumen
	$(CHECK_UNALIGNED) -Isrc/core -Isrc/ui -Ithird_party/sqlite -I$(DEPS)/riscos/include $(CORE) src/core/fsys_riscos.c $(UI)

$(OUT)/!Sprites,ff9: app/lumen.png app/sm-lumen.png
	@mkdir -p $(OUT)
	$(PNG2SPRITE) $@ '!lumen=app/lumen.png' 'sm!lumen=app/sm-lumen.png'

APPFILES := app/!Run,feb app/!Boot,feb app/!Help,fff
$(OUT)/Lumen.zip: $(OUT)/lumen $(OUT)/!Sprites,ff9 $(APPFILES) | checks
	rm -rf $(OUT)/!Lumen && mkdir -p $(OUT)/!Lumen
	$(RO_STRIP) -o $(OUT)/lumen.stripped $(OUT)/lumen
	$(ELF2AIF) -e $(OUT)/lumen.stripped $(OUT)/!Lumen/!RunImage,ff8 >/dev/null
	cp $(APPFILES) $(OUT)/!Sprites,ff9 $(OUT)/!Lumen/
	$(ROZIP) $@ -C $(OUT) !Lumen
	@echo "$@: unzip on RISC OS and run !Lumen"

# --- Tests ----------------------------------------------------------------------
HOST_DEPS := -I$(DEPS)/host/include $(DEPS)/host/lib/libjpeg.a $(DEPS)/host/lib/libpng16.a -lz -lm
$(OUT)/test_core_host: tests/test_core.c $(CORE) src/core/fsys_posix.c $(CORE_H) $(OUT)/host/sqlite3.o
	gcc -O1 -g $(WARN) -fsanitize=address,undefined -Ithird_party/sqlite -I$(DEPS)/host/include -o $@ \
	  tests/test_core.c $(CORE) src/core/fsys_posix.c $(OUT)/host/sqlite3.o \
	  $(DEPS)/host/lib/libjpeg.a $(DEPS)/host/lib/libpng16.a -lz -lm
test-host: $(OUT)/test_core_host
	rm -rf $(OUT)/work-host && $(OUT)/test_core_host $(OUT)/work-host

ARM_LIBS := $(DEPS)/linuxarm/lib/libjpeg.a $(DEPS)/linuxarm/lib/libpng16.a $(DEPS)/linuxarm/lib/libz.a -lm
$(OUT)/test_core_arm: tests/test_core.c $(CORE) src/core/fsys_posix.c $(CORE_H) $(OUT)/arm/sqlite3.o
	$(LINUXARM_CC) $(LINUXARM_CFLAGS) $(WARN) -Ithird_party/sqlite -I$(DEPS)/linuxarm/include -o $@ \
	  tests/test_core.c $(CORE) src/core/fsys_posix.c $(OUT)/arm/sqlite3.o $(ARM_LIBS)
test-arm: $(OUT)/test_core_arm
	rm -rf $(OUT)/work-arm && PATH="$(RISCOS_TOOLCHAIN)/bin:$$PATH" $(ALIGNTRAP) $(OUT)/test_core_arm $(OUT)/work-arm

H32 := $(DEPS)/host32
$(OUT)/test_ui: tests/test_ui.c $(CORE) src/core/fsys_posix.c $(filter-out src/ui/main.c,$(UI)) $(CORE_H) $(UI_H) $(OUT)/host32/sqlite3.o $(FAKE_SRC)
	$(FAKE_CC) $(FAKE_CFLAGS) $(DEFS) -Ithird_party/sqlite -I$(H32)/include -Isrc/core -Isrc/ui -o $@ \
	  tests/test_ui.c $(CORE) src/core/fsys_posix.c $(filter-out src/ui/main.c,$(UI)) $(OUT)/host32/sqlite3.o $(FAKE_SRC) \
	  $(H32)/lib/libjpeg.a $(H32)/lib/libpng16.a $(H32)/lib/libz.a -lm
test-ui: $(OUT)/test_ui $(OUT)/test_core_host
	rm -rf $(OUT)/work-ui && $(OUT)/test_ui $(OUT)/work-ui

$(OUT)/test_fsys: tests/test_fsys.c src/core/fsys_riscos.c src/core/fsys.h $(FAKE_SRC)
	$(FAKE_CC) $(FAKE_CFLAGS) -Isrc/core -o $@ tests/test_fsys.c src/core/fsys_riscos.c $(FAKE_SRC)
test-fsys: $(OUT)/test_fsys
	$(OUT)/test_fsys

clean:
	rm -rf $(OUT)

.PHONY: all check checks test-host test-arm test-ui test-fsys clean
