/* Copyright (c) 2026 Andrew Youll
   Released under the MIT licence: see the LICENSE file.
   SPDX-License-Identifier: MIT */
/* stack.c: see stack.h. */
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "fits.h"
#include "fsys.h"
#include "stack.h"

#define BAND 16                        /* output rows stacked together */
#define MAX_STARS 300
#define MAX_FRAMES 256
#define KAPPA 3.0f                     /* sigma clipping: values this many sigma from the median are left out */

enum { P_DARK, P_STARS, P_MATCH, P_OPEN, P_WARP, P_COMBINE, P_DONE, P_FAILED };

typedef struct
{
  stack_frame rep;
  fits_hdr h;
  FILE *f;
  fits_reader *r;
  astro_star *stars;
  int nstars;
  double med[3];              /* its sky in each colour (0-1) */
  astro_xform t;              /* the reference's pixels to this frame's */
  float *win;                 /* rows win_y0... of the frame, linear RGB */
  int win_y0, win_rows, win_cap;
  float *band;                /* its band moved onto the reference */
} sframe;

struct stack_job
{
  int phase;
  char err[2400], out[1024], creator[64];
  int nl, nd;
  sframe *fr;
  char (*dark_path)[1024];
  fits_hdr h0;
  int w, h, planes, mosaic, out_planes;
  double lo, hi;              /* every frame read on the first one's scale */
  /* the master dark */
  uint16_t *mdark;
  FILE **df;
  fits_reader **dr;
  uint16_t *drows;
  int dark_row;               /* plane * h + row */
  /* stars: the frame being looked at */
  int k;
  int sub;                    /* 0 open it, 1 its rows, 2 its stars */
  float *plane, *rowbuf, *samples;
  int pw, ph, py;
  size_t ns, scap;
  astro_finder *finder;
  /* stacking */
  int ref, used, y0, wf;      /* the band's first row; the frame being moved */
  int cy;                     /* the row being combined */
  fits_writer *wr;
  float *outrow;
  long long clipped, values;
  double exptime;
};

static void fail (stack_job *j, const char *fmt, const char *a, const char *b)
{
  snprintf (j->err, sizeof j->err, fmt, a ? a : "", b ? b : "");
  j->phase = P_FAILED;
}

static const char *leaf (const char *path)
{
  return fsys_leaf (path);             /* the filing system's idea of a leafname */
}

/* The header of a frame, checked against the first. */
static int frame_header (const char *path, fits_hdr *h, char *err, size_t n)
{
  FILE *f = fopen (path, "rb");
  char e[200];
  if (!f)
    {
      snprintf (err, n, "Can't open %s", path);
      return -1;
    }
  if (fits_read_header (f, h, e, sizeof e))
    {
      snprintf (err, n, "%s isn't a FITS frame Lumen can stack (%s). Stacking works on FITS frames.", leaf (path), e);
      fclose (f);
      return -1;
    }
  fclose (f);
  return 0;
}

static int same_kind (const fits_hdr *a, const fits_hdr *b)
{
  return a->w == b->w && a->h == b->h && a->planes == b->planes && !strcmp (a->bayer, b->bayer);
}

static void describe_kind (const fits_hdr *h, char *out, size_t n)
{
  snprintf (out, n, "%d x %d, %s", h->w, h->h, h->planes == 3 ? "RGB" : h->bayer[0] ? "colour mosaic" : "grey");
}

stack_job *stack_start (const char *const *lights, int nl, const char *const *darks, int nd, const char *out,
                        const char *creator, char *err, size_t en)
{
  stack_job *j;
  int k;
  if (nl < 2)
    {
      snprintf (err, en, "Stacking needs at least two light frames.");
      return NULL;
    }
  if (nl > MAX_FRAMES || nd > MAX_FRAMES)
    {
      snprintf (err, en, "Lumen can stack at most %d frames at a time.", MAX_FRAMES);
      return NULL;
    }
  j = calloc (1, sizeof *j);
  if (!j || !(j->fr = calloc ((size_t) nl, sizeof *j->fr))
      || (nd && !(j->dark_path = calloc ((size_t) nd, sizeof *j->dark_path))))
    {
      if (j)
        free (j->fr);
      free (j);
      snprintf (err, en, "Not enough memory to stack these frames.");
      return NULL;
    }
  j->nl = nl;
  j->nd = nd;
  snprintf (j->out, sizeof j->out, "%s", out);
  snprintf (j->creator, sizeof j->creator, "%s", creator ? creator : "Lumen");
  for (k = 0; k < nl; k++)
    {
      sframe *s = &j->fr[k];
      snprintf (s->rep.path, sizeof s->rep.path, "%s", lights[k]);
      if (frame_header (lights[k], &s->h, err, en))
        {
          stack_free (j);
          return NULL;
        }
      if (k && !same_kind (&s->h, &j->fr[0].h))
        {
          char a[80], b[80];
          describe_kind (&j->fr[0].h, a, sizeof a);
          describe_kind (&s->h, b, sizeof b);
          snprintf (err, en, "%s isn't the same kind of frame as %s (%s, not %s). The lights must all come from the same camera, at the same size.",
                    leaf (lights[k]), leaf (lights[0]), b, a);
          stack_free (j);
          return NULL;
        }
    }
  j->h0 = j->fr[0].h;
  for (k = 0; k < nd; k++)
    {
      fits_hdr dh;
      snprintf (j->dark_path[k], sizeof j->dark_path[k], "%s", darks[k]);
      if (frame_header (darks[k], &dh, err, en))
        {
          stack_free (j);
          return NULL;
        }
      if (!same_kind (&dh, &j->h0))
        {
          char a[80], b[80];
          describe_kind (&j->h0, a, sizeof a);
          describe_kind (&dh, b, sizeof b);
          snprintf (err, en, "The dark %s isn't the same kind of frame as the lights (%s, not %s).", leaf (darks[k]), b, a);
          stack_free (j);
          return NULL;
        }
    }
  j->w = j->h0.w;
  j->h = j->h0.h;
  j->planes = j->h0.planes;
  j->mosaic = j->h0.bayer[0] != 0;
  j->out_planes = j->planes == 3 || j->mosaic ? 3 : 1;
  /* Every frame read on the first one's scale. */
  {
    FILE *f = fopen (lights[0], "rb");
    fits_reader *r = f ? fits_reader_open (f, &j->h0, 0, err, en) : NULL;
    if (!r)
      {
        if (f)
          fclose (f);
        stack_free (j);
        return NULL;
      }
    fits_reader_range (r, &j->lo, &j->hi);
    fits_reader_free (r);
    fclose (f);
  }
  j->phase = nd ? P_DARK : P_STARS;
  return j;
}

/* --- the master dark --- */

static int dark_step (stack_job *j)
{
  int k, x, w = j->w, total = j->planes * j->h;
  if (!j->dr)
    {
      char e[200];
      j->dr = calloc ((size_t) j->nd, sizeof *j->dr);
      j->df = calloc ((size_t) j->nd, sizeof *j->df);
      j->drows = malloc ((size_t) j->nd * w * sizeof *j->drows);
      j->mdark = malloc ((size_t) total * w * sizeof *j->mdark);
      if (!j->dr || !j->df || !j->drows || !j->mdark)
        {
          fail (j, "Not enough memory for the master dark.%s%s", NULL, NULL);
          return -1;
        }
      for (k = 0; k < j->nd; k++)
        {
          fits_hdr h;
          j->df[k] = fopen (j->dark_path[k], "rb");
          if (!j->df[k] || fits_read_header (j->df[k], &h, e, sizeof e)
              || !(j->dr[k] = fits_reader_open (j->df[k], &h, 0, e, sizeof e)))
            {
              fail (j, "The dark %s can't be read: %s", leaf (j->dark_path[k]), e);
              return -1;
            }
          fits_reader_set_range (j->dr[k], j->lo, j->hi);
        }
      j->dark_row = 0;
      return 0;
    }
  /* One row: each pixel the median of the darks'. */
  {
    int p = j->dark_row / j->h, y = j->dark_row % j->h, nd = j->nd;
    uint16_t *o = j->mdark + (size_t) j->dark_row * w;
    for (k = 0; k < nd; k++)
      if (fits_reader_raw_row (j->dr[k], p, y, j->drows + (size_t) k * w))
        {
          fail (j, "The dark %s can't be read (cut short?)%s", leaf (j->dark_path[k]), NULL);
          return -1;
        }
    for (x = 0; x < w; x++)
      {
        uint16_t v[MAX_FRAMES];
        int a, b;
        for (a = 0; a < nd; a++)
          {
            uint16_t u = j->drows[(size_t) a * w + x];
            for (b = a; b > 0 && v[b - 1] > u; b--)
              v[b] = v[b - 1];
            v[b] = u;
          }
        o[x] = nd & 1 ? v[nd / 2] : (uint16_t) ((v[nd / 2 - 1] + v[nd / 2] + 1) / 2);
      }
  }
  if (++j->dark_row >= total)
    {
      for (k = 0; k < j->nd; k++)
        {
          fits_reader_free (j->dr[k]);
          if (j->df[k])
            fclose (j->df[k]);
        }
      free (j->dr); free (j->df); free (j->drows);
      j->dr = NULL; j->df = NULL; j->drows = NULL;
      j->phase = P_STARS;
      j->k = 0;
      j->sub = 0;
    }
  return 0;
}

/* --- each light's stars --- */

static int open_frame (stack_job *j, sframe *s, int half)
{
  char e[200];
  s->f = fopen (s->rep.path, "rb");
  if (!s->f)
    {
      fail (j, "%s can't be opened%s", leaf (s->rep.path), NULL);
      return -1;
    }
  if (!(s->r = fits_reader_open (s->f, &s->h, half, e, sizeof e)))
    {
      fclose (s->f);
      s->f = NULL;
      fail (j, "%s can't be read: %s", leaf (s->rep.path), e);
      return -1;
    }
  /* Rows are read here and there: unbuffered, each is one read. */
  setvbuf (s->f, NULL, _IONBF, 0);
  fits_reader_set_range (s->r, j->lo, j->hi);
  if (j->mdark)
    fits_reader_set_dark (s->r, j->mdark);
  return 0;
}

static void close_frame (sframe *s)
{
  fits_reader_free (s->r);
  s->r = NULL;
  if (s->f)
    fclose (s->f);
  s->f = NULL;
}

static int cmp_d (const void *a, const void *b)
{
  float x = *(const float *) a, y = *(const float *) b;
  return x < y ? -1 : x > y;
}

static int stars_step (stack_job *j)
{
  sframe *s = &j->fr[j->k];
  int x, w = j->w;
  switch (j->sub)
    {
    case 0:
      /* A mosaic read 2 x 2 to a pixel (a hot pixel stays one pixel);
         others binned 2 x 2 here. */
      if (open_frame (j, s, j->mosaic))
        return -1;
      j->pw = w / 2;
      j->ph = j->h / 2;
      j->py = 0;
      j->plane = malloc ((size_t) j->pw * j->ph * sizeof *j->plane);
      j->rowbuf = malloc ((size_t) w * 3 * 2 * sizeof *j->rowbuf);
      j->scap = (size_t) (j->pw / 4 + 1) * (j->ph / 4 + 1);
      j->samples = malloc (j->scap * 3 * sizeof *j->samples);
      j->ns = 0;
      if (!j->plane || !j->rowbuf || !j->samples)
        {
          fail (j, "Not enough memory to look for stars%s%s", NULL, NULL);
          return -1;
        }
      j->sub = 1;
      return 0;
    case 1:
      {
        float *o = j->plane + (size_t) j->py * j->pw;
        if (j->mosaic)
          {
            if (fits_reader_row_linear (s->r, j->py, j->rowbuf))
              goto unreadable;
            for (x = 0; x < j->pw; x++)
              o[x] = (j->rowbuf[3 * x] + j->rowbuf[3 * x + 1] + j->rowbuf[3 * x + 2]) / 3;
          }
        else
          {
            float *a = j->rowbuf, *b = j->rowbuf + (size_t) w * 3;
            if (fits_reader_row_linear (s->r, 2 * j->py, a) || fits_reader_row_linear (s->r, 2 * j->py + 1, b))
              goto unreadable;
            for (x = 0; x < j->pw; x++)
              {
                int c;
                float sum = 0;
                for (c = 0; c < 3; c++)
                  {
                    a[6 * x + c] = (a[6 * x + c] + a[6 * x + 3 + c] + b[6 * x + c] + b[6 * x + 3 + c]) / 4;
                    sum += a[6 * x + c];
                  }
                o[x] = sum / 3;
              }
            /* Its colours where the sampling below looks for them. */
            for (x = 0; x < j->pw; x++)
              memmove (a + 3 * x, a + 6 * x, 3 * sizeof *a);
          }
        if (!(j->py & 3))
          for (x = 0; x < j->pw && j->ns < j->scap; x += 4)
            {
              const float *q = j->rowbuf + 3 * x;
              if (q[0] == q[0] && q[1] == q[1] && q[2] == q[2])
                {
                  j->samples[j->ns] = q[0];
                  j->samples[j->scap + j->ns] = q[1];
                  j->samples[2 * j->scap + j->ns] = q[2];
                  j->ns++;
                }
            }
        if (++j->py >= j->ph)
          {
            astro_plane pl;
            pl.w = j->pw; pl.h = j->ph; pl.px = j->plane; pl.scale = 2;
            close_frame (s);
            j->finder = astro_find_begin (&pl, 0);
            if (!j->finder)
              {
                fail (j, "Not enough memory to look for stars%s%s", NULL, NULL);
                return -1;
              }
            j->sub = 2;
          }
        return 0;
      }
    default:
      if (!astro_find_step (j->finder, 16))
        return 0;
      {
        astro_stats st;
        int c;
        s->stars = malloc (MAX_STARS * sizeof *s->stars);
        if (!s->stars)
          {
            fail (j, "Not enough memory to look for stars%s%s", NULL, NULL);
            return -1;
          }
        s->nstars = astro_find_end (j->finder, s->stars, MAX_STARS, &st);
        if (s->nstars < 0)
          s->nstars = 0;
        s->rep.stars = s->nstars;
        astro_find_free (j->finder);
        j->finder = NULL;
        for (c = 0; c < 3; c++)
          {
            float *v = j->samples + (size_t) c * j->scap;
            qsort (v, j->ns, sizeof *v, cmp_d);
            s->med[c] = j->ns ? v[j->ns / 2] : 0;
          }
        free (j->plane); free (j->rowbuf); free (j->samples);
        j->plane = j->rowbuf = j->samples = NULL;
        j->sub = 0;
        if (++j->k >= j->nl)
          j->phase = P_MATCH;
      }
      return 0;
    }
unreadable:
  fail (j, "%s can't be read (cut short?)%s", leaf (s->rep.path), NULL);
  return -1;
}

/* --- lining up --- */

static int match_all (stack_job *j)
{
  int k, best = 0;
  sframe *r;
  for (k = 1; k < j->nl; k++)
    if (j->fr[k].nstars > j->fr[best].nstars)
      best = k;
  j->ref = best;
  r = &j->fr[best];
  if (r->nstars < 3)
    {
      fail (j, "Too few stars to line the frames up (%s has the most, %s).", leaf (r->rep.path),
            r->nstars == 1 ? "one" : r->nstars == 2 ? "two" : "none");
      return -1;
    }
  r->t = astro_identity ();
  r->rep.used = r->rep.reference = 1;
  r->rep.pairs = r->nstars;
  j->used = 1;
  for (k = 0; k < j->nl; k++)
    {
      sframe *s = &j->fr[k];
      astro_fit fit;
      if (k == best)
        continue;
      if (s->nstars < 3)
        snprintf (s->rep.why, sizeof s->rep.why, "only %d star%s found (cloud, or out of focus?)", s->nstars,
                  s->nstars == 1 ? "" : "s");
      else if (astro_match (r->stars, r->nstars, s->stars, s->nstars, &s->t, &fit))
        snprintf (s->rep.why, sizeof s->rep.why, "its stars don't match the reference's (%d found)", s->nstars);
      else
        {
          s->rep.used = 1;
          s->rep.pairs = fit.pairs;
          s->rep.rms = fit.rms;
          s->rep.angle = fit.angle;
          s->rep.dx = s->t.tx;
          s->rep.dy = s->t.ty;
          j->used++;
        }
    }
  if (j->used < 2)
    {
      fail (j, "None of the other frames could be lined up with %s.%s", leaf (r->rep.path), NULL);
      return -1;
    }
  j->phase = P_OPEN;
  return 0;
}

/* --- stacking --- */

/* The rows of frame s that the band y0..y1-1 needs. */
static void rows_needed (const stack_job *j, const sframe *s, int y0, int y1, int *s0, int *s1)
{
  double lo = 1e30, hi = -1e30;
  int k;
  for (k = 0; k < 4; k++)
    {
      double x = k & 1 ? j->w - 1 : 0, y = k & 2 ? y1 - 1 : y0, u, v;
      astro_apply (&s->t, x, y, &u, &v);
      if (v < lo) lo = v;
      if (v > hi) hi = v;
    }
  *s0 = (int) floor (lo) - 2;
  *s1 = (int) ceil (hi) + 3;
  if (*s0 < 0) *s0 = 0;
  if (*s1 > j->h) *s1 = j->h;
}

static int open_all (stack_job *j)
{
  int k;
  char cards[12][84];
  const char *cp[13];
  int nc = 0;
  for (k = 0; k < j->nl; k++)
    {
      sframe *s = &j->fr[k];
      double span;
      if (!s->rep.used)
        continue;
      if (open_frame (j, s, 0))
        return -1;
      span = (BAND - 1) * fabs (s->t.a) + (j->w - 1) * fabs (s->t.b) + (BAND - 1) * fabs (s->t.b) + 8;
      s->win_cap = (int) ceil (span) + 1;
      if (s->win_cap > j->h)
        s->win_cap = j->h;
      s->win = malloc ((size_t) s->win_cap * j->w * 3 * sizeof *s->win);
      s->band = malloc ((size_t) BAND * j->w * 3 * sizeof *s->band);
      s->win_rows = 0;
      if (!s->win || !s->band)
        {
          char mb[16];
          snprintf (mb, sizeof mb, "%d", (int) (((size_t) (s->win_cap + BAND) * j->w * 12) >> 20) + 1);
          fail (j, "Not enough memory to stack these frames: each needs about %s MB while stacking. Stack fewer frames at a time, or quit other programs.%s", mb, NULL);
          return -1;
        }
      j->exptime += s->h.exptime;
    }
  j->outrow = malloc ((size_t) j->w * 3 * sizeof *j->outrow);
  if (!j->outrow)
    {
      fail (j, "Not enough memory to stack these frames.%s%s", NULL, NULL);
      return -1;
    }
  {
    const fits_hdr *r = &j->fr[j->ref].h;
    if (r->object[0]) snprintf (cards[nc++], 84, "OBJECT  = '%.60s'", r->object);
    if (r->telescop[0]) snprintf (cards[nc++], 84, "TELESCOP= '%.60s'", r->telescop);
    if (r->instrume[0]) snprintf (cards[nc++], 84, "INSTRUME= '%.60s'", r->instrume);
    if (r->filter[0]) snprintf (cards[nc++], 84, "FILTER  = '%.60s'", r->filter);
    if (r->date_obs[0]) snprintf (cards[nc++], 84, "DATE-OBS= '%.30s'", r->date_obs);
    if (r->focal > 0) snprintf (cards[nc++], 84, "FOCALLEN= %20g", r->focal);
    if (r->iso > 0) snprintf (cards[nc++], 84, "ISOSPEED= %20d", r->iso);
    if (j->exptime > 0) snprintf (cards[nc++], 84, "EXPTIME = %20g / seconds, all frames stacked", j->exptime);
    snprintf (cards[nc++], 84, "STACKCNT= %20d / frames stacked", j->used);
    snprintf (cards[nc++], 84, "IMAGETYP= 'Stacked light'");
    snprintf (cards[nc++], 84, "CREATOR = '%.60s'", j->creator);
    if (r->geo)
      {
        snprintf (cards[nc++], 84, "SITELAT = %20.7f", r->lat / 1e7);
        if (nc < 12)
          snprintf (cards[nc++], 84, "SITELONG= %20.7f", r->lon / 1e7);
      }
    for (k = 0; k < nc; k++)
      cp[k] = cards[k];
    cp[nc] = NULL;
  }
  {
    char e[300];
    j->wr = fits_writer_open (j->out, j->w, j->h, j->out_planes, -32, cp, e, sizeof e);
    if (!j->wr)
      {
        fail (j, "%s%s", e, NULL);
        return -1;
      }
  }
  j->y0 = 0;
  j->wf = 0;
  j->phase = P_WARP;
  return 0;
}

/* Frame s's rows s0..s1-1 in its window: those it has kept, the rest read. */
static int load_window (stack_job *j, sframe *s, int s0, int s1)
{
  size_t stride = (size_t) j->w * 3;
  int y, keep0 = s->win_rows ? s->win_y0 : 0, keep1 = s->win_rows ? s->win_y0 + s->win_rows : 0;
  int lo = s0 > keep0 ? s0 : keep0, hi = s1 < keep1 ? s1 : keep1;
  if (s1 - s0 > s->win_cap)
    s1 = s0 + s->win_cap;
  if (hi > lo)
    memmove (s->win + (size_t) (lo - s0) * stride, s->win + (size_t) (lo - s->win_y0) * stride,
             (size_t) (hi - lo) * stride * sizeof (float));
  /* The rows it hasn't got, in the order they come in the file's reading. */
  for (y = s0; y < s1; y++)
    if (y < lo || y >= hi)
      if (fits_reader_row_linear (s->r, y, s->win + (size_t) (y - s0) * stride))
        return -1;
  s->win_y0 = s0;
  s->win_rows = s1 - s0;
  return 0;
}

static int warp_step (stack_job *j)
{
  int y1 = j->y0 + BAND < j->h ? j->y0 + BAND : j->h;
  sframe *s;
  while (j->wf < j->nl && !j->fr[j->wf].rep.used)
    j->wf++;
  if (j->wf >= j->nl)
    {
      j->phase = P_COMBINE;
      j->cy = j->y0;
      return 0;
    }
  s = &j->fr[j->wf];
  {
    int s0, s1;
    astro_img src, dst;
    rows_needed (j, s, j->y0, y1, &s0, &s1);
    if (s1 <= s0)
      {
        size_t k, n = (size_t) (y1 - j->y0) * j->w * 3;
        for (k = 0; k < n; k++)
          s->band[k] = NAN;
      }
    else
      {
        if (load_window (j, s, s0, s1))
          {
            fail (j, "%s can't be read (cut short?)%s", leaf (s->rep.path), NULL);
            return -1;
          }
        src.w = j->w; src.h = j->h; src.ch = 3; src.px = s->win; src.y0 = s->win_y0; src.rows = s->win_rows;
        dst.w = j->w; dst.h = j->h; dst.ch = 3; dst.px = s->band; dst.y0 = j->y0; dst.rows = y1 - j->y0;
        astro_warp (&src, &s->t, &dst, j->y0, y1);
      }
  }
  j->wf++;
  return 0;
}

/* One row: each pixel the frames' average, sigma-clipped (Winsorized). */
static int combine_step (stack_job *j)
{
  int w = j->w, x, c, k, nl = j->nl, oc = j->out_planes;
  size_t off = (size_t) (j->cy - j->y0) * w * 3;
  const sframe *ref = &j->fr[j->ref];
  for (x = 0; x < w; x++)
    for (c = 0; c < oc; c++)
      {
        float v[MAX_FRAMES], m, sd, sum = 0, s2 = 0;
        int n = 0, i, a, kept = 0, it;
        for (k = 0; k < nl; k++)
          if (j->fr[k].rep.used)
            {
              float u = j->fr[k].band[off + (size_t) x * 3 + c];
              if (u == u)
                {
                  /* The frame's sky brought to the reference's. */
                  u += (float) (ref->med[c] - j->fr[k].med[c]);
                  for (a = n; a > 0 && v[a - 1] > u; a--)
                    v[a] = v[a - 1];
                  v[a] = u;
                  n++;
                }
            }
        if (!n)
          {
            j->outrow[(size_t) x * oc + c] = NAN;
            continue;
          }
        m = n & 1 ? v[n / 2] : 0.5f * (v[n / 2 - 1] + v[n / 2]);
        for (i = 0; i < n; i++)
          sum += v[i], s2 += v[i] * v[i];
        sd = n > 1 ? sqrtf (fmaxf (0, (s2 - sum * sum / n) / (n - 1))) : 0;
        /* Winsorized: the outliers pulled in to 1.5 sigma, the spread
           worked out again, until it settles. */
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
        sum = 0;
        for (i = 0; i < n; i++)
          if (n < 3 || fabsf (v[i] - m) <= KAPPA * sd)
            sum += v[i], kept++;
        j->clipped += n - kept;
        j->values += n;
        j->outrow[(size_t) x * oc + c] = kept ? sum / kept : m;
      }
  if (fits_writer_row (j->wr, j->cy, j->outrow))
    {
      fail (j, "Can't write %s (is the disc full?)%s", leaf (j->out), NULL);
      return -1;
    }
  if (++j->cy >= j->y0 + BAND || j->cy >= j->h)
    {
      j->y0 += BAND;
      j->wf = 0;
      if (j->y0 >= j->h)
        {
          int rc = fits_writer_close (j->wr);
          j->wr = NULL;
          if (rc)
            {
              fail (j, "Can't write %s (is the disc full?)%s", leaf (j->out), NULL);
              return -1;
            }
          for (k = 0; k < nl; k++)
            close_frame (&j->fr[k]);
          j->phase = P_DONE;
        }
      else
        j->phase = P_WARP;
    }
  return 0;
}

int stack_step (stack_job *j, unsigned (*now) (void), unsigned deadline)
{
  do
    {
      int rc = 0;
      switch (j->phase)
        {
        case P_DARK: rc = dark_step (j); break;
        case P_STARS: rc = stars_step (j); break;
        case P_MATCH: rc = match_all (j); break;
        case P_OPEN: rc = open_all (j); break;
        case P_WARP: rc = warp_step (j); break;
        case P_COMBINE: rc = combine_step (j); break;
        case P_DONE: return 1;
        default: return -1;
        }
      if (rc < 0 || j->phase == P_FAILED)
        {
          j->phase = P_FAILED;
          if (j->wr)
            {
              fits_writer_close (j->wr);
              j->wr = NULL;
            }
          remove (j->out);
          return -1;
        }
    }
  while ((int) (deadline - now ()) > 0);
  return j->phase == P_DONE ? 1 : 0;
}

const char *stack_error (const stack_job *j)
{
  return j->err;
}

int stack_progress (const stack_job *j, char *what, size_t n)
{
  /* Darks a tenth, stars four tenths, stacking half. */
  double dark = j->nd ? 10 : 0, stars = j->nd ? 40 : 45, pct = 0;
  switch (j->phase)
    {
    case P_DARK:
      pct = dark * j->dark_row / (j->planes * j->h + 1);
      snprintf (what, n, "Stacking: making the master dark from %d dark%s", j->nd, j->nd == 1 ? "" : "s");
      break;
    case P_STARS:
      pct = dark + stars * (j->k + (j->sub == 1 ? 0.7 * j->py / (j->ph + 1) : j->sub == 2 ? 0.8 : 0)) / j->nl;
      snprintf (what, n, "Stacking: finding the stars in frame %d of %d", j->k + 1, j->nl);
      break;
    case P_MATCH:
    case P_OPEN:
      pct = dark + stars;
      snprintf (what, n, "Stacking: lining up the frames");
      break;
    case P_WARP:
    case P_COMBINE:
      pct = dark + stars + (100 - dark - stars) * j->y0 / j->h;
      snprintf (what, n, "Stacking %d frames: row %d of %d", j->used, j->y0, j->h);
      break;
    case P_DONE:
      pct = 100;
      snprintf (what, n, "Stacked %d frames", j->used);
      break;
    default:
      snprintf (what, n, "Stacking failed");
    }
  return (int) pct;
}

int stack_frames (const stack_job *j, const stack_frame **frames)
{
  static stack_frame *list;
  static int cap;
  int k;
  if (cap < j->nl)
    {
      free (list);
      list = malloc ((size_t) j->nl * sizeof *list);
      cap = list ? j->nl : 0;
    }
  if (!list)
    return 0;
  for (k = 0; k < j->nl; k++)
    list[k] = j->fr[k].rep;
  *frames = list;
  return j->nl;
}

void stack_summary (const stack_job *j, char *out, size_t n)
{
  int left = j->nl - j->used;
  double pct = j->values ? 100.0 * j->clipped / j->values : 0;
  if (j->phase != P_DONE)
    {
      snprintf (out, n, "%s", j->phase == P_FAILED ? j->err : "Not finished.");
      return;
    }
  if (j->nd)
    snprintf (out, n, "%d of %d frames stacked, with a master dark from %d dark%s, lined up to %s; %.1f%% of values left out by sigma clipping.",
              j->used, j->nl, j->nd, j->nd == 1 ? "" : "s", leaf (j->fr[j->ref].rep.path), pct);
  else
    snprintf (out, n, "%d of %d frames stacked (no darks), lined up to %s; %.1f%% of values left out by sigma clipping.",
              j->used, j->nl, leaf (j->fr[j->ref].rep.path), pct);
  if (left)
    {
      size_t len = strlen (out);
      snprintf (out + len, n - len, " %d left out: see the list.", left);
    }
}

const char *stack_output (const stack_job *j)
{
  return j->out;
}

void stack_free (stack_job *j)
{
  int k;
  if (!j)
    return;
  if (j->wr)
    {
      fits_writer_close (j->wr);
      remove (j->out);
    }
  if (j->phase != P_DONE && j->phase != P_FAILED && j->phase >= P_WARP)
    remove (j->out);
  for (k = 0; j->fr && k < j->nl; k++)
    {
      close_frame (&j->fr[k]);
      free (j->fr[k].stars);
      free (j->fr[k].win);
      free (j->fr[k].band);
    }
  if (j->dr)
    for (k = 0; k < j->nd; k++)
      {
        fits_reader_free (j->dr[k]);
        if (j->df && j->df[k])
          fclose (j->df[k]);
      }
  free (j->dr); free (j->df); free (j->drows);
  astro_find_free (j->finder);
  free (j->plane); free (j->rowbuf); free (j->samples);
  free (j->mdark);
  free (j->outrow);
  free (j->dark_path);
  free (j->fr);
  free (j);
}
