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
   person sees it); back to linear; saturation and vibrancy in linear;
   colour (HSL: each hue band turned, saturated and brightened, the band
   found from the colour as seen, the change made in linear light); split
   black & white (a mix of the channels, as a filter on black and white
   film would give); toning; the glow; then the vignette, by the pixel's place in the whole output. */
#ifndef LUMEN_DEVELOP_H
#define LUMEN_DEVELOP_H
#include <stdint.h>
#include "recipe.h"
#include "thumbgen.h"
#include "geometry.h"

#define DEV_LUT_BITS 12
#define DEV_LUT_SIZE (1 << DEV_LUT_BITS)

/* Highlights & Shadows are local: each pixel is brightened
   (shadows) or darkened (highlights) by how light its surroundings are,
   not by its own value, so detail and local contrast stay. The
   surroundings come from a small, blurred map of the source picture's
   lightness (before geometry), made once per photo; develop_mask turns it
   into a grid of gains over the output, through the geometry. Every
   render of a photo (viewer, 100%, loupe, thumbnail, export) uses the same
   map, so they agree. */
#define HS_MAP 64                      /* the lightness map's longer side, in cells */
#define HS_GRID 64                     /* the gain grid's longer side, over the output */

/* Glow, as an Orton effect: the bright parts' light, blurred widely, is
   laid over the picture (screen, in linear light), so highlights spill a
   soft halo round them and the picture looks luminous while its edges
   stay sharp. The light comes from the same source map as Highlights &
   Shadows, in colour at GLOW_MAP cells: each cell developed (tone, colour,
   black & white, toning), weighed by how bright it is against the
   Highlights threshold, warmed or cooled, then blurred by Spread (1% to 12%
   of the longer side) and laid through the geometry onto a grid over the
   output, as the gains are. */
#define GLOW_MAP 96
#define GLOW_GRID 96
typedef struct
{
  int w, h; float l[HS_MAP * HS_MAP];  /* lightness, blurred (Highlights & Shadows) */
  int cw, ch; uint16_t c[GLOW_MAP * GLOW_MAP * 3];   /* colour, linear, unblurred (glow) */
} dev_lummap;

typedef struct
{
  uint16_t tone[3][DEV_LUT_SIZE];      /* linear in (top 12 bits) to linear out, per channel */
  int hs;                              /* highlights or shadows set */
  float hs_hl, hs_sh;                  /* their amounts, 0-1 */
  int gw, gh;                          /* the gain grid */
  uint16_t gain[HS_GRID * HS_GRID];    /* 4096 = unchanged */
  int fw, fh, rx, ry;                  /* the rows' place in the whole output (develop_region) */
  int sat;                             /* saturation, 65536 = unchanged */
  int toning;                          /* split toning on */
  uint16_t tint[3][256];               /* its gain per channel by luminance (top 8 bits), 4096 = none */
  int vib;                             /* vibrancy, 65536 = unchanged */
  int hsl;                             /* colour (HSL) adjustments on */
  int bw;                              /* black & white: how much, 256 = all */
  int bw_w[3];                         /* its mix of red, green and blue, 1024 = 100% */
  int16_t hsl_cos[360], hsl_sin[360];  /* hue turn by hue (degrees, as seen): cos, and sin / sqrt 3, 1.14 fixed */
  uint16_t hsl_sat[360];               /* saturation by hue, 256 = unchanged */
  uint16_t hsl_lum[360];               /* brightness by hue, 4096 = unchanged */
  int glow;                            /* glow's amount, 256 = all; 0 off */
  float glow_sigma, glow_high, glow_warm;   /* spread (a share of the longer side), threshold 0-1, warmth -1..1 */
  int ggw, ggh;                        /* the glow grid */
  uint16_t glow_px[GLOW_GRID * GLOW_GRID * 3];   /* its light, linear, per channel */
  int vig;                             /* vignette on */
  uint16_t vig_gain[257];              /* gain by squared distance from the centre (256: a corner), 4096 = none */
  int identity;                        /* nothing to do between the two conversions */
} develop;

/* The shared conversions (built on first use). */
const uint16_t *dev_srgb_to_linear (void);   /* 256 entries */
const uint8_t *dev_linear_to_srgb (void);    /* 65536 entries */

void develop_prepare (develop *d, const recipe *r);

/* Highlights & Shadows, and the glow. develop_prepare leaves the gains at
   1 and no glow; when develop_needs_mask, make the source's map (8-bit sRGB or 16-bit linear,
   before geometry) and give it with the geometry plan the output comes
   from (NULL: none, the output is the source). develop_region says where
   the rows develop_rows gets sit in the whole fw x fh output (default:
   they are it). */
int develop_needs_mask (const develop *d);
void dev_lummap_rgb8 (dev_lummap *m, const unsigned char *rgb, int w, int h);
void dev_lummap_lin (dev_lummap *m, const uint16_t *lin, int w, int h);
void develop_mask (develop *d, const dev_lummap *m, const geom_plan *g, int src_w, int src_h);
void develop_region (develop *d, int fw, int fh, int x0, int y0);

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
