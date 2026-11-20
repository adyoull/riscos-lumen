/* Copyright (c) 2026 Andrew Youll
   Released under the MIT licence: see the LICENSE file.
   SPDX-License-Identifier: MIT */
/* astro: lining up frames of the night sky, for stacking.

   1. astro_find_stars finds the stars in a frame's (linear) brightness:
      it models the sky (a coarse grid of medians, so a gradient from
      light pollution doesn't matter), measures its noise (the median
      absolute deviation), and keeps the local peaks well above both
      that are shaped like stars (not single hot pixels, not big blobs),
      each measured to a fraction of a pixel by the centre of its light.
   2. astro_match pairs one frame's stars with the reference frame's: the
      triangles made by the brightest stars, described by the ratios of
      their sides (which a shift and a turn don't change), vote for pairs
      of stars; then RANSAC finds the shift and turn (and a scale very
      near 1) that most stars agree with, refined by least squares over
      all of them.
   3. astro_warp moves a frame onto the reference (bicubic, kept within
      its neighbours' range so stars don't ring).

   Coordinates are pixels of the full-size frame, (0, 0) the centre of
   the top-left pixel, y down. A transform maps the reference's pixel
   (x, y) to the frame's: x' = a x - b y + tx, y' = b x + a y + ty. */
#ifndef LUMEN_ASTRO_H
#define LUMEN_ASTRO_H
#include <stddef.h>

typedef struct
{
  float x, y;                 /* the centre of its light */
  float flux;                 /* its light above the sky */
  float peak;                 /* its brightest pixel above the sky */
  float fwhm;                 /* its width, pixels */
} astro_star;

typedef struct
{
  double a, b, tx, ty;        /* see above: a = s cos r, b = s sin r */
} astro_xform;

/* A brightness plane (linear, any scale), w x h, rows top first. */
typedef struct
{
  int w, h;
  const float *px;
  int scale;                  /* its pixels are this many of the frame's (2: a half-size plane) */
} astro_plane;

typedef struct
{
  float sky, noise;           /* the sky's median level and its noise (1.4826 MAD), of the plane */
  int found;                  /* stars kept */
  int rejected_hot;           /* peaks too sharp to be stars (hot pixels, cosmic rays) */
} astro_stats;

/* The stars in p, brightest first, at most max. Returns how many, or -1
   if out of memory. sigma: how many times the noise above the sky a
   peak must be (0: 5). */
int astro_find_stars (const astro_plane *p, double sigma, astro_star *out, int max, astro_stats *st);

/* The same a little at a time (for the desktop): begin, step (about
   rows rows' work each time; 1 when all done), end (the stars, as
   astro_find_stars), free. The plane must stay as it is till the end. */
typedef struct astro_finder astro_finder;
astro_finder *astro_find_begin (const astro_plane *p, double sigma);
int astro_find_step (astro_finder *f, int rows);
int astro_find_end (astro_finder *f, astro_star *out, int max, astro_stats *st);
void astro_find_free (astro_finder *f);

typedef struct
{
  int pairs;                  /* stars that agree with the transform */
  double rms;                 /* their distance from where it puts them, pixels */
  double scale, angle;        /* the transform's (angle in degrees, anticlockwise as shown) */
} astro_fit;

/* The transform from the reference's stars to a frame's. 0 when found
   (at least 6 stars agreeing, or all of them if fewer than 6 each),
   -1 if the frames can't be lined up. */
int astro_match (const astro_star *ref, int nref, const astro_star *frm, int nfrm, astro_xform *t, astro_fit *fit);

/* From points picked by hand: ref[i] in the reference is frm[i] in the
   frame. One pair gives a shift; two or more a shift and turn (least
   squares). 0, or -1 if n < 1 or the points coincide. */
int astro_xform_from_pairs (const double *ref_xy, const double *frm_xy, int n, astro_xform *t);

void astro_apply (const astro_xform *t, double x, double y, double *ox, double *oy);
astro_xform astro_identity (void);

/* A frame of ch channels (1 or 3, interleaved), w x h, linear floats. */
typedef struct
{
  int w, h, ch;
  float *px;
  int y0, rows;               /* px holds rows y0 to y0 + rows - 1 only (rows 0: all h) */
} astro_img;

/* dst (the reference's size, already allocated) from src moved by t
   (src may hold only some of its rows: those the band needs):
   each of dst's pixels is src's at t(x, y), bicubic, kept within its
   four nearest neighbours' range. Pixels that fall outside src are NAN
   (left out when stacking). rows: dst's rows y0 to y1 - 1 only (a band,
   for working a little at a time). */
void astro_warp (const astro_img *src, const astro_xform *t, astro_img *dst, int y0, int y1);

#endif
