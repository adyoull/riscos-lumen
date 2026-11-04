/* Copyright (c) 2026 Andrew Youll
   Released under the MIT licence: see the LICENSE file.
   SPDX-License-Identifier: MIT */
/* exifout: the EXIF block written into an exported picture.

   From the original's own EXIF (a JPEG's APP1 block, or a TIFF-based raw
   file such as a NEF, which is TIFF throughout) it keeps what describes
   the photo - camera, lens, exposure, dates, GPS, author, copyright - and
   leaves out what no longer fits the new file: the thumbnail (IFD1), the
   maker's notes (their offsets point into the old file), the raw data's
   own tags, and the Interoperability block. The orientation becomes 1
   (the export is turned already), the pixel size is the export's, the
   colour space sRGB (what Lumen writes), and Software says Lumen.

   Without EXIF in the original (a PNG, a CR3 or RAF raw...) the block is
   made from what the library knows: camera, date and exposure.

   The block is little-endian TIFF, what follows "Exif\0\0" in a JPEG's
   APP1 segment and what a PNG's eXIf chunk holds; at most 65000 bytes. */
#ifndef LUMEN_EXIFOUT_H
#define LUMEN_EXIFOUT_H
#include <stddef.h>
#include "exif.h"

#define EXIFOUT_MAX 65000

/* Finds the EXIF (TIFF) data in the first len bytes of a file: a JPEG's
   APP1 Exif segment, or a whole TIFF file. 0 and *off, *n if found. */
int exif_find_tiff (const unsigned char *buf, size_t len, size_t *off, size_t *n);

/* Builds the block into out (cap bytes, EXIFOUT_MAX is enough). src is
   the original's TIFF data (NULL if none); known fills in for it when it
   is NULL (and may be NULL itself). w x h is the exported size. Returns
   the length, 0 if there is nothing worth writing. colour_space is EXIF's
   ColorSpace: 1 for sRGB, 0xFFFF (uncalibrated) for another space, whose
   ICC profile then goes in the file too. */
size_t exif_for_export (const unsigned char *src, size_t src_len, const img_info *known,
                        int w, int h, const char *software, int colour_space, unsigned char *out, size_t cap);

#endif
