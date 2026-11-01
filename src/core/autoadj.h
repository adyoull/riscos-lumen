/* autoadj: automatic adjustments worked out from the picture itself.

   White balance uses the same model as develop.c (temperature moves red
   and blue apart, tint moves green against both), so a picked grey is made
   exactly neutral. Auto adjust looks at a sample of the picture's pixels
   and sets white balance, exposure, levels, contrast and vibrancy; it
   leaves geometry alone. It aims for a pleasant, believable result rather
   than a mathematically flat one: corrections are damped and limited, so
   a sunset stays warm and a night stays dark. */
#ifndef LUMEN_AUTOADJ_H
#define LUMEN_AUTOADJ_H
#include <stdint.h>
#include "recipe.h"

/* Temperature and tint (-100..100) that make linear colour (R, G, B)
   neutral, as far as the range allows. 0 if it could be solved. */
int wb_neutral (double R, double G, double B, float *temp, float *tint);

/* The mean linear colour of a box of pixels (the eyedropper: a 5x5 box,
   say), clipped to the picture. */
void wb_sample (const uint16_t *lin, int w, int h, int cx, int cy, int radius, double rgb[3]);

/* Automatic white balance for the whole picture: near-neutral pixels are
   averaged (grey world when there are too few), the result damped. */
void auto_white_balance (const uint16_t *lin, int w, int h, float *temp, float *tint);

/* Sets r's look (white balance, exposure, levels, contrast, saturation,
   vibrancy) from the picture; geometry is left as it is. */
void auto_adjust (const uint16_t *lin, int w, int h, recipe *r);

#endif
