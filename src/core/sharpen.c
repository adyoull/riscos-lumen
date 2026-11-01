/* sharpen.c: see sharpen.h. */
#include <math.h>
#include <stdlib.h>
#include "sharpen.h"

#define KMAX 32                        /* kernel half-width limit */

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

int sharpen32 (uint32_t *px, int w, int h, int amount, double sigma)
{
  int m = sharpen_margin (sigma), k, x, y;
  int wt[2 * KMAX + 1], wsum = 0;
  uint8_t *lum;
  uint16_t *tmp;                       /* horizontal blur, luminance * 64 */
  if (amount <= 0 || w <= 0 || h <= 0)
    return 0;
  lum = malloc ((size_t) w * h);
  tmp = malloc ((size_t) w * h * sizeof *tmp);
  if (!lum || !tmp)
    {
      free (lum);
      free (tmp);
      return -1;
    }
  for (k = -m; k <= m; k++)
    wsum += wt[k + m] = (int) lround (1024 * exp (-(double) k * k / (2 * sigma * sigma)));
  for (k = 0; k < w * h; k++)
    {
      uint32_t p = px[k];
      lum[k] = (uint8_t) (((p & 255) * 54 + (p >> 8 & 255) * 183 + (p >> 16 & 255) * 19) >> 8);
    }
  /* Across, clamping at the edges. */
  for (y = 0; y < h; y++)
    {
      const uint8_t *l = lum + (size_t) y * w;
      uint16_t *t = tmp + (size_t) y * w;
      for (x = 0; x < w; x++)
        {
          int acc = 0;
          for (k = -m; k <= m; k++)
            {
              int xx = x + k < 0 ? 0 : x + k >= w ? w - 1 : x + k;
              acc += wt[k + m] * l[xx];
            }
          t[x] = (uint16_t) ((acc * 64 + wsum / 2) / wsum);
        }
    }
  /* Down, then the mask. */
  for (y = 0; y < h; y++)
    for (x = 0; x < w; x++)
      {
        int acc = 0, d, c, adj;
        uint32_t p = px[(size_t) y * w + x], out = 0;
        for (k = -m; k <= m; k++)
          {
            int yy = y + k < 0 ? 0 : y + k >= h ? h - 1 : y + k;
            acc += wt[k + m] * tmp[(size_t) yy * w + x];
          }
        /* luminance * 64 minus its blur * 64 */
        d = lum[(size_t) y * w + x] * 64 - (acc + wsum / 2) / wsum;
        if (d >= -64 && d <= 64)
          continue;                    /* a level or less: leave it */
        adj = d * amount / 6400;
        for (c = 0; c < 24; c += 8)
          {
            int v = (int) (p >> c & 255) + adj;
            v = v < 0 ? 0 : v > 255 ? 255 : v;
            out |= (uint32_t) v << c;
          }
        px[(size_t) y * w + x] = out;
      }
  free (lum);
  free (tmp);
  return 0;
}
