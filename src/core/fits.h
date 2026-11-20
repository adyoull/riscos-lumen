/* Copyright (c) 2026 Andrew Youll
   Released under the MIT licence: see the LICENSE file.
   SPDX-License-Identifier: MIT */
/* fits: reads FITS pictures, the format astronomers' cameras and programs
   save (.fits, .fit, .fts): from a dedicated astro camera, a DSLR's
   frames converted by Siril, or a stack.

   A FITS file is text header blocks (80-character cards, KEY = value, in
   2880-byte blocks ending with END) followed by the data, big-endian:
   8, 16 or 32-bit integers (with BZERO and BSCALE) or 32 or 64-bit
   floating point. Lumen reads a 2-D picture (grey, or a camera's raw
   colour mosaic when BAYERPAT says so) or a 3-D one of three planes (red,
   green, blue), in the first HDU that holds one (the primary, or an IMAGE
   extension). Tile-compressed FITS (fpack) isn't read.

   Astronomical pictures are linear: the sky is nearly black and the
   detail is in the faintest levels. The reader stretches them for the
   screen as Siril's and PixInsight's auto-stretch does (a shadows clip and
   a midtones balance from each channel's median and its median absolute
   deviation). A picture that is already bright (its median above a fifth
   of the range) is shown as it is, scaled from its range.

   Rows: FITS puts the first row at the bottom. Lumen shows it so unless
   ROWORDER says TOP-DOWN. A colour mosaic's BAYERPAT (with XBAYROFF and
   YBAYROFF) is taken as starting at the top left of the picture as shown,
   which is how Siril and the capture programs give it. */
#ifndef LUMEN_FITS_H
#define LUMEN_FITS_H
#include <stdio.h>
#include <stddef.h>
#include "exif.h"

typedef struct
{
  int bitpix;                 /* 8, 16, 32, -32, -64 */
  int w, h, planes;           /* planes: 1 or 3 */
  double bzero, bscale;
  long data;                  /* where the data starts in the file */
  int hdu;                    /* 0 the primary, 1... an extension */
  int bottom_up;              /* the first row is the bottom (the FITS default) */
  char bayer[5];              /* "RGGB", "BGGR", "GRBG", "GBRG", or "" */
  int xoff, yoff;             /* the mosaic's offsets (XBAYROFF, YBAYROFF) */
  char date_obs[24];          /* "YYYY-MM-DDThh:mm:ss..." as given, or "" */
  char object[64], telescop[64], instrume[64], filter[32], imagetyp[32], creator[64];
  double exptime;             /* seconds, 0 if not given */
  double focal;               /* mm, 0 if not given */
  double ccdtemp, gain;       /* if has_temp, has_gain */
  int has_temp, has_gain;
  int iso;                    /* ISOSPEED, 0 if not given */
  int stacked;                /* frames stacked (STACKCNT, NCOMBINE), 0 if not said */
  int geo, lat, lon;          /* SITELAT, SITELONG: GEO_POS, 1e-7 degrees */
  double datamin, datamax;    /* if has_range */
  int has_range;
} fits_hdr;

/* 1 if buf, the file's start, is a FITS file ("SIMPLE  =" ... T). */
int fits_is (const unsigned char *buf, size_t len);
/* 1 if the name ends in a FITS extension (fits, fit, fts). */
int fits_has_extension (const char *name);
/* Reads the header of f (any position: it is rewound) and finds the
   picture. 0, or -1 with a message for people in err. */
int fits_read_header (FILE *f, fits_hdr *h, char *err, size_t n);
/* "16-bit integers, colour mosaic (RGGB)", "32-bit floating point, RGB"... */
void fits_encoding (const fits_hdr *h, char *out, size_t n);
/* The details Lumen keeps: size, date taken ("YYYY:MM:DD hh:mm:ss"), the
   camera (INSTRUME), exposure, ISO, focal length and position. */
void fits_to_info (const fits_hdr *h, img_info *in);

/* Reads the picture a row at a time as 8-bit RGB, top row first,
   stretched for the screen. half: a colour mosaic is read as one pixel
   for each 2 x 2 (quicker, for pictures shown smaller than half size);
   fits_reader_size gives the size of what it reads. */
typedef struct fits_reader fits_reader;
fits_reader *fits_reader_open (FILE *f, const fits_hdr *h, int half, char *err, size_t n);
void fits_reader_size (const fits_reader *r, int *w, int *h);
/* Row y (0 the top) into rgb (w * 3 bytes). Quickest taken in order.
   0, or -1 if it couldn't be read. */
int fits_reader_row (fits_reader *r, int y, unsigned char *rgb);
/* How it is being shown: 1 stretched (the picture is linear), 0 as it is. */
int fits_reader_stretched (const fits_reader *r);
void fits_reader_free (fits_reader *r);

/* The auto-stretch for a channel whose (0-1) median and MAD are med and
   mad: the shadows clip *c0 and the midtones balance *m; 0 if it should
   be stretched, -1 if it shouldn't (already bright, or flat). */
int fits_stretch_params (double med, double mad, double *c0, double *m);
/* The midtones transfer function: x (0-1) with midtones balance m. */
double fits_mtf (double m, double x);

#endif
