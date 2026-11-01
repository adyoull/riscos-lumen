# Lumen

A photo library and non-destructive editor for RISC OS 5, in the spirit of
Apple's Aperture. Milestone 0 (import, browse, rate, flag, albums) and most
of milestone 1 (the viewer, adjustments, Auto, crop and geometry, presets).

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

## Milestone 1 so far (0.0.3)

- The viewer: Return or double-click opens a photo; its thumbnail shows at
  once, a 1600-pixel proxy is decoded in idle time, turned into 16-bit
  linear light and developed a band of rows per slice.
- Adjustments: white balance (temperature, tint), exposure, levels (black,
  white, midtones), contrast, saturation, vibrancy. Per-channel steps are
  folded into one table per channel; a pixel costs six lookups plus the
  saturation step, all in 32-bit integers.
- Sliders follow the pointer (Wimp_DragBox type 7) at half resolution while
  dragging, full resolution on release. Adjust resets a slider.
- Recipes are text in the catalogue (`exposure ev=0.35`), saved at every
  change, with undo/redo, Reset all, before/after (M).
- Edited thumbnails are made again with the recipe; "Edited" marks them.
- `!Lumen.Bench` times decoding and developing on the machine it runs on.

## Milestone 1, second part (0.0.4)

- Geometry (geometry.c): quarter turns, straighten (±15°), vertical and
  horizontal perspective, and crop, in that order, run before the tone
  stages into a second linear image that is only remade when the geometry
  changes. Turning or keystoning zooms in just enough (checked at 33 points
  per edge) that no empty corners show. Bilinear sampling.
- The crop tool: the whole picture with the crop drawn over it, outside
  darkened; corners, edges, move, draw new; fixed shapes (Original, 1:1,
  3:2, 4:3, 16:9, either way round). Return applies, Escape cancels.
- Grids: thirds, quarters, fine; a fine grid while straightening.
- Auto (autoadj.c): white balance from near-neutral pixels (brighter ones
  weighted more; grey world when there are too few), damped and limited;
  exposure towards a mid-grey median; levels half way to the 0.5%/99.5%
  ends; contrast for flat pictures; vibrancy for dull ones. Geometry kept.
- White balance eyedropper: the picked colour (mapped back through the
  geometry to the original) is made exactly neutral by solving the
  develop pipeline's own gain model.
- Presets (a `preset` table: name and look) and copy/paste of adjustments,
  in the viewer and on a library selection. A look is the recipe without
  its geometry, so a preset never moves anyone's crop.

## 0.0.5

- Smooth scaling (resample.c): photos and thumbnails are no longer
  scaled by OS_SpriteOp (nearest neighbour: jagged edges, sparkle when a
  256-pixel thumbnail is shrunk to 112). draw.c keeps smoothed copies at
  exactly the pixels shown (tent filter when shrinking, bilinear when
  enlarging; 24 MB, least recently used first) and plots them 1:1. The
  half-size render shown while dragging is still plotted directly.
- Histogram as Aperture draws it: red, green and blue overlaid with
  additive overlaps, one column per pixel, smoothed, quarter guides,
  clipping marks. develop_rows now counts 256 bins per channel and
  luminance (`dev_hist`).
- The accent is the Wimp's dark blue (colour 8, #004499), light blue
  (colour 15) on dark cells, instead of orange.

## 0.0.6

- New icon: the aperture redrawn in the desktop's own colours (Wimp blues
  for the blades, cream light through the opening, a bevelled grey
  barrel, lit from the top left, outlined), `app/make-icons.py`.

## 0.0.7

- Export (export.c, exportbox.c): JPEG (quality; no colour subsampling
  from 90 up) or PNG (sRGB chunk), full size or a longer side of 3840,
  2048 or 1024. The original is decoded only as large as needed (DCT
  scaling + area averaging) into 8-bit RGB, then each 16-row band goes
  through the geometry (sampled in linear light from the 8-bit source),
  the develop pipeline and straight into the encoder; the filetype is set
  with OS_File 18; a failed or abandoned export removes its file.
- The Export box is a standard save box (Wimp icons, radio groups,
  writable name and quality) using the data transfer protocol:
  Message_DataSave, DataSaveAck, and DataLoad back to a program receiving
  <Wimp$Scrap>. Several photos go into a new directory. Exports are queued
  and run in idle time; the library's title shows "Exporting 2 of 5 (40%)".
- Tested in ROL 5.31-5: single JPEG dragged to a Filer window; three photos
  as PNG at 1024 into a new directory.

## 0.0.8

- 100% view: Z, Photo > Zoom to 100%, or a click on the photo. The
  full-size original is decoded once (8-bit, in idle time), and only the
  part that fits the window is rendered (geom_region_rgb8, then develop),
  in bands, plotted 1:1. Drag to pan (the view moves with the pointer and
  renders on release); a click without moving, Z or Escape returns to fit.
  Adjustments while zoomed re-render the region; the histogram stays the
  proxy's.

## 0.0.9

- Sharpen (sharpen.c): unsharp mask on luminance (all three channels
  move by the same amount, so no colour fringes), gaussian sigma = radius
  in full-size pixels times the picture's scale, a one-level threshold.
  Applied last: to the viewer's half and full renders and the 100%
  region, and to exports in 48-row bands with overlapping margins (the
  result is identical to sharpening the whole picture at once).
- The panel's groups fold (click the name; disclosure triangles); when
  the window is too short, untouched groups fold from the bottom.
- Still to come in milestone 1: the loupe, a curves editor, several
  versions of one original, full-resolution rendering.

## Layout

```
src/core/   portable C: catalogue (SQLite), EXIF, thumbnails, importer,
            recipe.c (adjustments as text), develop.c (the pipeline),
            geometry.c (turn, straighten, perspective, crop), autoadj.c
  fsys_riscos.c / fsys_posix.c   the filing system, RISC OS or Linux
  sqlite_vfs.c                   SQLite on plain POSIX calls, native paths
src/ui/     the Wimp front end: app, browser window, viewer, menus, looks
            (copy/paste and presets), sprites
tests/      test_core and test_develop (Linux and ARM/QEMU), test_ui and test_fsys
            (Fake RISC OS), bench.c (LumenBench), shots/ (make shots)
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

Tested on Linux, and in the desktop of riscos-linux-testbed (RISC OS 5.31-5
under QEMU). Not yet run on a Pi.
