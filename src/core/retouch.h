/* Copyright (c) 2026 Andrew Youll
   Released under the MIT licence: see the LICENSE file.
   SPDX-License-Identifier: MIT */
/* retouch: the recipe's spots and masks (recipe.h) on a picture, before
   geometry, on the photo as it is turned upright.

   Each spot fills its shape (a circle, or the brush along a painted path)
   from the same shape at its source: cloned, the source's pixels as they
   are; healed (repair), the source's texture with the colour and light of
   the spot's own surroundings, seamlessly. Healing finds how much lighter
   or darker and of what colour the surroundings are than the source's,
   all round the shape's edge, and spreads that difference smoothly through
   the inside (the smoothest field that matches it at the edge: a membrane,
   solved on a grid of up to 96 cells across), so light grading across the
   spot carries on through it and no seam is left. The outer part of the
   brush (the spot's feather) fades into the photo.

   Spots apply in order: one may take its source from where another has
   already been painted out. Then the masks, in order, in linear light:
   each mask's coverage (its shape, its strokes added at the most any
   gives and erased) is found on a grid of up to 2048 cells across, and
   its adjustments are made at each pixel as much as it covers it:
   exposure, temperature and tint as gains; contrast, highlights,
   shadows, clarity (detail against the surroundings, about 1.5% of the
   longer side) and sharpness (against the next pixels) on the lightness
   as seen; then saturation round the luminance. */
#ifndef LUMEN_RETOUCH_H
#define LUMEN_RETOUCH_H
#include <stdint.h>
#include "recipe.h"

/* In place: 8-bit RGB (sRGB, as decoded) or 16-bit linear RGB, w x h. */
void retouch_rgb8 (unsigned char *rgb, int w, int h, const recipe *r);
void retouch_lin (uint16_t *lin, int w, int h, const recipe *r);

/* As retouch_lin, then mask `show` (if >= 0) shown over the picture in
   red, as much as it covers it (the Viewer's Show mask). */
void retouch_lin_show (uint16_t *lin, int w, int h, const recipe *r, int show);

/* Part of that: the spots (if spots), masks m0..m1-1, then mask show (if
   >= 0) in red. The Viewer keeps a copy with the layers below the one
   being dragged, and while it's dragged does only that one and those
   above it. */
void retouch_lin_part (uint16_t *lin, int w, int h, const recipe *r, int spots, int m0, int m1, int show);

/* A source for a spot at (x, y), radius r (as recipe_spot): the place
   round it whose surroundings look most like the spot's own. For a
   painted one, along n points (xy, fractions): the offset of its source
   (dx, dy, fractions). */
void retouch_auto_source (const uint16_t *lin, int w, int h, float x, float y, float r, float *sx, float *sy);
void retouch_auto_offset (const uint16_t *lin, int w, int h, const float *xy, int n, float r, float *dx, float *dy);

/* How much mask m covers a w x h photo at (u, v) (fractions), 0..1. */
float retouch_mask_at (const recipe *r, int m, int w, int h, float u, float v);

#endif
