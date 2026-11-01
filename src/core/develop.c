/* develop.c: see develop.h. Float maths only when building tables;
   the per-pixel work is integer. */
#include <math.h>
#include <string.h>
#include <stdlib.h>
#include "develop.h"
#include "geometry.h"

static uint16_t s2l[256];
static uint8_t l2s[65536];
static int recip[256];                 /* 65536 / (i + 1): vibrancy's relative spread without dividing */
static int tables_made;

static double srgb_decode (double v)  /* sRGB-encoded 0-1 to linear */
{
  return v <= 0.04045 ? v / 12.92 : pow ((v + 0.055) / 1.055, 2.4);
}

static double srgb_encode (double v)  /* linear 0-1 to sRGB-encoded */
{
  return v <= 0.0031308 ? v * 12.92 : 1.055 * pow (v, 1 / 2.4) - 0.055;
}

static void make_tables (void)
{
  int i;
  if (tables_made)
    return;
  for (i = 0; i < 256; i++)
    s2l[i] = (uint16_t) lround (srgb_decode (i / 255.0) * 65535);
  for (i = 0; i < 65536; i++)
    l2s[i] = (uint8_t) lround (srgb_encode (i / 65535.0) * 255);
  for (i = 0; i < 256; i++)
    recip[i] = 65536 / (i + 1);
  tables_made = 1;
}

const uint16_t *dev_srgb_to_linear (void) { make_tables (); return s2l; }
const uint8_t *dev_linear_to_srgb (void) { make_tables (); return l2s; }

void dev_to_linear (const unsigned char *rgb, uint16_t *lin, size_t pixels)
{
  size_t i;
  make_tables ();
  for (i = 0; i < pixels * 3; i++)
    lin[i] = s2l[rgb[i]];
}

static double clamp01 (double v)
{
  return v < 0 ? 0 : v > 1 ? 1 : v;
}

void develop_prepare (develop *d, const recipe *r)
{
  double t = r->v[P_TEMP] / 100.0, n = r->v[P_TINT] / 100.0;
  double gain = pow (2.0, r->v[P_EV]);
  double mul[3], black = r->v[P_BLACK] / 100.0, white = r->v[P_WHITE] / 100.0;
  double gamma = pow (2.0, -r->v[P_MID] / 100.0);   /* +100: midtones a stop-ish brighter */
  double k = r->v[P_CONTRAST] / 100.0;
  int c, i;
  make_tables ();
  /* Warmer: more red, less blue. Tint: magenta (+) takes green down.
     Scaled so mid grey keeps its luminance. */
  mul[0] = 1 + 0.30 * t + 0.05 * n;
  mul[1] = 1 - 0.20 * n;
  mul[2] = 1 - 0.30 * t + 0.05 * n;
  {
    double y = 0.2126 * mul[0] + 0.7152 * mul[1] + 0.0722 * mul[2];
    for (c = 0; c < 3; c++)
      mul[c] /= y;
  }
  if (white - black < 0.05)
    white = black + 0.05;
  for (c = 0; c < 3; c++)
    for (i = 0; i < DEV_LUT_SIZE; i++)
      {
        /* Each entry is sampled at the start of its range, so black (0)
           and white (the last entry) are exact. */
        double x = (double) i / (DEV_LUT_SIZE - 1), p;
        x *= mul[c] * gain;
        p = srgb_encode (clamp01 (x));
        p = clamp01 ((p - black) / (white - black));
        if (gamma != 1.0)
          p = pow (p, gamma);
        if (k != 0)
          {
            /* An S-curve about the middle: smoothstep, blended by k
               (negative k flattens towards a straight line and past it). */
            double s = p * p * (3 - 2 * p);
            p = clamp01 (p + k * (s - p));
          }
        d->tone[c][i] = (uint16_t) lround (srgb_decode (p) * 65535);
      }
  d->sat = (int) lround (65536 * (1 + r->v[P_SATURATION] / 100.0));
  d->vib = (int) lround (65536 * (r->v[P_VIBRANCE] / 100.0));
  /* Identity: every table entry maps its bin to its own middle. */
  {
    recipe look = *r;
    int p;
    for (p = P_FIRST_GEOMETRY; p < P_COUNT; p++)   /* geometry is not develop's business */
      look.v[p] = recipe_params[p].def;
    d->identity = recipe_is_default (&look);
  }
}

/* One pixel: tone tables, then saturation and vibrancy around luminance.
   All 32-bit: the factor is in 1/256ths (at most 768) and a difference
   from luminance at most 65535, so the products stay under 2^26. No
   division: vibrancy weighs a colour by its channels' spread relative to
   the brightest, through a reciprocal table. */
static inline void pixel (const develop *d, const uint16_t *s, int *r, int *g, int *b)
{
  int R = d->tone[0][s[0] >> (16 - DEV_LUT_BITS)];
  int G = d->tone[1][s[1] >> (16 - DEV_LUT_BITS)];
  int B = d->tone[2][s[2] >> (16 - DEV_LUT_BITS)];
  if (d->sat != 65536 || d->vib != 0)
    {
      /* Rec. 709 luminance; unsigned: 65535 * 65536 only just fits 32 bits. */
      int Y = (int) (((unsigned) R * 13933u + (unsigned) G * 46871u + (unsigned) B * 4732u) >> 16);
      int f = d->sat >> 8;
      if (d->vib)
        {
          int mx = R > G ? (R > B ? R : B) : (G > B ? G : B);
          int mn = R < G ? (R < B ? R : B) : (G < B ? G : B);
          int spread = (((mx - mn) >> 8) * recip[mx >> 8]) >> 8;   /* 0-256: 256 is pure colour */
          if (spread > 256)
            spread = 256;
          f += ((d->vib >> 8) * (256 - spread)) >> 8;
        }
      R = Y + (((R - Y) * f) >> 8);
      G = Y + (((G - Y) * f) >> 8);
      B = Y + (((B - Y) * f) >> 8);
      R = R < 0 ? 0 : R > 65535 ? 65535 : R;
      G = G < 0 ? 0 : G > 65535 ? 65535 : G;
      B = B < 0 ? 0 : B > 65535 ? 65535 : B;
    }
  *r = R; *g = G; *b = B;
}

void develop_rows (const develop *d, const uint16_t *src, int w, int y0, int y1,
                   uint32_t *dst, unsigned hist[64])
{
  int y, x;
  make_tables ();
  for (y = y0; y < y1; y++)
    {
      const uint16_t *s = src + (size_t) y * w * 3;
      uint32_t *o = dst + (size_t) y * w;
      if (d->identity)
        for (x = 0; x < w; x++, s += 3)
          {
            unsigned r = l2s[s[0]], g = l2s[s[1]], b = l2s[s[2]];
            o[x] = r | g << 8 | b << 16;
            if (hist)
              hist[(r * 54 + g * 183 + b * 19) >> 10]++;
          }
      else
        for (x = 0; x < w; x++, s += 3)
          {
            int R, G, B;
            unsigned r, g, b;
            pixel (d, s, &R, &G, &B);
            r = l2s[R]; g = l2s[G]; b = l2s[B];
            o[x] = r | g << 8 | b << 16;
            if (hist)
              hist[(r * 54 + g * 183 + b * 19) >> 10]++;
          }
    }
}

int develop_image (const recipe *r, rgb_image *img)
{
  develop *d = malloc (sizeof *d);
  geom_plan g;
  uint16_t *lin = NULL, *out = NULL;
  unsigned char *rgb = NULL;
  size_t i, n;
  if (!d)
    return -1;
  develop_prepare (d, r);
  if (recipe_geometry_is_default (r))
    {
      develop_rgb8 (d, img);
      free (d);
      return 0;
    }
  geom_plan_make (&g, r, img->w, img->h, 1, 0, 0);
  n = (size_t) g.ow * g.oh;
  lin = malloc ((size_t) img->w * img->h * 6);
  out = malloc (n * 6);
  rgb = malloc (n * 3);
  if (!lin || !out || !rgb)
    {
      free (d); free (lin); free (out); free (rgb);
      return -1;
    }
  dev_to_linear (img->rgb, lin, (size_t) img->w * img->h);
  geom_rows (&g, lin, out, 0, g.oh);
  for (i = 0; i < n; i++)
    {
      int R, G, B;
      if (d->identity)
        {
          R = out[3 * i]; G = out[3 * i + 1]; B = out[3 * i + 2];
        }
      else
        pixel (d, out + 3 * i, &R, &G, &B);
      rgb[3 * i] = l2s[R]; rgb[3 * i + 1] = l2s[G]; rgb[3 * i + 2] = l2s[B];
    }
  free (img->rgb);
  img->rgb = rgb;
  img->w = g.ow;
  img->h = g.oh;
  free (lin); free (out); free (d);
  return 0;
}

void develop_rgb8 (const develop *d, rgb_image *img)
{
  size_t i, n = (size_t) img->w * img->h;
  make_tables ();
  if (d->identity)
    return;
  for (i = 0; i < n; i++)
    {
      unsigned char *p = img->rgb + 3 * i;
      uint16_t s[3];
      int R, G, B;
      s[0] = s2l[p[0]]; s[1] = s2l[p[1]]; s[2] = s2l[p[2]];
      pixel (d, s, &R, &G, &B);
      p[0] = l2s[R]; p[1] = l2s[G]; p[2] = l2s[B];
    }
}
