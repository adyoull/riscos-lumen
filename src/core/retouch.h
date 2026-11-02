/* retouch: the recipe's spots (recipe.h) painted out of a picture, before
   geometry, on the photo as it is turned upright.

   Each spot fills its circle from the circle at its source: cloned, the
   source's pixels as they are; healed (repair), the source's texture with
   the colour and light of the spot's own surroundings, so it blends in.
   Healing works out, at points round the edge of the circle, how much
   lighter or darker and of what colour the surroundings are than the
   source's, and adds that difference inside: a plane fitted to it (so
   light grading across the spot carries on through it), and what the
   plane leaves blended smoothly in from the edge points (inverse distance). The outer part of the
   circle (the spot's feather) fades into the photo.

   Spots apply in order: one may take its source from where another has
   already been painted out. Then the brushed adjustments (recipe.h's
   strokes), in linear light: each stroke's mask (its brush along its
   points, the most at each pixel) scales the light (dodge, burn) or the
   colour round the luminance (saturate, desaturate). */
#ifndef LUMEN_RETOUCH_H
#define LUMEN_RETOUCH_H
#include <stdint.h>
#include "recipe.h"

/* In place: 8-bit RGB (sRGB, as decoded) or 16-bit linear RGB, w x h. */
void retouch_rgb8 (unsigned char *rgb, int w, int h, const recipe *r);
void retouch_lin (uint16_t *lin, int w, int h, const recipe *r);

/* A source for a spot at (x, y), radius r (as recipe_spot): the place
   round it whose surroundings look most like the spot's own. */
void retouch_auto_source (const uint16_t *lin, int w, int h, float x, float y, float r, float *sx, float *sy);

#endif
