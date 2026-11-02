/* denoise.c: see denoise.h. */
#include <stdlib.h>
#include <string.h>
#include "denoise.h"

/* The chroma blur's radius (each of two box passes) and the luminance
   filter's, for the amounts at a scale. */
static int col_radius (int col, double scale)
{
  int r;
  if (col <= 0)
    return 0;
  r = (int) (col / 100.0 * 4 * scale + 0.5);
  return r < 1 ? 1 : r > 6 ? 6 : r;
}

static int lum_radius (int lum, double scale)
{
  if (lum <= 0 || scale < 0.15)
    return 0;
  return scale >= 0.6 ? 2 : 1;
}

int denoise_margin (int lum, int col, double scale)
{
  return lum_radius (lum, scale) + 2 * col_radius (col, scale);
}

/* A box blur of radius r along a line of n values, step apart, into tmp
   then back (edges held). */
static void box_line (int16_t *v, int n, int step, int r, int16_t *tmp)
{
  int i, sum = 0, win = 2 * r + 1;
  for (i = -r; i <= r; i++)
    sum += v[(i < 0 ? 0 : i >= n ? n - 1 : i) * step];
  for (i = 0; i < n; i++)
    {
      int out = i - r, in = i + r + 1;
      tmp[i] = (int16_t) (sum / win);
      sum += v[(in >= n ? n - 1 : in) * step] - v[(out < 0 ? 0 : out) * step];
    }
  for (i = 0; i < n; i++)
    v[i * step] = tmp[i];
}

int denoise32 (uint32_t *px, int w, int h, int lum, int col, double scale)
{
  int rc = col_radius (col, scale), rl = lum_radius (lum, scale), x, y, pass;
  size_t n = (size_t) w * h;
  int16_t *Y, *cb, *cr, *tmp;
  if ((!rc && !rl) || w <= 0 || h <= 0)
    return 0;
  Y = malloc (n * 2 * 3);
  tmp = malloc ((size_t) (w > h ? w : h) * 2 * 2);
  if (!Y || !tmp)
    {
      free (Y);
      free (tmp);
      return -1;
    }
  cb = Y + n;
  cr = cb + n;
  for (size_t i = 0; i < n; i++)
    {
      int r = (int) (px[i] & 255), g = (int) ((px[i] >> 8) & 255), b = (int) ((px[i] >> 16) & 255);
      int l = (r * 54 + g * 183 + b * 19) >> 8;
      Y[i] = (int16_t) l;
      cb[i] = (int16_t) (b - l);
      cr[i] = (int16_t) (r - l);
    }
  /* Colour: two box passes each way (close to a Gaussian), mixed in by the amount. */
  if (rc)
    {
      int16_t *keep = NULL;
      int mix = col >= 50 ? 256 : col * 256 / 50;
      if (mix < 256 && !(keep = malloc (n * 2 * 2)))
        mix = 256;
      if (keep)
        memcpy (keep, cb, n * 2 * 2);
      for (pass = 0; pass < 2; pass++)
        {
          for (y = 0; y < h; y++)
            {
              box_line (cb + (size_t) y * w, w, 1, rc, tmp);
              box_line (cr + (size_t) y * w, w, 1, rc, tmp);
            }
          for (x = 0; x < w; x++)
            {
              box_line (cb + x, h, w, rc, tmp);
              box_line (cr + x, h, w, rc, tmp);
            }
        }
      if (keep)
        {
          for (size_t i = 0; i < 2 * n; i++)
            cb[i] = (int16_t) (keep[i] + (((cb[i] - keep[i]) * mix) >> 8));
          free (keep);
        }
    }
  /* Luminance: each pixel the average of its neighbours within rl, each
     weighed by how close it is in lightness (none past the threshold). */
  if (rl)
    {
      int t = 2 + lum * 22 / 100;      /* levels: 2 to 24 */
      int16_t *out = malloc (n * 2);
      if (out)
        {
          for (y = 0; y < h; y++)
            for (x = 0; x < w; x++)
              {
                int c = Y[(size_t) y * w + x], sum = 0, wsum = 0, dy, dx;
                for (dy = -rl; dy <= rl; dy++)
                  {
                    int yy = y + dy < 0 ? 0 : y + dy >= h ? h - 1 : y + dy;
                    const int16_t *row = Y + (size_t) yy * w;
                    for (dx = -rl; dx <= rl; dx++)
                      {
                        int xx = x + dx < 0 ? 0 : x + dx >= w ? w - 1 : x + dx;
                        int v = row[xx], d = v > c ? v - c : c - v, wt = t - d;
                        if (wt > 0)
                          {
                            sum += v * wt;
                            wsum += wt;
                          }
                      }
                  }
                out[(size_t) y * w + x] = (int16_t) (wsum ? (sum + wsum / 2) / wsum : c);
              }
          memcpy (Y, out, n * 2);
          free (out);
        }
    }
  for (size_t i = 0; i < n; i++)
    {
      int l = Y[i], b = l + cb[i], r = l + cr[i], g;
      r = r < 0 ? 0 : r > 255 ? 255 : r;
      b = b < 0 ? 0 : b > 255 ? 255 : b;
      g = ((l << 8) - r * 54 - b * 19) / 183;
      g = g < 0 ? 0 : g > 255 ? 255 : g;
      px[i] = (uint32_t) r | (uint32_t) g << 8 | (uint32_t) b << 16;
    }
  free (Y);
  free (tmp);
  return 0;
}
