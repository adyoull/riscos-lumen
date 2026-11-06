/* Copyright (c) 2026 Andrew Youll
   Released under the MIT licence: see the LICENSE file.
   SPDX-License-Identifier: MIT */
/* denoise: noise reduction, after develop and before sharpening.

   Wavelet shrinkage, as darktable and RawTherapee do it (written here
   from the method, not their code): the picture, as luminance and two
   colour differences, is split by the "a trous" wavelet transform (a
   B3-spline blur with holes, at steps of 1, 2, 4, 8 and 16 pixels) into
   layers of detail, fine to coarse, and what's left. Noise is small
   detail everywhere; edges and texture are large detail in places. So
   each detail layer is shrunk towards nothing by a threshold (soft
   thresholding): what's under it (noise) goes, what's over it (edges)
   is kept, less the threshold. Luminance (grain) uses four layers,
   colour (blotches, which are coarser) five.

   The thresholds follow how white noise spreads over the layers (most
   in the finest). Amounts are 0-100: Luminance 100 is a threshold of
   14 levels (of 255) for noise in the finest layer, Colour 100 of 20.
   They're for the full-size original: a picture shown or written smaller
   passes its scale, and its layers are matched to the original's (noise
   already shrinks with the picture). denoise_estimate measures a
   picture's noise (the finest layer's median size, which edges hardly
   move) and gives the amounts that suit it: the Auto link. */
#ifndef LUMEN_DENOISE_H
#define LUMEN_DENOISE_H
#include <stdint.h>

/* Rows a band needs above and below it. */
int denoise_margin (int lum, int col, double scale);
/* In place on w x h 32bpp pixels (&00BBGGRR). 0, or -1 if out of memory
   (the pixels are then unchanged). */
int denoise32 (uint32_t *px, int w, int h, int lum, int col, double scale);
/* The amounts (0-100) for the noise in w x h 32bpp pixels at scale. */
void denoise_estimate (const uint32_t *px, int w, int h, double scale, int *lum, int *col);

#endif
