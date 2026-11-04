/* Copyright (c) 2026 Andrew Youll
   Released under the MIT licence: see the LICENSE file.
   SPDX-License-Identifier: MIT */
/* exif: reads what the library shows from the start of a JPEG or PNG file:
   pixel size, and for JPEG the EXIF orientation, capture time, camera and
   exposure. Byte-at-a-time reads only (no unaligned access), bounds checked
   against the buffer it is given. */
#ifndef LUMEN_EXIF_H
#define LUMEN_EXIF_H
#include <stddef.h>

enum { IMG_UNKNOWN = 0, IMG_JPEG = 1, IMG_PNG = 2, IMG_RAW = 3 };

typedef struct
{
  int format;                 /* IMG_* */
  int width, height;          /* stored pixels (before orientation) */
  int orientation;            /* EXIF 1-8; 1 when absent */
  char taken[20];             /* "YYYY:MM:DD HH:MM:SS" or "" */
  char make[32], model[48];
  double exposure;            /* seconds, 0 if unknown */
  double fnumber, focal;      /* 0 if unknown */
  int iso;                    /* 0 if unknown */
  long thumb_offset;          /* embedded JPEG thumbnail, from the file start; 0 if none */
  long thumb_length;
} img_info;

/* 1 if the name ends in a camera raw extension ("/nef" or ".nef"). */
int img_has_raw_extension (const char *name);
/* What kind of image the first bytes are. */
int img_probe (const unsigned char *buf, size_t len);
/* The same, knowing the file's name: camera raw files (decoded by the
   LibRaw plug-in) are IMG_RAW, by their own signature (Canon CR2 and CR3,
   Fuji RAF, Olympus ORF, Panasonic RW2) or, for the many that are TIFF
   files inside (Nikon NEF, Sony ARW, Adobe DNG, Pentax PEF...), by a TIFF
   header and a raw file's extension ("/nef" on RISC OS, ".nef" elsewhere). */
int img_probe_named (const unsigned char *buf, size_t len, const char *name);
/* What can be read of a raw file's metadata without the plug-in: the
   camera, date and exposure from a TIFF-based raw's EXIF. Its size and
   orientation come from the plug-in. 0 if anything was found. */
int img_read_raw_info (const unsigned char *buf, size_t len, img_info *info);

/* Fills *info from buf, the first len bytes of the file (64 KB or more is
   enough for nearly every camera). Returns 0 if the format was recognised
   and its pixel size found, -1 otherwise. */
int img_read_info (const unsigned char *buf, size_t len, img_info *info);

/* Every EXIF tag in the file, named and formatted for people to read:
   fn (ctx, group, name, value) once for each, in file order. group is
   "Camera" (the main image's IFD0), "Exif", "GPS" or "Interoperability".
   Tags it has no name for are given as "Tag &XXXX"; binary blocks (the
   maker's notes, say) as their size. Returns the number given, or -1 if
   buf has no EXIF block (a PNG, or a JPEG without one). */
typedef void (*exif_tag_fn) (void *ctx, const char *group, const char *name, const char *value);
int exif_list (const unsigned char *buf, size_t len, exif_tag_fn fn, void *ctx);
/* The same from the EXIF block itself (the TIFF data after "Exif\0\0"). */
int exif_list_tiff (const unsigned char *tiff, size_t len, exif_tag_fn fn, void *ctx);

#endif
