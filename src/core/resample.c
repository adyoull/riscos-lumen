/* Copyright (c) 2026 Andrew Youll
   Released under the MIT licence: see the LICENSE file.
   SPDX-License-Identifier: MIT */
/* resample.c: see resample.h. */
#include <math.h>
#include <stdlib.h>
#include <string.h>
#include "resample.h"
#include "simd.h"

#define WBITS 14                       /* weights sum to 1 << WBITS */

/* For each output position on one axis: the first source index, the
   number of taps, and their weights (from one shared array). */
typedef struct { int *first, *count, *wpos; int *w; } axis;

static void axis_free (axis *a)
{
  free (a->first); free (a->count); free (a->wpos); free (a->w);
}

static int axis_make (axis *a, int n_in, int n_out)
{
  int i, k, taps = n_out < n_in ? 2 * ((n_in + n_out - 1) / n_out) + 3 : 2, used = 0;
  a->first = malloc (sizeof (int) * (size_t) n_out);
  a->count = malloc (sizeof (int) * (size_t) n_out);
  a->wpos = malloc (sizeof (int) * (size_t) n_out);
  a->w = malloc (sizeof (int) * (size_t) n_out * (size_t) taps);
  if (!a->first || !a->count || !a->wpos || !a->w)
    return -1;
  for (i = 0; i < n_out; i++)
    {
      int sum = 0, last;
      a->wpos[i] = used;
      if (n_out < n_in)
        {
          /* Shrinking: a tent as wide as two destination pixels (in
             source pixels, scale either side of the centre). Plain area
             averaging leaves a beat pattern on fine regular detail at odd
             ratios; the tent smooths it out for twice the taps. */
          double scale = (double) n_in / n_out, centre = (i + 0.5) * scale, tot = 0, wf[512];
          int j0 = (int) floor (centre - scale), j1 = (int) ceil (centre + scale), j, n = 0;
          if (j0 < 0) j0 = 0;
          if (j1 > n_in - 1) j1 = n_in - 1;
          for (j = j0; j <= j1 && n < 512; j++)
            {
              double t = fabs (j + 0.5 - centre) / scale;
              wf[n] = t < 1 ? 1 - t : 0;
              tot += wf[n++];
            }
          a->first[i] = j0;
          a->count[i] = n;
          for (k = 0; k < n; k++)
            a->w[used + k] = (int) (wf[k] / tot * (1 << WBITS) + 0.5);
        }
      else
        {
          /* Bilinear on pixel centres. */
          long long num = (long long) (2 * i + 1) * n_in - n_out;   /* (centre - 0.5) * 2 * n_out */
          int j, f;
          if (num < 0)
            num = 0;
          j = (int) (num / (2LL * n_out));
          f = (int) (((num - (long long) j * 2 * n_out) << WBITS) / (2LL * n_out));
          if (j >= n_in - 1)
            {
              j = n_in - 1;
              f = 0;
            }
          a->first[i] = j;
          a->count[i] = f ? 2 : 1;
          a->w[used] = (1 << WBITS) - f;
          if (f)
            a->w[used + 1] = f;
        }
      /* Make the weights sum exactly (rounding), on the largest. */
      last = 0;
      for (k = 0; k < a->count[i]; k++)
        {
          sum += a->w[used + k];
          if (a->w[used + k] > a->w[used + last])
            last = k;
        }
      a->w[used + last] += (1 << WBITS) - sum;
      used += a->count[i];
    }
  return 0;
}

/* Two passes for each output row:
     down: the source rows it takes, weighed (all of a row's pixels use the
       same weight, so this is a sweep along memory), into a row of 16-bit
       values, 8 fraction bits (rounded: + 32 >> 6);
     across: each output pixel from its taps along that row, rounded to 8 bits.
   The intermediate row keeps all four bytes of each pixel (the fourth is
   ignored), so both passes are the same in the plain and NEON versions,
   which give the same pixels. Overflow: 255 << 14 summed with weights
   totalling 1 << 14 stays under 2^23; the across sums under 2^31. */

static void down_c (const uint32_t *src, int sw, const axis *ay, int y, uint16_t *v)
{
  int i, k, n = ay->count[y];
  const int *w = ay->w + ay->wpos[y];
  const uint8_t *row0 = (const uint8_t *) (src + (size_t) ay->first[y] * sw);
  for (i = 0; i < 4 * sw; i++)
    {
      uint32_t a = 0;
      for (k = 0; k < n; k++)
        a += (uint32_t) w[k] * row0[(size_t) k * sw * 4 + i];
      v[i] = (uint16_t) ((a + 32) >> 6);
    }
}

static void across_c (const uint16_t *v, const axis *ax, int x0, int x1, uint32_t *o)
{
  int x, t, c;
  for (x = x0; x < x1; x++)
    {
      const uint16_t *p = v + 4 * ax->first[x];
      const int *w = ax->w + ax->wpos[x];
      uint32_t out = 0;
      for (c = 0; c < 3; c++)
        {
          uint32_t a = 0, r;
          for (t = 0; t < ax->count[x]; t++)
            a += (uint32_t) w[t] * p[4 * t + c];
          r = (a + (1u << (WBITS + 7))) >> (WBITS + 8);
          out |= (r > 255 ? 255 : r) << (8 * c);
        }
      o[x] = out;
    }
}

static int resample32_c (const uint32_t *src, int sw, int sh, uint32_t *dst, int dw, int dh)
{
  axis ax, ay;
  uint16_t *v;
  int y, rc = -1;
  memset (&ax, 0, sizeof ax);
  memset (&ay, 0, sizeof ay);
  v = malloc ((size_t) sw * 4 * sizeof *v);
  if (!v || axis_make (&ax, sw, dw) || axis_make (&ay, sh, dh))
    goto done;
  for (y = 0; y < dh; y++)
    {
      down_c (src, sw, &ay, y, v);
      across_c (v, &ax, 0, dw, dst + (size_t) y * dw);
    }
  rc = 0;
done:
  axis_free (&ax);
  axis_free (&ay);
  free (v);
  return rc;
}

#if defined(__arm__)
#pragma GCC push_options
#pragma GCC target ("fpu=neon")
#include <arm_neon.h>

/* Down, 16 bytes (4 pixels) at a time. */
static void down_neon (const uint32_t *src, int sw, const axis *ay, int y, uint16_t *v)
{
  int i = 0, k, n = ay->count[y], len = 4 * sw;
  const int *w = ay->w + ay->wpos[y];
  const uint8_t *row0 = (const uint8_t *) (src + (size_t) ay->first[y] * sw);
  size_t stride = (size_t) sw * 4;
  for (; i + 16 <= len; i += 16)
    {
      uint32x4_t a0 = vdupq_n_u32 (0), a1 = a0, a2 = a0, a3 = a0;
      for (k = 0; k < n; k++)
        {
          uint8x16_t b = vld1q_u8 (row0 + (size_t) k * stride + i);
          uint16x8_t lo = vmovl_u8 (vget_low_u8 (b)), hi = vmovl_u8 (vget_high_u8 (b));
          uint16_t wk = (uint16_t) w[k];
          a0 = vmlal_n_u16 (a0, vget_low_u16 (lo), wk);
          a1 = vmlal_n_u16 (a1, vget_high_u16 (lo), wk);
          a2 = vmlal_n_u16 (a2, vget_low_u16 (hi), wk);
          a3 = vmlal_n_u16 (a3, vget_high_u16 (hi), wk);
        }
      /* + 32 >> 6, rounding as the plain version */
      vst1q_u16 (v + i, vcombine_u16 (vrshrn_n_u32 (a0, 6), vrshrn_n_u32 (a1, 6)));
      vst1q_u16 (v + i + 8, vcombine_u16 (vrshrn_n_u32 (a2, 6), vrshrn_n_u32 (a3, 6)));
    }
  for (; i < len; i++)
    {
      uint32_t a = 0;
      for (k = 0; k < n; k++)
        a += (uint32_t) w[k] * row0[(size_t) k * stride + i];
      v[i] = (uint16_t) ((a + 32) >> 6);
    }
}

/* Across: a pixel's four channels are four lanes; each tap is one load
   and one multiply-accumulate straight from the 16-bit row; two output
   pixels go out together. */
static void across_neon (const uint16_t *v, const axis *ax, int dw, uint32_t *o)
{
  int x = 0, t;
  const uint32x4_t half = vdupq_n_u32 (1u << (WBITS + 7)), top = vdupq_n_u32 (255);
  for (; x + 2 <= dw; x += 2)
    {
      const uint16_t *p0 = v + 4 * ax->first[x], *p1 = v + 4 * ax->first[x + 1];
      const int *w0 = ax->w + ax->wpos[x], *w1 = ax->w + ax->wpos[x + 1];
      uint32x4_t a0 = vdupq_n_u32 (0), a1 = vdupq_n_u32 (0);
      uint16x8_t r;
      for (t = 0; t < ax->count[x]; t++)
        a0 = vmlal_n_u16 (a0, vld1_u16 (p0 + 4 * t), (uint16_t) w0[t]);
      for (t = 0; t < ax->count[x + 1]; t++)
        a1 = vmlal_n_u16 (a1, vld1_u16 (p1 + 4 * t), (uint16_t) w1[t]);
      a0 = vminq_u32 (vshrq_n_u32 (vaddq_u32 (a0, half), WBITS + 8), top);
      a1 = vminq_u32 (vshrq_n_u32 (vaddq_u32 (a1, half), WBITS + 8), top);
      r = vcombine_u16 (vmovn_u32 (a0), vmovn_u32 (a1));
      vst1_u32 (o + x, vand_u32 (vreinterpret_u32_u8 (vmovn_u16 (r)), vdup_n_u32 (0xFFFFFFu)));
    }
  across_c (v, ax, x, dw, o);
}
#pragma GCC pop_options
#endif

#if defined(__arm__)
static int resample32_neon (const uint32_t *src, int sw, int sh, uint32_t *dst, int dw, int dh)
{
  axis ax, ay;
  uint16_t *v;
  int y, rc = -1;
  memset (&ax, 0, sizeof ax);
  memset (&ay, 0, sizeof ay);
  v = malloc ((size_t) sw * 4 * sizeof *v);
  if (!v || axis_make (&ax, sw, dw) || axis_make (&ay, sh, dh))
    goto done;
  for (y = 0; y < dh; y++)
    {
      down_neon (src, sw, &ay, y, v);
      across_neon (v, &ax, dw, dst + (size_t) y * dw);
    }
  rc = 0;
done:
  axis_free (&ax);
  axis_free (&ay);
  free (v);
  return rc;
}
#endif

int resample32 (const uint32_t *src, int sw, int sh, uint32_t *dst, int dw, int dh)
{
  if (sw <= 0 || sh <= 0 || dw <= 0 || dh <= 0)
    return -1;
  if (sw == dw && sh == dh)
    {
      memcpy (dst, src, (size_t) sw * sh * 4);
      return 0;
    }
#if defined(__arm__)
  if (simd_neon ())
    return resample32_neon (src, sw, sh, dst, dw, dh);
#endif
  return resample32_c (src, sw, sh, dst, dw, dh);
}
