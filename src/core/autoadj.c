/* Copyright (c) 2026 Andrew Youll
   Released under the MIT licence: see the LICENSE file.
   SPDX-License-Identifier: MIT */
/* autoadj.c: see autoadj.h. The picture is sampled on a grid (about
   60,000 pixels), so it takes the same time for any size. */
#include <math.h>
#include <stdlib.h>
#include <string.h>
#include "autoadj.h"

#define SAMPLES 60000
#define WB_DAMP 0.75                   /* auto white balance goes three quarters of the way */
#define EV_DAMP 0.7
#define WB_MAX_TEMP 60                 /* auto never goes further than this... */
#define WB_MAX_TINT 25                 /* ...or this: tint casts are rarely large */
#define LEVELS_SHARE 0.5               /* levels close half the gap to the ends */

/* develop.c's channel gains for temperature t and tint n (-1..1). */
static void gains (double t, double n, double m[3])
{
  m[0] = 1 + 0.30 * t + 0.05 * n;
  m[1] = 1 - 0.20 * n;
  m[2] = 1 - 0.30 * t + 0.05 * n;
  {
    double y = 0.2126 * m[0] + 0.7152 * m[1] + 0.0722 * m[2];
    m[0] /= y; m[1] /= y; m[2] /= y;
  }
}

static double clampd (double v, double lo, double hi)
{
  return v < lo ? lo : v > hi ? hi : v;
}

int wb_neutral (double R, double G, double B, float *temp, float *tint)
{
  /* R*mR = G*mG and B*mB = G*mG, linear in t and n:
       0.3R t + (0.05R + 0.2G) n = G - R
      -0.3B t + (0.05B + 0.2G) n = G - B */
  double a = 0.3 * R, b = 0.05 * R + 0.2 * G, e = G - R;
  double c = -0.3 * B, d = 0.05 * B + 0.2 * G, f = G - B;
  double det = a * d - b * c, t, n;
  *temp = 0;
  *tint = 0;
  if (R <= 0 || G <= 0 || B <= 0 || fabs (det) < 1e-12)
    return -1;
  t = (e * d - b * f) / det;
  n = (a * f - e * c) / det;
  *temp = (float) clampd (t * 100, -100, 100);
  *tint = (float) clampd (n * 100, -100, 100);
  return 0;
}

void wb_sample (const uint16_t *lin, int w, int h, int cx, int cy, int radius, double rgb[3])
{
  int x, y, c, n = 0;
  double s[3] = { 0, 0, 0 };
  for (y = cy - radius; y <= cy + radius; y++)
    for (x = cx - radius; x <= cx + radius; x++)
      {
        const uint16_t *p;
        if (x < 0 || y < 0 || x >= w || y >= h)
          continue;
        p = lin + ((size_t) y * w + x) * 3;
        for (c = 0; c < 3; c++)
          s[c] += p[c];
        n++;
      }
  for (c = 0; c < 3; c++)
    rgb[c] = n ? s[c] / n / 65535.0 : 0;
}

static double encode (double v)        /* linear to sRGB-encoded, 0-1 */
{
  v = clampd (v, 0, 1);
  return v <= 0.0031308 ? v * 12.92 : 1.055 * pow (v, 1 / 2.4) - 0.055;
}

static double decode (double v)
{
  return v <= 0.04045 ? v / 12.92 : pow ((v + 0.055) / 1.055, 2.4);
}

/* Visits about SAMPLES pixels on a grid. */
#define FOR_SAMPLES(lin, w, h, p)                                              \
  for (int sy_ = 0, step_ = (int) sqrt ((double) (w) * (h) / SAMPLES) + 1; sy_ < (h); sy_ += step_) \
    for (int sx_ = 0; sx_ < (w); sx_ += step_)                                \
      for (const uint16_t *p = (lin) + ((size_t) sy_ * (w) + sx_) * 3; p; p = NULL)

void auto_white_balance (const uint16_t *lin, int w, int h, float *temp, float *tint)
{
  double neutral[3] = { 0, 0, 0 }, all[3] = { 0, 0, 0 };
  long nn = 0, na = 0;
  float t, n;
  *temp = *tint = 0;
  FOR_SAMPLES (lin, w, h, p)
    {
      double r = p[0] / 65535.0, g = p[1] / 65535.0, b = p[2] / 65535.0;
      double mx = r > g ? (r > b ? r : b) : (g > b ? g : b);
      double mn = r < g ? (r < b ? r : b) : (g < b ? g : b);
      double y = 0.2126 * r + 0.7152 * g + 0.0722 * b;
      if (y < 0.01 || mx > 0.98)       /* too dark to judge, or clipped */
        continue;
      all[0] += r; all[1] += g; all[2] += b;
      na++;
      if ((mx - mn) / mx < 0.20)       /* near-neutral: what should be grey */
        {
          /* Brighter greys count for more: whites and light greys are
             the most trustworthy (shadows pick up coloured bounce light). */
          neutral[0] += r * y; neutral[1] += g * y; neutral[2] += b * y;
          nn++;
        }
    }
  if (na == 0)
    return;
  /* Enough near-neutral pixels (5% of those judged): use them; else the
     whole picture's average (grey world). */
  if (nn * 20 >= na)
    wb_neutral (neutral[0], neutral[1], neutral[2], &t, &n);
  else
    wb_neutral (all[0], all[1], all[2], &t, &n);
  *temp = roundf ((float) clampd (t * WB_DAMP, -WB_MAX_TEMP, WB_MAX_TEMP));
  *tint = roundf ((float) clampd (n * WB_DAMP, -WB_MAX_TINT, WB_MAX_TINT));
}

static int cmp_double (const void *a, const void *b)
{
  double x = *(const double *) a, y = *(const double *) b;
  return x < y ? -1 : x > y;
}

void auto_adjust (const uint16_t *lin, int w, int h, recipe *r)
{
  float t, n;
  double m[3], *lum, chroma = 0, gain, p_lo, p_hi, p5, p95, med, spread;
  long cnt = 0, cap = SAMPLES * 2 + 64;
  int p;
  for (p = 0; p < P_SHARPEN; p++)       /* start from the original tones (sharpening and geometry stay) */
    if (p < P_TONE_SH_HUE || p > P_TONE_HI)   /* and so does toning: a choice of look, not a correction */
    r->v[p] = recipe_params[p].def;
  auto_white_balance (lin, w, h, &t, &n);
  recipe_set (r, P_TEMP, t);
  recipe_set (r, P_TINT, n);
  gains (r->v[P_TEMP] / 100.0, r->v[P_TINT] / 100.0, m);

  lum = malloc (sizeof *lum * (size_t) cap);
  if (!lum)
    return;
  FOR_SAMPLES (lin, w, h, px)
    {
      double R = px[0] / 65535.0 * m[0], G = px[1] / 65535.0 * m[1], B = px[2] / 65535.0 * m[2];
      double mx = R > G ? (R > B ? R : B) : (G > B ? G : B);
      double mn = R < G ? (R < B ? R : B) : (G < B ? G : B);
      if (cnt < cap)
        lum[cnt++] = 0.2126 * R + 0.7152 * G + 0.0722 * B;
      if (mx > 0.02)
        chroma += (mx - mn) / mx;
    }
  if (cnt == 0)
    {
      free (lum);
      return;
    }
  chroma /= cnt;
  qsort (lum, (size_t) cnt, sizeof *lum, cmp_double);
  med = lum[cnt / 2];

  /* Exposure: the median towards mid grey (18% light, sRGB 118), damped,
     and gentler upwards so dark scenes stay dark. */
  gain = 0;
  if (med > 1e-4)
    gain = log2 (0.18 / med) * EV_DAMP;
  gain = clampd (gain, -2, 1.5);
  recipe_set (r, P_EV, (float) gain);
  gain = pow (2, r->v[P_EV]);

  /* Levels from the exposed picture's ends: 0.5% and 99.5%, as seen. */
  p_lo = encode (lum[(long) (cnt * 0.005)] * gain);
  p_hi = encode (lum[(long) (cnt * 0.995)] * gain);
  if (p_lo > 0.02)
    recipe_set (r, P_BLACK, (float) clampd (p_lo * 100 * LEVELS_SHARE, 0, 6));
  if (p_hi < 0.97)
    recipe_set (r, P_WHITE, (float) clampd (100 - (100 - p_hi * 100) * LEVELS_SHARE, 92, 100));

  /* Contrast: more for flat pictures, a little for the rest. */
  p5 = encode (lum[(long) (cnt * 0.05)] * gain);
  p95 = encode (lum[(long) (cnt * 0.95)] * gain);
  {
    double b = r->v[P_BLACK] / 100.0, wpt = r->v[P_WHITE] / 100.0;
    p5 = clampd ((p5 - b) / (wpt - b), 0, 1);
    p95 = clampd ((p95 - b) / (wpt - b), 0, 1);
  }
  spread = p95 - p5;
  recipe_set (r, P_CONTRAST, (float) clampd (10 + (0.6 - spread) * 80, 0, 35));

  /* Vibrancy: dull pictures get more. */
  recipe_set (r, P_VIBRANCE, chroma < 0.15 ? 25 : chroma < 0.30 ? 15 : 5);
  free (lum);
  (void) decode;
}
