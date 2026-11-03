/* sharpen.c: see sharpen.h. */
#include <math.h>
#include <stdlib.h>
#include "sharpen.h"
#include "simd.h"

#define KMAX 32                        /* kernel half-width limit */
#define WSHIFT 10                      /* the kernel's weights sum to 1 << WSHIFT */

double sharpen_sigma (const recipe *r, double scale)
{
  double s = r->v[P_SHARP_RADIUS] * scale;
  return s < 0.35 ? 0.35 : s;          /* below this the kernel is a single tap */
}

int sharpen_margin (double sigma)
{
  int m = (int) ceil (sigma * 3);
  return m < 1 ? 1 : m > KMAX ? KMAX : m;
}

/* A Gaussian of 2m + 1 taps whose weights sum to exactly 1 << WSHIFT (the
   middle one takes the rounding), so the blur is a shift, not a divide. */
static void kernel (int m, double sigma, int *wt)
{
  double g[2 * KMAX + 1], tot = 0;
  int k, sum = 0;
  for (k = -m; k <= m; k++)
    tot += g[k + m] = exp (-(double) k * k / (2 * sigma * sigma));
  for (k = -m; k <= m; k++)
    sum += wt[k + m] = (int) lround (g[k + m] / tot * (1 << WSHIFT));
  wt[m] += (1 << WSHIFT) - sum;
}

/* Across, one row of luminance to its blur * 64, clamping at the edges;
   x0..x1 only (the rest done by the caller). */
static void across_c (const uint8_t *l, uint16_t *t, int w, int m, const int *wt, int x0, int x1)
{
  int x, k;
  for (x = x0; x < x1; x++)
    {
      int acc = 0;
      for (k = -m; k <= m; k++)
        {
          int xx = x + k < 0 ? 0 : x + k >= w ? w - 1 : x + k;
          acc += wt[k + m] * l[xx];
        }
      t[x] = (uint16_t) ((acc * 64 + (1 << (WSHIFT - 1))) >> WSHIFT);
    }
}

/* Down: the blur * 64 of row y, from the across rows. */
static void down_c (const uint16_t *tmp, int w, int h, int y, int m, const int *wt, uint16_t *out)
{
  int x, k;
  for (x = 0; x < w; x++)
    {
      unsigned acc = 0;
      for (k = -m; k <= m; k++)
        {
          int yy = y + k < 0 ? 0 : y + k >= h ? h - 1 : y + k;
          acc += (unsigned) wt[k + m] * tmp[(size_t) yy * w + x];
        }
      out[x] = (uint16_t) ((acc + (1u << (WSHIFT - 1))) >> WSHIFT);
    }
}

#if defined(__arm__)
/* The same two passes with NEON, eight pixels at a time, the same sums. */
#pragma GCC push_options
#pragma GCC target ("fpu=neon")
#include <arm_neon.h>

static void across_neon (const uint8_t *l, uint16_t *t, int w, int m, const int *wt)
{
  int x = m, k;
  across_c (l, t, w, m, wt, 0, m < w ? m : w);
  for (; x + 8 <= w - m; x += 8)
    {
      uint32x4_t lo = vdupq_n_u32 (0), hi = vdupq_n_u32 (0);
      for (k = -m; k <= m; k++)
        {
          uint16x8_t v = vmovl_u8 (vld1_u8 (l + x + k));
          lo = vmlal_n_u16 (lo, vget_low_u16 (v), (uint16_t) wt[k + m]);
          hi = vmlal_n_u16 (hi, vget_high_u16 (v), (uint16_t) wt[k + m]);
        }
      /* (acc * 64 + half) >> WSHIFT */
      lo = vshrq_n_u32 (vaddq_u32 (vshlq_n_u32 (lo, 6), vdupq_n_u32 (1u << (WSHIFT - 1))), WSHIFT);
      hi = vshrq_n_u32 (vaddq_u32 (vshlq_n_u32 (hi, 6), vdupq_n_u32 (1u << (WSHIFT - 1))), WSHIFT);
      vst1q_u16 (t + x, vcombine_u16 (vmovn_u32 (lo), vmovn_u32 (hi)));
    }
  across_c (l, t, w, m, wt, x < m ? m : x, w);
}

static void down_neon (const uint16_t *tmp, int w, int h, int y, int m, const int *wt, uint16_t *out)
{
  int x = 0, k;
  for (; x + 8 <= w; x += 8)
    {
      uint32x4_t lo = vdupq_n_u32 (0), hi = vdupq_n_u32 (0);
      for (k = -m; k <= m; k++)
        {
          int yy = y + k < 0 ? 0 : y + k >= h ? h - 1 : y + k;
          uint16x8_t v = vld1q_u16 (tmp + (size_t) yy * w + x);
          lo = vmlal_n_u16 (lo, vget_low_u16 (v), (uint16_t) wt[k + m]);
          hi = vmlal_n_u16 (hi, vget_high_u16 (v), (uint16_t) wt[k + m]);
        }
      lo = vshrq_n_u32 (vaddq_u32 (lo, vdupq_n_u32 (1u << (WSHIFT - 1))), WSHIFT);
      hi = vshrq_n_u32 (vaddq_u32 (hi, vdupq_n_u32 (1u << (WSHIFT - 1))), WSHIFT);
      vst1q_u16 (out + x, vcombine_u16 (vmovn_u32 (lo), vmovn_u32 (hi)));
    }
  if (x < w)
    {
      /* The last few: as the plain version, into a scratch row. */
      uint16_t rest[8];
      int n = w - x, i;
      for (i = 0; i < n; i++)
        {
          unsigned acc = 0;
          for (k = -m; k <= m; k++)
            {
              int yy = y + k < 0 ? 0 : y + k >= h ? h - 1 : y + k;
              acc += (unsigned) wt[k + m] * tmp[(size_t) yy * w + x + i];
            }
          rest[i] = (uint16_t) ((acc + (1u << (WSHIFT - 1))) >> WSHIFT);
        }
      for (i = 0; i < n; i++)
        out[x + i] = rest[i];
    }
}
#pragma GCC pop_options
#endif

int sharpen32 (uint32_t *px, int w, int h, int amount, double sigma)
{
  int m = sharpen_margin (sigma), k, x, y, neon = 0;
  int wt[2 * KMAX + 1];
  uint8_t *lum;
  uint16_t *tmp, *blur;                /* horizontal blur, luminance * 64; one row of the whole blur */
  if (amount <= 0 || w <= 0 || h <= 0)
    return 0;
  lum = malloc ((size_t) w * h);
  tmp = malloc ((size_t) w * h * sizeof *tmp);
  blur = malloc ((size_t) w * sizeof *blur);
  if (!lum || !tmp || !blur)
    {
      free (lum);
      free (tmp);
      free (blur);
      return -1;
    }
#if defined(__arm__)
  neon = simd_neon ();
#endif
  (void) neon;
  kernel (m, sigma, wt);
  for (k = 0; k < w * h; k++)
    {
      uint32_t p = px[k];
      lum[k] = (uint8_t) (((p & 255) * 54 + (p >> 8 & 255) * 183 + (p >> 16 & 255) * 19) >> 8);
    }
  for (y = 0; y < h; y++)
    {
#if defined(__arm__)
      if (neon)
        across_neon (lum + (size_t) y * w, tmp + (size_t) y * w, w, m, wt);
      else
#endif
        across_c (lum + (size_t) y * w, tmp + (size_t) y * w, w, m, wt, 0, w);
    }
  /* Down, then the mask. */
  for (y = 0; y < h; y++)
    {
#if defined(__arm__)
      if (neon)
        down_neon (tmp, w, h, y, m, wt, blur);
      else
#endif
        down_c (tmp, w, h, y, m, wt, blur);
      for (x = 0; x < w; x++)
        {
          int d, c, adj;
          uint32_t p = px[(size_t) y * w + x], out = 0;
          /* luminance * 64 minus its blur * 64 */
          d = lum[(size_t) y * w + x] * 64 - blur[x];
          if (d >= -64 && d <= 64)
            continue;                  /* a level or less: leave it */
          adj = d * amount / 6400;
          for (c = 0; c < 24; c += 8)
            {
              int v = (int) (p >> c & 255) + adj;
              v = v < 0 ? 0 : v > 255 ? 255 : v;
              out |= (uint32_t) v << c;
            }
          px[(size_t) y * w + x] = out;
        }
    }
  free (lum);
  free (tmp);
  free (blur);
  return 0;
}
