/* Copyright (c) 2026 Andrew Youll
   Released under the MIT licence: see the LICENSE file.
   SPDX-License-Identifier: MIT */
/* astrostack: lines up FITS frames of the night sky and stacks them (an
   average with sigma clipping), all in memory: a test of Lumen's astro
   code on whole frames, and a measure of its speed.

     astrostack OUT.fits REF.fits FRAME.fits...

   Prints each frame's stars, how it lines up, and the time each step
   takes. */
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include "fits.h"
#include "astro.h"

#define MAXF 64

static double now (void)
{
  struct timespec ts;
  clock_gettime (CLOCK_MONOTONIC, &ts);
  return ts.tv_sec + ts.tv_nsec / 1e9;
}

/* A whole frame, linear RGB, and its brightness at half size (a mosaic
   read 2 x 2 to a pixel, so a hot pixel stays one pixel; others binned). */
static int load (const char *path, astro_img *img, float **lum, int *lw, int *lh)
{
  char err[200];
  fits_hdr h;
  FILE *f = fopen (path, "rb");
  fits_reader *r, *hr;
  int y, x, rw, rh;
  float *row;
  if (!f || fits_read_header (f, &h, err, sizeof err))
    {
      fprintf (stderr, "%s: %s\n", path, f ? err : "can't open");
      return -1;
    }
  r = fits_reader_open (f, &h, 0, err, sizeof err);
  hr = h.bayer[0] ? fits_reader_open (f, &h, 1, err, sizeof err) : NULL;
  if (!r || (h.bayer[0] && !hr))
    {
      fprintf (stderr, "%s: %s\n", path, err);
      return -1;
    }
  img->w = h.w; img->h = h.h; img->ch = 3;
  img->px = malloc ((size_t) h.w * h.h * 3 * sizeof (float));
  *lw = h.w / 2; *lh = h.h / 2;
  *lum = malloc ((size_t) *lw * *lh * sizeof (float));
  row = malloc ((size_t) h.w * 3 * sizeof *row);
  if (!img->px || !*lum || !row)
    return -1;
  for (y = 0; y < h.h; y++)
    if (fits_reader_row_linear (r, y, img->px + (size_t) y * h.w * 3))
      return -1;
  if (hr)
    {
      fits_reader_size (hr, &rw, &rh);
      for (y = 0; y < *lh && y < rh; y++)
        {
          fits_reader_row_linear (hr, y, row);
          for (x = 0; x < *lw && x < rw; x++)
            (*lum)[(size_t) y * *lw + x] = (row[3 * x] + row[3 * x + 1] + row[3 * x + 2]) / 3;
        }
      fits_reader_free (hr);
    }
  else
    for (y = 0; y < *lh; y++)
      for (x = 0; x < *lw; x++)
        {
          const float *a = img->px + ((size_t) (2 * y) * h.w + 2 * x) * 3, *b = a + (size_t) h.w * 3;
          (*lum)[(size_t) y * *lw + x] = (a[0] + a[1] + a[2] + a[3] + a[4] + a[5] + b[0] + b[1] + b[2] + b[3] + b[4] + b[5]) / 12;
        }
  free (row);
  fits_reader_free (r);
  fclose (f);
  return 0;
}

int main (int argc, char **argv)
{
  int nf = argc - 2, k, y, x, c, used = 0, w = 0, h = 0, lw, lh, nref = 0;
  static astro_img in[MAXF], al[MAXF];
  static astro_star stars[MAXF][300];
  static int ns[MAXF], ok[MAXF];
  astro_xform t[MAXF];
  double t0, tl = 0, tf = 0, tw = 0, ts;
  char err[200];
  fits_writer *wr;
  float *row;
  if (argc < 4 || nf > MAXF)
    {
      fprintf (stderr, "astrostack OUT.fits REF.fits FRAME.fits...\n");
      return 2;
    }
  for (k = 0; k < nf; k++)
    {
      float *lum;
      astro_plane pl;
      astro_stats st;
      t0 = now ();
      if (load (argv[2 + k], &in[k], &lum, &lw, &lh))
        return 1;
      tl += now () - t0;
      t0 = now ();
      pl.w = lw; pl.h = lh; pl.px = lum; pl.scale = 2;
      ns[k] = astro_find_stars (&pl, 0, stars[k], 300, &st);
      tf += now () - t0;
      free (lum);
      printf ("%s: %d x %d, %d stars (sky %.4f, noise %.5f, %d hot pixels passed over)\n", argv[2 + k], in[k].w, in[k].h,
              ns[k], st.sky, st.noise, st.rejected_hot);
      if (!k)
        w = in[k].w, h = in[k].h, nref = ns[k];
    }
  t[0] = astro_identity ();
  ok[0] = 1;
  for (k = 1; k < nf; k++)
    {
      astro_fit fit;
      t0 = now ();
      ok[k] = astro_match (stars[0], nref, stars[k], ns[k], &t[k], &fit) == 0;
      tf += now () - t0;
      if (ok[k])
        printf ("%s: lines up: %d stars, %.3f px rms; moved %.2f, %.2f px, turned %.3f degrees, scale %.5f\n", argv[2 + k],
                fit.pairs, fit.rms, t[k].tx, t[k].ty, fit.angle, fit.scale);
      else
        printf ("%s: can't be lined up: left out\n", argv[2 + k]);
    }
  for (k = 0; k < nf; k++)
    {
      if (!ok[k])
        continue;
      al[k].w = w; al[k].h = h; al[k].ch = 3;
      al[k].px = malloc ((size_t) w * h * 3 * sizeof (float));
      t0 = now ();
      astro_warp (&in[k], &t[k], &al[k], 0, h);
      tw += now () - t0;
      free (in[k].px);
      used++;
    }
  /* An average with sigma clipping: values more than 3 sigma from the
     median of a pixel's frames are left out, sigma from the median
     absolute deviation (one satellite can't widen it). */
  t0 = now ();
  {
    static const char *const cards[] = { "CREATOR = 'Lumen astrostack (test)'", NULL };
    wr = fits_writer_open (argv[1], w, h, 3, -32, cards, err, sizeof err);
    if (!wr)
      {
        fprintf (stderr, "%s\n", err);
        return 1;
      }
  }
  row = malloc ((size_t) w * 3 * sizeof *row);
  {
    long clipped = 0;
    for (y = 0; y < h; y++)
      {
        for (x = 0; x < w; x++)
          for (c = 0; c < 3; c++)
            {
              float v[MAXF], m, sd, sum = 0, s2 = 0;
              int n = 0, i, j, kept = 0;
              for (k = 0; k < nf; k++)
                if (ok[k])
                  {
                    float u = al[k].px[((size_t) y * w + x) * 3 + c];
                    if (u == u)
                      v[n++] = u;
                  }
              if (!n)
                {
                  row[(size_t) x * 3 + c] = NAN;
                  continue;
                }
              for (i = 1; i < n; i++)
                for (j = i; j > 0 && v[j - 1] > v[j]; j--)
                  {
                    float tt = v[j]; v[j] = v[j - 1]; v[j - 1] = tt;
                  }
              m = n & 1 ? v[n / 2] : 0.5f * (v[n / 2 - 1] + v[n / 2]);
              /* Winsorized sigma: the spread of the values with the
                 outliers pulled in to 1.5 sigma, until it settles. */
              {
                int it;
                for (i = 0; i < n; i++)
                  sum += v[i], s2 += v[i] * v[i];
                sd = n > 1 ? sqrtf (fmaxf (0, (s2 - sum * sum / n) / (n - 1))) : 0;
                for (it = 0; it < 5 && n > 2 && sd > 0; it++)
                  {
                    float lo = m - 1.5f * sd, hi = m + 1.5f * sd, ws = 0, ws2 = 0, nsd;
                    for (i = 0; i < n; i++)
                      {
                        float u = v[i] < lo ? lo : v[i] > hi ? hi : v[i];
                        ws += u, ws2 += u * u;
                      }
                    nsd = 1.134f * sqrtf (fmaxf (0, (ws2 - ws * ws / n) / (n - 1)));
                    if (fabsf (nsd - sd) < 5e-4f * sd)
                      {
                        sd = nsd;
                        break;
                      }
                    sd = nsd;
                  }
              }
              sum = 0;
              for (i = 0; i < n; i++)
                if (n < 3 || fabsf (v[i] - m) <= 3 * sd)
                  sum += v[i], kept++;
                else
                  clipped++;
              row[(size_t) x * 3 + c] = kept ? sum / kept : m;
            }
        fits_writer_row (wr, y, row);
      }
    ts = now () - t0;
    printf ("stacked %d frames: %ld values clipped\n", used, clipped);
  }
  if (fits_writer_close (wr))
    return 1;
  printf ("time: reading %.2f s, stars and matching %.2f s, moving %.2f s, stacking %.2f s\n", tl, tf, tw, ts);
  return 0;
}
