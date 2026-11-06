/* Copyright (c) 2026 Andrew Youll
   Released under the MIT licence: see the LICENSE file.
   SPDX-License-Identifier: MIT */
/* geometry: rotation by quarter turns, straightening, perspective
   (keystone) correction and cropping, applied to a 16-bit linear image.

   Each output pixel is mapped back to the source and sampled bilinearly:
     output -> crop (fractions of the canvas) -> zoom -> perspective
            -> straighten -> quarter turn -> lens -> source.
   The lens step (lens correction, recipe.h) undoes the lens's distortion
   (and the Manual barrel or pincushion), scaled so the corrected picture
   fills the frame with as much of the photo as it can; then red and blue
   are each taken from a little further out or in than green (lateral
   colour, which fringes edges towards the corners), and the light is
   lifted where the lens darkened it (vignetting).
   The canvas is the picture after its quarter turns. Straightening and
   perspective zoom in just enough that no empty corners show, as cameras'
   own straightening does.

   Vertical perspective (P_PERSP_V) widens the top (+) or the bottom (-) of
   the picture, correcting converging verticals; horizontal does the same
   for the left and right edges. They are keystone corrections, not a full
   projective transform. */
#ifndef LUMEN_GEOMETRY_H
#define LUMEN_GEOMETRY_H
#include <stdint.h>
#include "recipe.h"

typedef struct
{
  int sw, sh;                          /* source */
  int cw, ch;                          /* canvas: after the quarter turns */
  int ow, oh;                          /* output */
  int quarter;
  double cs, sn;                       /* straightening angle */
  double kv, kh;                       /* perspective */
  double zoom;
  double x0, y0, x1, y1;               /* crop, fractions of the canvas */
  /* lens correction (none when lens is 0) */
  int lens;
  double lcx, lcy, lhd;                /* the source's centre and half diagonal */
  double ls;                           /* corrected place -> where it is undistorted (the fill) */
  int dm;                              /* distortion model (LENS_D_*), its terms, how much */
  double d[3], damt, man;
  int tm;                              /* lateral colour */
  double t[6], tamt;
  int vm;                              /* vignetting */
  double k[3], vamt;
  double rh, rv;
} geom_plan;

/* Plans a source of sw x sh. with_crop 0 shows the whole canvas (the crop
   tool does). out_w, out_h 0: the natural size (the crop's size in canvas
   pixels); otherwise the output is that size. */
void geom_plan_make (geom_plan *g, const recipe *r, int sw, int sh, int with_crop, int out_w, int out_h);

/* Output rows y0 to y1-1 (3 channels, 16 bits). */
void geom_rows (const geom_plan *g, const uint16_t *src, uint16_t *dst, int y0, int y1);

/* Where output pixel (x, y) comes from in the source, in pixels. */
void geom_to_source (const geom_plan *g, double x, double y, double *sx, double *sy);
/* As geom_to_source, for red, green and blue (lateral colour moves red
   and blue), with the gain (1 or more) that undoes vignetting there. */
void geom_to_source_rgb (const geom_plan *g, double x, double y, double sx[3], double sy[3], double *gain);
/* Only straightening, perspective, quarter turns and crop: 1 if the plan
   does nothing but (perhaps) resize. */
int geom_plan_is_simple (const geom_plan *g);
/* The other way: where source pixel (sx, sy) is in the output (perhaps outside it). */
void geom_from_source (const geom_plan *g, double sx, double sy, double *x, double *y);

/* Fits a crop to an aspect ratio (w:h of the output; 0 = free), keeping
   its centre, inside the canvas. Canvas shape cw x ch decides fractions. */
void geom_crop_aspect (double *x0, double *y0, double *x1, double *y1, double aspect, int cw, int ch);

/* As geom_rows, but from 8-bit sRGB (export works on the full-size
   original, which would take twice the memory as 16-bit linear): output
   rows y0..y1-1 go to dst starting at its row (y - dst_row0), as linear. */
void geom_rows_rgb8 (const geom_plan *g, const unsigned char *src, const uint16_t *s2l,
                     uint16_t *dst, int y0, int y1, int dst_row0);

/* Any rectangle of the output (rw x rh from (rx, ry)), from 8-bit sRGB,
   to dst (rw pixels a row) as linear: the viewer's 100% view. */
void geom_region_rgb8 (const geom_plan *g, const unsigned char *src, const uint16_t *s2l,
                       uint16_t *dst, int rx, int ry, int rw, int rh);

#endif
