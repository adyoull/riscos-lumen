/* sharpen: unsharp masking on luminance, the last step after develop.

   Each pixel's red, green and blue move by the same amount: the
   difference between its luminance and a gaussian blur of the
   luminance round it, times the amount. Working on luminance alone
   sharpens edges without coloured fringes. Differences of a level or
   less are left alone, so smooth skies and noise aren't roughened.

   The radius is the blur's sigma in pixels of the full-size original;
   pictures shown or written smaller pass their scale, so the effect
   looks the same at any size. */
#ifndef LUMEN_SHARPEN_H
#define LUMEN_SHARPEN_H
#include <stdint.h>
#include "recipe.h"

/* The sigma to use for r at a picture scale times the full size. */
double sharpen_sigma (const recipe *r, double scale);
/* Rows a band needs above and below it for that sigma. */
int sharpen_margin (double sigma);
/* In place on w x h 32bpp pixels (&00BBGGRR). amount in percent; 0 does
   nothing. 0, or -1 if out of memory (the pixels are then unchanged). */
int sharpen32 (uint32_t *px, int w, int h, int amount, double sigma);

#endif
