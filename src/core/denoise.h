/* denoise: noise reduction, after develop and before sharpening.

   Colour noise (blotches of colour in dark, high-ISO areas) is taken out
   by blurring the colour (chroma) alone, leaving the detail, which is in
   the luminance. Luminance noise (grain) is smoothed by averaging each
   pixel with its neighbours that are close to it in lightness, so edges,
   which differ by more, are kept (a small bilateral filter).

   Amounts are 0-100. Both are measured in pixels of the full-size
   original: a picture shown or written smaller passes its scale (noise
   already shrinks with it). */
#ifndef LUMEN_DENOISE_H
#define LUMEN_DENOISE_H
#include <stdint.h>

/* Rows a band needs above and below it. */
int denoise_margin (int lum, int col, double scale);
/* In place on w x h 32bpp pixels (&00BBGGRR). 0, or -1 if out of memory
   (the pixels are then unchanged). */
int denoise32 (uint32_t *px, int w, int h, int lum, int col, double scale);

#endif
