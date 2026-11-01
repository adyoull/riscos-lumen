/* develop: applies a recipe to pixels.

   The working image is 16-bit linear RGB (0-65535 = 0.0-1.0 linear light,
   sRGB primaries for now). Everything per channel - white balance,
   exposure, levels, contrast - is folded into one lookup table per channel
   when the recipe changes (develop_prepare), so each pixel costs three
   table lookups, an optional saturation step, and three more lookups to
   reach sRGB for the screen. Rendering goes a band of rows at a time so
   the desktop never waits.

   Order (the spec's stages 2, 4 and 5): white balance and exposure in
   linear light; levels and contrast on the sRGB-encoded value (as a
   person sees it); back to linear; saturation and vibrancy in linear. */
#ifndef LUMEN_DEVELOP_H
#define LUMEN_DEVELOP_H
#include <stdint.h>
#include "recipe.h"
#include "thumbgen.h"

#define DEV_LUT_BITS 12
#define DEV_LUT_SIZE (1 << DEV_LUT_BITS)

typedef struct
{
  uint16_t tone[3][DEV_LUT_SIZE];      /* linear in (top 12 bits) to linear out, per channel */
  int sat;                             /* saturation, 65536 = unchanged */
  int vib;                             /* vibrancy, 65536 = unchanged */
  int identity;                        /* nothing to do between the two conversions */
} develop;

/* The shared conversions (built on first use). */
const uint16_t *dev_srgb_to_linear (void);   /* 256 entries */
const uint8_t *dev_linear_to_srgb (void);    /* 65536 entries */

void develop_prepare (develop *d, const recipe *r);

/* 8-bit sRGB RGB (as decoded) to 16-bit linear RGB. */
void dev_to_linear (const unsigned char *rgb, uint16_t *lin, size_t pixels);

/* The output's histogram: red, green, blue and luminance, one bin per
   8-bit level. */
#define DEV_HIST_BINS 256
enum { HIST_R, HIST_G, HIST_B, HIST_LUM };
typedef struct { unsigned c[4][DEV_HIST_BINS]; } dev_hist;

/* Rows y0 to y1-1 of a w-pixel-wide linear image to 32bpp sprite pixels
   (&00BBGGRR), counting them into hist if not NULL. */
void develop_rows (const develop *d, const uint16_t *src, int w, int y0, int y1,
                   uint32_t *dst, dev_hist *hist);

/* In place, on an 8-bit sRGB image (thumbnails): tone only. */
void develop_rgb8 (const develop *d, rgb_image *img);

/* The whole recipe, geometry included, on an 8-bit sRGB image; may change
   its size (img->rgb is replaced). 0 on success. */
int develop_image (const recipe *r, rgb_image *img);

#endif
