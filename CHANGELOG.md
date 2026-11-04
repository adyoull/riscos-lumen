# Lumen changelog

## 0.0.37 (4 November 2026): HEIF photos

- HEIF photos (.heic, .heif, .hif), as iPhones and many other phones
  take them, with a new plug-in, LumenHEIF 1.0: libheif 1.23.7 with
  FFmpeg 5.1.10 cut down to its HEVC decoder. Built for ARMv7 with
  VFPv3, using FFmpeg's NEON code where the processor has it (the same
  pictures either way). The photo's own thumbnail is used when it has
  one; Display P3 photos are converted to sRGB; sideways photos are
  turned the right way up.
- Format plug-ins (API 1, docs/plugin-api.md): a plug-in's Plugin file
  says which files are its own (Kind: format, Format, Extensions,
  Signature), Lumen imports what installed plug-ins claim and runs them
  to read them, and the plug-in says where the photo's EXIF is for the
  Info tab. Lumen has no knowledge of HEIF itself; a new format needs
  only a new plug-in. Each is switched on and off on its own; one for a
  newer API is listed but not run. Previews (Raw files) cover them too.
- The library's catalogue (schema 7) keeps where a plug-in says a
  photo's EXIF is.
- Menus: the Photo menu is grouped into File, Edit, View, Adjust and
  Organise, and the Selection menu into File, Edit and Organise (every
  item kept). Keys are shown after one space, so RISC OS lines them up
  on the right; letter keys are shown with Shift (they work with or
  without it). New letters for keys that couldn't be shown that way: P
  flags (as /), X rejects (9), L the loupe (`), K the stack's pick (\).
- The Plug-ins menu and messages name plug-ins without the library they
  are built on ("Camera raw 1.1", "HEIF photos 1.0").
- The guide's menu pictures retaken.
- LumenHEIF-1.0-sources.zip has LumenHEIF's, libheif's and FFmpeg's
  sources and the patches, as the LGPL asks.

## 0.0.36 (4 November 2026): the first public release

- The same program as 0.0.35, released: version strings, "test build"
  wording gone from !Help and the guide.
- The help, the guide and Lumen's own message say where LumenRaw comes
  from: PackMan, or LumenRaw-1.1.zip from the release page (drag the
  LibRaw directory in its !Lumen.Plugins into Plug-ins > Add plug-in).
- README for GitHub; these notes moved here from it. The screenshot
  scripts no longer name the machine they were run on.

## Development notes, 0.0.1 to 0.0.35

The notes kept while Lumen was built, oldest first (some early versions
are covered together).

### What milestone 0 does

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

### Milestone 1 so far (0.0.3)

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

### Milestone 1, second part (0.0.4)

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

### 0.0.5

- Smooth scaling (resample.c): photos and thumbnails are no longer
  scaled by OS_SpriteOp (nearest neighbour: jagged edges, sparkle when a
  256-pixel thumbnail is shrunk to 112). draw.c keeps smoothed copies at
  exactly the pixels shown (tent filter when shrinking, bilinear when
  enlarging; 24 MB, least recently used first) and plots them 1:1. The
  half-size render shown while dragging is still plotted directly.
- Histogram: red, green and blue overlaid with
  additive overlaps, one column per pixel, smoothed, quarter guides,
  clipping marks. develop_rows now counts 256 bins per channel and
  luminance (`dev_hist`).
- The accent is the Wimp's dark blue (colour 8, #004499), light blue
  (colour 15) on dark cells, instead of orange.

### 0.0.6

- New icon: the aperture redrawn in the desktop's own colours (Wimp blues
  for the blades, cream light through the opening, a bevelled grey
  barrel, lit from the top left, outlined), `app/make-icons.py`.

### 0.0.7

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

### 0.0.8

- 100% view: Z, Photo > Zoom to 100%, or a click on the photo. The
  full-size original is decoded once (8-bit, in idle time), and only the
  part that fits the window is rendered (geom_region_rgb8, then develop),
  in bands, plotted 1:1. Drag to pan (the view moves with the pointer and
  renders on release); a click without moving, Z or Escape returns to fit.
  Adjustments while zoomed re-render the region; the histogram stays the
  proxy's.

### 0.0.9

- Sharpen (sharpen.c): unsharp mask on luminance (all three channels
  move by the same amount, so no colour fringes), gaussian sigma = radius
  in full-size pixels times the picture's scale, a one-level threshold.
  Applied last: to the viewer's half and full renders and the 100%
  region, and to exports in 48-row bands with overlapping margins (the
  result is identical to sharpening the whole picture at once).
- The panel's groups fold (click the name; disclosure triangles); when
  the window is too short, untouched groups fold from the bottom.

### 0.0.10

- No dead task: if Lumen couldn't start (its library couldn't be opened or
  created, or its windows made), it left a Wimp task with no icon bar
  icon behind. Every way out now calls Wimp_CloseDown (also from atexit).
- The library error says where the library is; with no <Choices$Write>
  (desktop not fully started) !Run puts it beside !Lumen instead of at
  ".Lumen.Library".
- If the icon sprites aren't in the Wimp's pool when Lumen starts, it
  loads <Lumen$Dir>.!Sprites itself; if there is still no picture, the
  icon bar icon is the word "Lumen", so there is always something to click.

### 0.0.11

- Fixed the Pi's "Lumen doesn't know where to keep its library ("")":
  main kept the pointer UnixLib's getenv returned for Lumen$Library, and
  the getenv of Lumen$Dir (made when the icon sprites had to be loaded)
  reused that buffer, emptying it. The value is copied at once now.
  Reproduced in ROL 5.31-5 (the old build showed "Can't open the
  catalogue ø:") and fixed there.
- !Run: the nested If ... Else could leave Lumen$Library unset; now two
  plain lines (Choices, else beside !Lumen).

### 0.0.12

- Loupe (`): a round, masked 300-pixel magnifier over the photo showing
  the full-size original at 100/200/400% (Adjust), pinnable (Select),
  with a bezel; it follows the sliders.
- Curves: RGB, red, green and blue, up to 10 points each, monotone cubic
  (Fritsch-Carlson, no overshoot), applied in develop's per-channel tables
  on the value as seen (after levels and contrast), RGB first. Text form
  `curves rgb=0.000:0.000,0.500:0.700,1.000:1.000 red=...`; part of a look
  (presets, copy/paste). The editor: channel buttons, the graph with the
  channel's histogram behind, add/drag/Adjust-remove points.
- Still to come in milestone 1: full-resolution rendering.

### 0.0.23

- Export options: fit in a box, colour space (sRGB, Adobe RGB, Display P3,
  with an ICC v2 profile; LittleCMS-checked), output sharpening, and
  file-naming patterns (naming.c: `{date}_{name}_{nnn}` and the rest).
- Printing (printbox.c): through the printer drivers, one photo a page,
  fit/fill/given size, turned to suit the page; each photo developed at
  the printer's resolution in the background, then plotted with
  JPEG_PlotScaled in the PDriver rectangle loop. Warns when a PostScript
  printer is set to grey.
- Captions, keywords and smart albums (schema 4): the Caption and keywords
  window works on a selection; search matches both; smart albums are a
  rule (words, rating, flag, adjusted, dates) evaluated in `cat_list`.
  XMP (xmp.c) out on export (JPEG APP1, PNG iTXt) and in on import.
- Colour (HSL, eight bands, hue/saturation/luminance by table on the hue as
  seen, applied in linear light), noise reduction (denoise.c: chroma blur,
  small bilateral on luminance; before sharpening, with band margins) and
  vignette (by place in the whole output, through `develop_region`).
- Versions and stacks (schema 5): duplicate versions share the original;
  stacks with a pick, open/closed, collapsed in the Browser; auto-stack by
  capture time.

### 0.0.24

Fixes from a review of 0.0.23:
- A closed stack whose pick the view leaves out (rejected, filtered by
  stars, another project) vanished whole; now the first member shown
  stands for it.
- The Viewer kept a stale place after the list changed (closing a stack,
  a smart album, a search): it now follows, or shows the stack's pick.
- Printing: a failed or aborted job was logged as printed; an unreadable
  prepared page was skipped silently; EndJob failure now aborts; the
  previous job is only reselected if ours was selected; scraps (and the
  LumenPrint directory) go on quit; preparation no longer slows down when
  Lumen isn't in front.
- Smart albums kept only 8 words between search and rule; now 4 + 8.
- Export margins with recipe and output sharpening together were too
  small (faint seams every 48 rows).
- Thumbnails with geometry and a vignette were darkened all over.
- Schema upgrades are now all-or-nothing.
- Unstack takes the whole stack apart, in any view; XMP captions prefer
  x-default; long keywords cut on a character boundary; Caption window
  fields hold the catalogue's full sizes; a smart album deleted while
  being edited is noticed; the Caption window closes if its photo goes.

### 0.0.25

- Retouch (R): spots painted out, healed (the patch's texture with a
  plane plus inverse-distance blend of the edge difference, so gradients
  carry through) or cloned, from a source found by ring matching; size
  and feather sliders, a brush outline under the pointer. retouch.c,
  applied on the photo upright before geometry.
- Brush (B): Dodge, Burn, Saturate, Desaturate strokes with a feathered
  brush, in linear light after the spots; up to 32 strokes, 1024 points.
- Black & White mixer (amount and red/green/blue weights) after HSL,
  before split toning.
- Colour labels (schema 6): seven coloured dots, in the Selection and
  Photo menus, the Info tab, the Adjustments tab and beside each name.
  Search matches the label's name.
- Compare (O): a photo pinned on the left beside the one being looked at.

### 0.0.26

- A raw photo exported or printed at 1024 pixels or less was made from
  the camera's preview inside the file, which can look quite unlike the
  raw; now everything bigger than a thumbnail is developed.
- The Info tab says when the LibRaw plug-in is switched off, rather than
  that it needs installing.
- LumenRaw 1.1: LibRaw 0.22.3 (was 0.21.4): 1,258 cameras (77 more, to
  2024: Canon R8/R50/R6 II/R5 II, Nikon Z 8/Z f/Z 6 III, Sony A7R V/A7C II/
  A6700, Fujifilm X-T5/X-H2/X100VI, OM-5, GH6/GH7, iPhone 15...), and
  0.22.x's fixes for damaged files. Data callback offsets are 64-bit.
- The LumenRaw User Guide (docs/lumenraw-guide, built by
  tools/lumenraw-guide/build.py from LibRaw's camera list), packaged in
  the plug-in as !Lumen.Plugins.LibRaw.Guide.

### 0.0.27

- Printing several photos to a page: 1, 2, 4, 6 or 9 in equal cells (the
  grid nearest 3:2 cells), or as many of a given size as fit (turned if
  more fit), with a gap; names under them (Font_Paint, Homerton 9 pt) for
  contact sheets. Each photo gets its own PDriver rectangle, clipped to
  its cell, so Fill never spills into a neighbour.

### 0.0.28

- Libraries: Export library (the whole library, the selection, a project or
  an album) writes a new library with every version, adjustment, rating,
  label, caption, keyword, project, album, stack and preset, and the
  thumbnails; optionally the originals too, in `Originals.<project>` with
  the project's folders kept. Import library merges another library:
  originals matched by size and hash, a version with a different recipe
  added as another version, projects, albums, keywords and presets by
  name; folder albums and watching stay behind. The other catalogue is
  ATTACHed and merged in one transaction after the files are copied
  (libxfer.c). A catalogue records its directory (`meta home`); opened
  from elsewhere, paths under it are moved with it.
- Check library (libcheck.c): SQLite's integrity check, orphaned rows,
  broken stacks, missing, failed and leftover thumbnails; Repair writes
  `Catalogue-backup` with VACUUM INTO first, REINDEXes if needed, and puts
  versions without a project and originals without a version into a
  "Recovered" project.

### 0.0.29

- Glow (an Orton effect): Amount, Spread, Highlights, Warmth. The bright
  parts of a 96-cell colour map of the source (the Highlights & Shadows
  map, now with colour) are developed, weighed above the threshold
  (smoothstep), tinted, blurred with a Gaussian of 1-12% of the longer
  side, laid through the geometry onto a 96-cell grid over the output, and
  screened over each pixel in linear light before the vignette. Same in
  the Viewer, 100%, loupe, thumbnails, exports and prints.
- A built-in style, Sunlit Garden: Andrew Youll's look for his dog photos
  (matched on his Labrador NEF) with a touch of glow.

### 0.0.30

- No flicker in the Viewer: every render (the half size while dragging or
  previewing a style, and the full proxy) is scaled to the size shown in
  idle time (draw_prepare) before the window is redrawn, so the redraw is
  a 1:1 copy; and only the margins round the photo are filled with the
  ground, so the old picture stays until the new one replaces it. (On the
  Pi, the ground fill followed by OS_SpriteOp's scaled plot was visible as
  a dark wipe down the photo.)
- Raw files: the full size is read ahead in the background once the photo
  is on screen (quietly; 100% and the loupe ask for it first if wanted
  sooner), and the loupe shows the picture on screen enlarged until it
  comes. The viewer's idle no longer stops while the full size is read.
- NEON, VFPv3 kept: resample32 and sharpen32 have NEON versions (GCC
  `#pragma GCC target ("fpu=neon")`, intrinsics) chosen at start by
  VFPSupport_Features (simd.c, fsys_neon); same pixels as the plain code
  (tested under ARM emulation both ways, `LUMEN_NEON`/`Lumen$NEON` 0 or 1).
  Sharpening's kernel now sums to exactly 1024, so its blur is a shift.
  LumenBench times both.

### 0.0.31

- resample32 rebuilt for NEON: each output row is made down first (the
  source rows it takes, one weight per row, so a sweep along memory: 16
  bytes at a time with NEON) into a 16-bit row, then across (a pixel's
  four channels in four lanes, loaded straight from that row, two pixels
  out together). The plain code does the same two passes, so the two
  still give identical pixels. 0.0.30's NEON version worked one pixel at a
  time and was slower than the plain code under QEMU; this one is about
  1.3-1.8x faster shrinking and up to 1.5x enlarging there (QEMU only:
  LumenBench on the Pi gives the real figures).

### 0.0.32

- Opening a JPEG is quicker. Pi 4 LumenBench (0.0.31) put opening a
  6000 x 4000 JPEG at 1600 at 710 ms. Most of it was thumbgen's box filter
  doing a 64-bit division for every pixel (a library call on ARM): it now
  works out each source column's target column once. Same pixels (a test
  compares it with the filter worked out directly). Under QEMU: 1850 ms to
  750 ms. Thumbnails on import go through the same code.
- JPEGs are read through a 64 KB stdio buffer (libjpeg asks for 4 KB at a
  time).
- Decoding at any eighth: libjpeg-turbo can scale by 1/8 to 8/8, not only
  1/2, 1/4, 1/8; at 3/8 a 6000-pixel photo is decoded at 2250 rather than
  3000 for the 1600 proxy (44% fewer pixels; QEMU 750 to 440 ms). Its NEON
  inverse DCT covers only the halvings, so it is off until measured on
  the Pi: `*Set Lumen$JPEGEighths 1` (`LUMEN_JPEG_EIGHTHS=1`) turns it on.
- LumenBench splits opening into reading the file (4 KB and 64 KB at a
  time), libjpeg-turbo's decoding alone (1/2 and 3/8) and the rest, and
  times opening at 3/8. Its gate now counts a whole frame while dragging
  (develop + enlarge + 5 ms plot); it used to leave out the enlarge.

### 0.0.33

- Previews: each original at 1600 (the Viewer's proxy: upright, sRGB,
  no adjustments) kept as a JPEG (quality 92) in Previews/xx/<original>_<key>,
  the key an FNV-1a hash of the file's size and date, its orientation,
  the side, and for raws the plug-in's program (path, size, date). An out
  of date preview simply isn't found and is made again (the old one goes).
  No catalogue change.
- Made in the background after thumbnails and the originals check, newest
  first (Choices: Raw files, the default; All photos; None; Discard). The
  Viewer opens from a preview when there is one, and keeps what it opened
  from the file as the preview otherwise.
- The photo being opened in the Viewer comes before everything else: the
  idle loop does nothing more until it is open, its LumenRaw job goes to
  the front of the queue (rawjob_hurry), and a background preview of the
  same photo is handed over (lib_preview_adopt) rather than started again.
- Check library counts previews and finds ones out of date or of no
  photo (repair removes them); removing photos removes their previews.
  Exported libraries don't carry previews (the new library makes its own).
- Status bar: "Making previews: <leaf>", then "Made N previews in T".

### 0.0.34

- Export at full size: geometry with no turn, straightening or
  perspective works out each column's source once per band (no divisions
  per pixel; the same sums, so the same pixels), and a pixel that falls
  exactly on a source pixel is a table lookup. thumbgen's box filter at
  1:1 copies the row. Identical files (checked on a 12 MP NEF export).
- Export: an original of unknown size (0 x 0) took the decoded picture's
  scale as thousands, so sharpening used its widest kernel (65 taps); now
  the decoded size stands in (test: same file as with the size given).
- LumenBench FILE: exports that photo at full size and splits the time:
  LumenRaw developing it, reading its picture back (a 12 MP raw is a
  35 MB PPM through the scrap directory), and Lumen's adjusting,
  sharpening and writing.

### 0.0.35

- The Activity history says how long an export took (a second or more),
  split into LumenRaw (export_phase EXPORT_PLUGIN: its job waiting or
  running), reading the photo or the plug-in's picture, and adjusting,
  sharpening and writing.
- LumenBench FILE is for JPEGs only: running LumenRaw from Bench with
  system() crashed on RISC OS (UnixLib's pthread ticker in the child, an
  undefined instruction), so raw files are timed by Lumen itself. The
  help no longer says to drag the photo into the task window (that types
  its contents): Shift-drag, or type the pathname.

Copyright (c) 2026 Andrew Youll. Released under the MIT licence: see the LICENSE file.
