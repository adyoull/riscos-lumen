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

/* IMG_PLUGIN: a format Lumen knows nothing of itself, read by a format
   plug-in that claims it (by its extension or first bytes: plugins.h).
   (4 is thumbgen's own: a picture the raw plug-in made.) IMG_TIFF: a
   TIFF that isn't a camera raw file (read with libtiff: tiffmeta.h).
   IMG_FITS: an astronomer's FITS picture (fits.h). */
enum { IMG_UNKNOWN = 0, IMG_JPEG = 1, IMG_PNG = 2, IMG_RAW = 3, IMG_PLUGIN = 5, IMG_TIFF = 6, IMG_FITS = 7 };

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
  char lens[96];              /* the lens's name (EXIF LensModel, or the raw plug-in's), or "" */
  double focal35;             /* the focal length in 35 mm terms (EXIF), 0 unknown */
  double lens_fmin, lens_fmax, lens_amin;   /* its focal lengths and widest aperture (LensSpecification), 0 unknown */
  long thumb_offset;          /* embedded JPEG thumbnail, from the file start; 0 if none */
  long thumb_length;
  long exif_offset;           /* its EXIF (TIFF) data, from the file start, when a plug-in */
  long exif_length;           /* says where it is (plug-in formats); 0 if not known */
  char encoding[48];          /* how a plug-in's photo is encoded ("AV1, 10 bits, 4:2:0"), or "" */
  int geo;                    /* GEO_*: what's known of where it was taken (EXIF GPS) */
  int lat, lon;               /* WGS84, in ten-millionths of a degree, north and east positive */
  int alt_mm;                 /* millimetres above sea level (below: negative) */
  int bearing;                /* the way the camera faced, thousandths of a degree from north (0-359999) */
} img_info;

/* img_info.geo: the latitude and longitude, the altitude, the bearing. */
enum { GEO_POS = 1, GEO_ALT = 2, GEO_DIR = 4 };

/* The IPTC caption and keywords ("a, b") in a JPEG's start, as written
   by Photoshop and many photo programs; "" for none. 1 if any. */
int img_iptc_read (const unsigned char *buf, size_t len, char *caption, size_t cn, char *keywords, size_t kn);

/* A position for people: "54.601234\xB0 N, 3.134567\xB0 W" (Latin-1 degree sign). */
void img_geo_text (int lat, int lon, char *out, size_t n);

/* 1 if the name ends in a camera raw extension ("/nef" or ".nef"). */
int img_has_raw_extension (const char *name);
/* A photo's size in megapixels, for people: "12 MP", "15.6 MP", "0.4 MP"
   (one decimal place, none when it's whole; two below 0.1 MP). "" if the
   size isn't known. */
void img_megapixels (int width, int height, char *out, size_t n);
/* Read by a plug-in (a camera raw, or a format a plug-in claims). */
int img_is_plugin_format (int format);
/* What kind of image the first bytes are. */
int img_probe (const unsigned char *buf, size_t len);
/* The same, knowing the file's name: camera raw files (decoded by the
   LibRaw plug-in) are IMG_RAW, by their own signature (Canon CR2 and CR3,
   Fuji RAF, Olympus ORF, Panasonic RW2) or, for the many that are TIFF
   files inside (Nikon NEF, Sony ARW, Adobe DNG, Pentax PEF...), by a TIFF
   header and a raw file's extension ("/nef" on RISC OS, ".nef" elsewhere).
   Any other TIFF is IMG_TIFF. */
int img_probe_named (const unsigned char *buf, size_t len, const char *name);
/* The camera, date, exposure and orientation from a TIFF's tags (the
   compact copy tiffmeta.h makes); the size is the caller's. */
void img_read_tiff_tags (const unsigned char *tiff, size_t len, img_info *info);
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
