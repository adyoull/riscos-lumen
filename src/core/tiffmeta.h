/* Copyright (c) 2026 Andrew Youll
   Released under the MIT licence: see the LICENSE file.
   SPDX-License-Identifier: MIT */
/* tiffmeta: TIFF files' structure, without the pictures. Reading: the
   first image's details, its ICC profile and XMP packet, and a compact copy
   of its tags with its EXIF and GPS IFDs (a small TIFF block of its own,
   for exif.h), wherever in the file the writer put them (often at the end).
   Writing: 8-bit RGB TIFFs, LZW with the horizontal predictor (what
   Photoshop and nearly every reader read), with a profile, XMP and the
   EXIF of an EXIF block (exifout.h) carried into the file's own tags.
   Byte-at-a-time reads and writes, so no unaligned access; every offset
   and count is checked. libtiff decodes the pictures (thumbgen.c). */
#ifndef LUMEN_TIFFMETA_H
#define LUMEN_TIFFMETA_H
#include <stddef.h>
#include <stdio.h>

typedef struct
{
  int width, height;
  int bits;                            /* bits per sample (the first sample's) */
  int samples;                         /* samples per pixel */
  int compression;                     /* TIFF's numbers: 1 none, 5 LZW, 7 JPEG, 8 or 32946 Deflate, 32773 PackBits */
  int photometric;                     /* 0 white is zero, 1 black is zero, 2 RGB, 3 palette, 5 CMYK, 6 YCbCr */
  int planar;                          /* 1 contiguous, 2 separate planes */
  int sample_format;                   /* 1 unsigned, 2 signed, 3 floating point */
  int orientation;                     /* 1-8 */
  int tiled;
  int raw;                             /* a camera raw file inside (DNG, or a NEF whose first image is its thumbnail) */
  unsigned char *icc;                  /* the ICC profile, or NULL */
  size_t icc_len;
  unsigned char *xmp;                  /* the XMP packet, or NULL */
  size_t xmp_len;
  unsigned char *exif;                 /* "II*\0"... IFD0's tags, its EXIF and GPS IFDs; NULL if none */
  size_t exif_len;
} tiff_meta;

/* 1 if the bytes start like a TIFF file ("II*\0" or "MM\0*"). */
int tiff_signature (const unsigned char *buf, size_t len);
/* Reads the first image's details from an open TIFF file. 0, or -1 if it
   isn't one (or has no usable first directory). Free with tiff_meta_free. */
int tiff_meta_read (FILE *f, tiff_meta *m);
void tiff_meta_free (tiff_meta *m);
/* 1 if the TIFF file is a camera raw file, whatever it's called: a DNG
   (DNGVersion), raw sensor data (CFA or linear raw), or a first image
   that's only a thumbnail with the photo in sub-IFDs (Nikon, Sony...). */
int tiff_is_raw (FILE *f);
/* How it's stored, for people: "LZW, 16 bits, RGB", "uncompressed, 8 bits, grey". */
void tiff_encoding (const tiff_meta *m, char *out, size_t n);

/* Writing: a w x h RGB TIFF, a row at a time, to f (open, at its start).
   icc, xmp and exif (an EXIF block's TIFF data) may be NULL. */
typedef struct tiff_writer tiff_writer;
tiff_writer *tiff_write_start (FILE *f, int w, int h, char *err, size_t errlen);
void tiff_write_extras (tiff_writer *t, const unsigned char *icc, size_t icc_len, const unsigned char *xmp,
                        size_t xmp_len, const unsigned char *exif, size_t exif_len);
/* One row of 8-bit RGB, top first. 0, or -1 if it can't be written. */
int tiff_write_row (tiff_writer *t, const unsigned char *rgb);
/* After the last row: its tags. 0, or -1. */
int tiff_write_finish (tiff_writer *t);
const char *tiff_write_error (const tiff_writer *t);
void tiff_write_free (tiff_writer *t);

#endif
