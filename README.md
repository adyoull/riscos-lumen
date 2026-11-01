# Lumen

A photo library and non-destructive editor for RISC OS 5, in the spirit of
Apple's Aperture. This is milestone 0: import, browse, rate, flag, albums.

- Design spec: https://claude.ai/artifact/ELstfvAAkFbZGNj7wbhYTs
- Target: RISC OS 5.30+ on a Raspberry Pi 4, 400 or CM4.
- Needs SharedUnixLibrary 1.16+ and ARMEABISupport (PackMan).

## What milestone 0 does

- Drag a folder or files from the Filer to the window or icon bar icon:
  JPEG and PNG files are found recursively (application directories are
  skipped), added to the catalogue, and given 256-pixel thumbnails, all in
  the background in 10-20 ms slices between Wimp polls.
- Originals are never written. Re-importing finds files already there by
  path or by content (size plus a hash of the first and last 64 KB).
- One project per imported folder; albums across projects.
- The browser shows All photos, Flagged, Rejected, Last import, a project
  or an album, with a rating filter, at three thumbnail sizes.
- Keys: 0-5 rating, 9 reject, / flag, cursor keys, Ctrl-A, Escape.
- EXIF orientation, capture time, camera and exposure are read on import.

## Layout

```
src/core/   portable C: catalogue (SQLite), EXIF, thumbnails, importer
  fsys_riscos.c / fsys_posix.c   the filing system, RISC OS or Linux
  sqlite_vfs.c                   SQLite on plain POSIX calls, native paths
src/ui/     the Wimp front end: app, browser window, menus, sprites
tests/      test_core (Linux and ARM/QEMU), test_ui and test_fsys (Fake RISC OS)
app/        !Run, !Boot, !Help, icons (make-icons.py draws them)
third_party/sqlite/   SQLite 3.49.2 amalgamation
```

Library on disc: `<Lumen$Library>` (default `<Choices$Write>.Lumen.Library`)
holds `Catalogue` (SQLite) and `Thumbs` (256 subdirectories of JPEGs).

## Building

Needs riscos-crossdev 1.8.2 (`make prebuilt devkit`) and libjpeg-turbo
3.0.4 and libpng 1.6.44 built for four targets (`deps/build-deps.sh`):
RISC OS, x86-64, ARM Linux and 32-bit x86 (for the Fake RISC OS tests).

```
make RCD_DIR=/path/to/riscos-crossdev DEPS=/path/to/deps check all
```

`make check` runs test_core on x86-64 (with ASan/UBSan) and on ARM under
qemu-arm-aligntrap, test_ui and test_fsys against the Fake RISC OS, and
riscos-crossdev's checks on the RISC OS binary. `make` builds
`out/Lumen.zip`.

## Status

Tested on Linux only so far. Not yet run on RISC OS.
