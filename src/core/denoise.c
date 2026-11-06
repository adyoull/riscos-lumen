/* Copyright (c) 2026 Andrew Youll
   Released under the MIT licence: see the LICENSE file.
   SPDX-License-Identifier: MIT */
/* denoise.c: see denoise.h. */
#include <math.h>
#include <stdlib.h>
#include <string.h>
#include "denoise.h"
#include "simd.h"

#define LUM_LAYERS 4
#define COL_LAYERS 5
#define LUM_MAX 14.0                   /* Luminance 100: the finest layer's threshold, levels */
#define COL_MAX 20.0

/* White noise of 1 in each layer of the B3-spline a trous transform. */
static const double layer_noise[7] = { 0.890, 0.201, 0.086, 0.042, 0.021, 0.010, 0.005 };

/* Layer j's threshold at a scale: noise in a picture made smaller is
   averaged down by the scale (and then spreads over the layers as white
   noise does). */
static double threshold (double finest, int j, double scale)
{
  if (scale > 1)
    scale = 1;
  return finest * scale * layer_noise[j] / layer_noise[0];
}

/* The layers worth doing (a threshold of at least a tenth of a level). */
static int layers (double finest, int most, double scale)
{
  int n = 0;
  while (n < most && threshold (finest, n, scale) >= 0.1)
    n++;
  return n;
}

static int reach (int n)
{
  return n > 0 ? 2 * ((1 << n) - 1) : 0;
}

int denoise_margin (int lum, int col, double scale)
{
  int a = lum > 0 ? reach (layers (LUM_MAX * lum / 100, LUM_LAYERS, scale)) : 0;
  int b = col > 0 ? reach (layers (COL_MAX * col / 100, COL_LAYERS, scale)) : 0;
  return a > b ? a : b;
}

#if defined(__arm__)
#pragma GCC push_options
#pragma GCC target ("fpu=neon")
#include <arm_neon.h>
/* With NEON, 4 at a time, the same sums in the same order (no fused
   multiply-adds), so the same results. Each returns how far it got. */
static int row_neon (const float *r, float *o, int w, int s)
{
  int x = 2 * s;
  float32x4_t four = vdupq_n_f32 (4), six = vdupq_n_f32 (6), sixteenth = vdupq_n_f32 (1.0f / 16);
  for (; x + 4 <= w - 2 * s; x += 4)
    {
      float32x4_t a = vld1q_f32 (r + x - 2 * s), b = vld1q_f32 (r + x - s), c = vld1q_f32 (r + x + s);
      float32x4_t d = vld1q_f32 (r + x + 2 * s), m = vld1q_f32 (r + x);
      float32x4_t t = vaddq_f32 (vaddq_f32 (vaddq_f32 (a, d), vmulq_f32 (four, vaddq_f32 (b, c))), vmulq_f32 (six, m));
      vst1q_f32 (o + x, vmulq_f32 (t, sixteenth));
    }
  return x;
}

static int col_neon (const float *ra, const float *rb, const float *rm, const float *rc, const float *rd, float *o, int w)
{
  int x = 0;
  float32x4_t four = vdupq_n_f32 (4), six = vdupq_n_f32 (6), sixteenth = vdupq_n_f32 (1.0f / 16);
  for (; x + 4 <= w; x += 4)
    {
      float32x4_t t = vaddq_f32 (vaddq_f32 (vaddq_f32 (vld1q_f32 (ra + x), vld1q_f32 (rd + x)),
                                            vmulq_f32 (four, vaddq_f32 (vld1q_f32 (rb + x), vld1q_f32 (rc + x)))),
                                 vmulq_f32 (six, vld1q_f32 (rm + x)));
      vst1q_f32 (o + x, vmulq_f32 (t, sixteenth));
    }
  return x;
}

static size_t shrink_neon (const float *cur, const float *next, float *acc, size_t n, float th)
{
  size_t i = 0;
  float32x4_t t = vdupq_n_f32 (th), nt = vdupq_n_f32 (-th), zero = vdupq_n_f32 (0);
  for (; i + 4 <= n; i += 4)
    {
      float32x4_t d = vsubq_f32 (vld1q_f32 (cur + i), vld1q_f32 (next + i));
      /* d - th over th, d + th under -th, else 0 */
      float32x4_t up = vsubq_f32 (d, t), down = vaddq_f32 (d, t);
      float32x4_t k = vbslq_f32 (vcgtq_f32 (d, t), up, vbslq_f32 (vcltq_f32 (d, nt), down, zero));
      vst1q_f32 (acc + i, vaddq_f32 (vld1q_f32 (acc + i), k));
    }
  return i;
}
#pragma GCC pop_options
#endif

/* One a trous smoothing at step s: the B3 spline (1 4 6 4 1)/16 with
   holes, rows then columns, edges held. in -> out (tmp: w x h). */
static void smooth (const float *in, float *out, float *tmp, int w, int h, int s)
{
  int x, y, neon = 0;
#if defined(__arm__)
  neon = simd_neon ();
#endif
  (void) neon;
  for (y = 0; y < h; y++)
    {
      const float *r = in + (size_t) y * w;
      float *o = tmp + (size_t) y * w;
      int from = w, to = w;
#if defined(__arm__)
      if (neon && w > 4 * s + 4)
        {
          from = 2 * s;
          to = row_neon (r, o, w, s);
        }
#endif
      for (x = 0; x < w; x++)
        {
          int a, b, c, d;
          if (x == from)
            x = to;
          if (x >= w)
            break;
          a = x - 2 * s; b = x - s; c = x + s; d = x + 2 * s;
          a = a < 0 ? 0 : a; b = b < 0 ? 0 : b; c = c >= w ? w - 1 : c; d = d >= w ? w - 1 : d;
          o[x] = (r[a] + r[d] + 4 * (r[b] + r[c]) + 6 * r[x]) * (1.0f / 16);
        }
    }
  for (y = 0; y < h; y++)
    {
      int a = y - 2 * s, b = y - s, c = y + s, d = y + 2 * s;
      const float *ra, *rb, *rc, *rd, *rm = tmp + (size_t) y * w;
      float *o = out + (size_t) y * w;
      a = a < 0 ? 0 : a; b = b < 0 ? 0 : b; c = c >= h ? h - 1 : c; d = d >= h ? h - 1 : d;
      ra = tmp + (size_t) a * w; rb = tmp + (size_t) b * w; rc = tmp + (size_t) c * w; rd = tmp + (size_t) d * w;
      x = 0;
#if defined(__arm__)
      if (neon)
        x = col_neon (ra, rb, rm, rc, rd, o, w);
#endif
      for (; x < w; x++)
        o[x] = (ra[x] + rd[x] + 4 * (rb[x] + rc[x]) + 6 * rm[x]) * (1.0f / 16);
    }
}

/* One channel (int16, w x h) through n_layers with the finest threshold
   `finest`: each layer's detail soft-thresholded and kept, added to the
   coarse rest at the end. bufs: four of w x h floats. */
static void shrink_channel (int16_t *ch, int w, int h, double finest, int n_layers, double scale, float *bufs)
{
  size_t n = (size_t) w * h, i;
  float *cur = bufs, *next = bufs + n, *tmp = bufs + 2 * n, *acc = bufs + 3 * n, *t;
  int j;
  for (i = 0; i < n; i++)
    {
      cur[i] = ch[i];
      acc[i] = 0;
    }
  for (j = 0; j < n_layers; j++)
    {
      float th = (float) threshold (finest, j, scale);
      smooth (cur, next, tmp, w, h, 1 << j);
      i = 0;
#if defined(__arm__)
      if (simd_neon ())
        i = shrink_neon (cur, next, acc, n, th);
#endif
      for (; i < n; i++)
        {
          float d = cur[i] - next[i];
          acc[i] += d > th ? d - th : d < -th ? d + th : 0;
        }
      t = cur; cur = next; next = t;
    }
  for (i = 0; i < n; i++)
    {
      float v = acc[i] + cur[i];
      ch[i] = (int16_t) (v < -32768 ? -32768 : v > 32767 ? 32767 : lrintf (v));
    }
}

int denoise32 (uint32_t *px, int w, int h, int lum, int col, double scale)
{
  double tl = LUM_MAX * (lum > 0 ? lum : 0) / 100, tc = COL_MAX * (col > 0 ? col : 0) / 100;
  int nl = lum > 0 ? layers (tl, LUM_LAYERS, scale) : 0, nc = col > 0 ? layers (tc, COL_LAYERS, scale) : 0;
  size_t n = (size_t) w * h, i;
  int16_t *Y, *cb, *cr;
  float *bufs;
  if ((!nl && !nc) || w <= 0 || h <= 0)
    return 0;
  Y = malloc (n * 2 * 3);
  bufs = malloc (n * sizeof *bufs * 4);
  if (!Y || !bufs)
    {
      free (Y);
      free (bufs);
      return -1;
    }
  cb = Y + n;
  cr = cb + n;
  /* Luminance and colour differences, in quarter levels (finer steps). */
  for (i = 0; i < n; i++)
    {
      int r = (int) (px[i] & 255), g = (int) ((px[i] >> 8) & 255), b = (int) ((px[i] >> 16) & 255);
      int l = (r * 54 + g * 183 + b * 19) >> 8;
      Y[i] = (int16_t) (((r * 54 + g * 183 + b * 19) + 32) >> 6);
      cb[i] = (int16_t) ((b - l) * 4);
      cr[i] = (int16_t) ((r - l) * 4);
    }
  if (nl)
    shrink_channel (Y, w, h, tl * 4, nl, scale, bufs);
  if (nc)
    {
      shrink_channel (cb, w, h, tc * 4, nc, scale, bufs);
      shrink_channel (cr, w, h, tc * 4, nc, scale, bufs);
    }
  for (i = 0; i < n; i++)
    {
      int l4 = Y[i], r4 = l4 + cr[i], b4 = l4 + cb[i], g4, r, g, b;
      g4 = ((l4 << 8) - r4 * 54 - b4 * 19) / 183;
      r = (r4 + 2) >> 2; g = (g4 + 2) >> 2; b = (b4 + 2) >> 2;
      r = r < 0 ? 0 : r > 255 ? 255 : r;
      g = g < 0 ? 0 : g > 255 ? 255 : g;
      b = b < 0 ? 0 : b > 255 ? 255 : b;
      px[i] = (uint32_t) r | (uint32_t) g << 8 | (uint32_t) b << 16;
    }
  free (Y);
  free (bufs);
  return 0;
}

static int cmp_float (const void *a, const void *b)
{
  float x = *(const float *) a, y = *(const float *) b;
  return x < y ? -1 : x > y;
}

/* The median size of the finest layer's detail of one channel (a sample
   of the picture's pixels), as the noise in one pixel. */
static double finest_noise (const uint32_t *px, int w, int h, int which)
{
  int x, y, step = 1, k = 0;
  float *v;
  double m;
  while ((size_t) (w / step) * (h / step) > 200000)
    step++;
  v = malloc (sizeof *v * (size_t) (w / step + 1) * (h / step + 1));
  if (!v)
    return 0;
  for (y = 2; y < h - 2; y += step)
    for (x = 2; x < w - 2; x += step)
      {
        /* The detail at (x, y): the pixel less its B3 blur (step 1). */
        static const int kern[5] = { 1, 4, 6, 4, 1 };
        double s = 0, c = 0;
        int a, b;
        for (b = -2; b <= 2; b++)
          for (a = -2; a <= 2; a++)
            {
              uint32_t p = px[(size_t) (y + b) * w + x + a];
              int r = (int) (p & 255), g = (int) ((p >> 8) & 255), bl = (int) ((p >> 16) & 255);
              int l = (r * 54 + g * 183 + bl * 19) >> 8;
              double val = which == 0 ? l : which == 1 ? bl - l : r - l;
              s += kern[a + 2] * kern[b + 2] * val;
              if (!a && !b)
                c = val;
            }
        v[k++] = (float) fabs (c - s / 256);
      }
  if (!k)
    {
      free (v);
      return 0;
    }
  qsort (v, (size_t) k, sizeof *v, cmp_float);
  m = v[k / 2];
  free (v);
  /* The median of |normal| is 0.6745 sigma; the finest layer holds 0.89 of it. */
  return m / 0.6745 / layer_noise[0];
}

void denoise_estimate (const uint32_t *px, int w, int h, double scale, int *lum, int *col)
{
  double s = scale > 1 ? 1 : scale < 0.05 ? 0.05 : scale, ny, nc, tl, tc;
  *lum = *col = 0;
  if (w < 8 || h < 8)
    return;
  /* The noise there, as it would be at full size; thresholds of about
     three times it in the finest layer. */
  ny = finest_noise (px, w, h, 0) / s;
  nc = (finest_noise (px, w, h, 1) + finest_noise (px, w, h, 2)) / 2 / s;
  tl = 3 * ny * layer_noise[0];
  tc = 3 * nc * layer_noise[0];
  *lum = (int) lround (tl / LUM_MAX * 100 / 5) * 5;
  *col = (int) lround (tc / COL_MAX * 100 / 5) * 5;
  *lum = *lum > 80 ? 80 : *lum;
  *col = *col > 80 ? 80 : *col;
}
