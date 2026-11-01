/* resample.c: see resample.h. */
#include <math.h>
#include <stdlib.h>
#include <string.h>
#include "resample.h"

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

int resample32 (const uint32_t *src, int sw, int sh, uint32_t *dst, int dw, int dh)
{
  axis ax, ay;
  uint32_t *row = NULL, *acc = NULL;
  int x, y, k, rc = -1;
  memset (&ax, 0, sizeof ax);
  memset (&ay, 0, sizeof ay);
  if (sw <= 0 || sh <= 0 || dw <= 0 || dh <= 0)
    return -1;
  if (sw == dw && sh == dh)
    {
      memcpy (dst, src, (size_t) sw * sh * 4);
      return 0;
    }
  row = malloc ((size_t) dw * 3 * sizeof *row);    /* one source row, scaled across: value << 8 */
  acc = malloc ((size_t) dw * 3 * sizeof *acc);
  if (!row || !acc || axis_make (&ax, sw, dw) || axis_make (&ay, sh, dh))
    goto done;
  for (y = 0; y < dh; y++)
    {
      uint32_t *o = dst + (size_t) y * dw;
      memset (acc, 0, (size_t) dw * 3 * sizeof *acc);
      for (k = 0; k < ay.count[y]; k++)
        {
          const uint32_t *s = src + (size_t) (ay.first[y] + k) * sw;
          uint32_t wy = (uint32_t) ay.w[ay.wpos[y] + k];
          for (x = 0; x < dw; x++)
            {
              const uint32_t *p = s + ax.first[x];
              const int *w = ax.w + ax.wpos[x];
              uint32_t r = 0, g = 0, b = 0;
              int t;
              for (t = 0; t < ax.count[x]; t++)
                {
                  uint32_t v = p[t], wt = (uint32_t) w[t];
                  r += (v & 255) * wt;
                  g += (v >> 8 & 255) * wt;
                  b += (v >> 16 & 255) * wt;
                }
              /* 255 << 14 fits; keep 8 fraction bits for the second pass. */
              row[3 * x] = r >> (WBITS - 8);
              row[3 * x + 1] = g >> (WBITS - 8);
              row[3 * x + 2] = b >> (WBITS - 8);
            }
          /* (255 << 8) * (1 << 14) < 2^32 */
          for (x = 0; x < 3 * dw; x++)
            acc[x] += row[x] * wy;
        }
      for (x = 0; x < dw; x++)
        {
          uint32_t r = (acc[3 * x] + (1u << (WBITS + 7))) >> (WBITS + 8);
          uint32_t g = (acc[3 * x + 1] + (1u << (WBITS + 7))) >> (WBITS + 8);
          uint32_t b = (acc[3 * x + 2] + (1u << (WBITS + 7))) >> (WBITS + 8);
          o[x] = (r > 255 ? 255 : r) | (g > 255 ? 255 : g) << 8 | (b > 255 ? 255 : b) << 16;
        }
    }
  rc = 0;
done:
  axis_free (&ax);
  axis_free (&ay);
  free (row);
  free (acc);
  return rc;
}
