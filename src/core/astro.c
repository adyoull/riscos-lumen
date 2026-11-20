/* Copyright (c) 2026 Andrew Youll
   Released under the MIT licence: see the LICENSE file.
   SPDX-License-Identifier: MIT */
/* astro.c: see astro.h. */
#include <math.h>
#include <stdlib.h>
#include <string.h>
#include "astro.h"

#define GRID 32                        /* the sky model's cells, plane pixels */
#define WIN 4                          /* a star is measured within this many pixels of its peak */
#define MAX_CAND 20000                 /* peaks considered (the brightest kept) */
#define TRI_STARS 30                   /* the brightest stars make the triangles */
#define TRI_EPS 0.006                  /* how alike two triangles' side ratios must be */
#define TRI_MIN_SIDE 12.0              /* pixels: shorter triangles are too uncertain */
#define SCALE_LO 0.9                   /* frames from the same camera and lens: about the same scale */
#define SCALE_HI 1.1
#define MATCH_TOL 2.5                  /* pixels: a star agrees if it's this near where it should be */
#define MIN_PAIRS 6

/* --- the sky --- */

static int cmp_f (const void *a, const void *b)
{
  float x = *(const float *) a, y = *(const float *) b;
  return x < y ? -1 : x > y;
}

static float median (float *v, size_t n)
{
  if (!n)
    return 0;
  qsort (v, n, sizeof *v, cmp_f);
  return n & 1 ? v[n / 2] : 0.5f * (v[n / 2 - 1] + v[n / 2]);
}

/* The sky at (x, y): the cells' centres interpolated. */
static float sky_at (const float *g, int gw, int gh, float x, float y)
{
  float fx = x / GRID - 0.5f, fy = y / GRID - 0.5f;
  int x0, y0, x1, y1;
  float ax, ay;
  if (fx < 0) fx = 0;
  if (fy < 0) fy = 0;
  if (fx > gw - 1) fx = (float) (gw - 1);
  if (fy > gh - 1) fy = (float) (gh - 1);
  x0 = (int) fx; y0 = (int) fy;
  x1 = x0 + 1 < gw ? x0 + 1 : x0;
  y1 = y0 + 1 < gh ? y0 + 1 : y0;
  ax = fx - x0; ay = fy - y0;
  return (g[(size_t) y0 * gw + x0] * (1 - ax) + g[(size_t) y0 * gw + x1] * ax) * (1 - ay)
         + (g[(size_t) y1 * gw + x0] * (1 - ax) + g[(size_t) y1 * gw + x1] * ax) * ay;
}

static int cmp_flux (const void *a, const void *b)
{
  const astro_star *s = a, *t = b;
  return s->flux > t->flux ? -1 : s->flux < t->flux;
}

/* --- finding stars --- */

/* Finding stars a little at a time: the sky model a row of cells at a
   time, then the noise, then the stars a few rows at a time. */
struct astro_finder
{
  astro_plane p;
  double sigma;
  int phase;                  /* 0 the cells, 1 the noise, 2 the stars, 3 done */
  int gw, gh, cy, y;
  float *g, *g2, *tmp, *skyrow, noise;
  size_t cap;
  astro_star *c;
  int n, hot;
};

astro_finder *astro_find_begin (const astro_plane *p, double sigma)
{
  astro_finder *f = calloc (1, sizeof *f);
  if (!f)
    return NULL;
  f->p = *p;
  f->sigma = sigma > 0 ? sigma : 5;
  if (p->w < 2 * WIN + 3 || p->h < 2 * WIN + 3)
    {
      f->phase = 3;
      return f;
    }
  f->gw = (p->w + GRID - 1) / GRID;
  f->gh = (p->h + GRID - 1) / GRID;
  f->cap = (size_t) GRID * GRID;
  if ((size_t) p->w * p->h / 16 + 16 > f->cap)
    f->cap = (size_t) p->w * p->h / 16 + 16;
  f->g = malloc ((size_t) f->gw * f->gh * sizeof *f->g);
  f->g2 = malloc ((size_t) f->gw * f->gh * sizeof *f->g2);
  f->tmp = malloc (f->cap * sizeof *f->tmp);
  f->skyrow = malloc ((size_t) p->w * sizeof *f->skyrow);
  f->c = malloc (MAX_CAND * sizeof *f->c);
  if (!f->g || !f->g2 || !f->tmp || !f->skyrow || !f->c)
    {
      astro_find_free (f);
      return NULL;
    }
  f->y = WIN;
  return f;
}

/* The k-th smallest of v[0..n-1] (v is reordered). */
static float select_k (float *v, size_t n, size_t k)
{
  size_t lo = 0, hi = n - 1;
  while (lo < hi)
    {
      float pivot = v[(lo + hi) / 2];
      size_t i = lo, j = hi;
      while (i <= j)
        {
          while (v[i] < pivot) i++;
          while (v[j] > pivot) j--;
          if (i <= j)
            {
              float t = v[i]; v[i] = v[j]; v[j] = t;
              i++;
              if (j == 0)
                break;
              j--;
            }
        }
      if (k <= j)
        hi = j;
      else if (k >= i)
        lo = i;
      else
        break;
    }
  return v[k];
}

static void finder_cells (astro_finder *f)
{
  const astro_plane *p = &f->p;
  int cx, x, y, cy = f->cy;
  for (cx = 0; cx < f->gw; cx++)
    {
      size_t n = 0;
      for (y = cy * GRID; y < (cy + 1) * GRID && y < p->h; y++)
        for (x = cx * GRID; x < (cx + 1) * GRID && x < p->w; x++)
          {
            float v = p->px[(size_t) y * p->w + x];
            if (v == v)
              f->tmp[n++] = v;
          }
      f->g[(size_t) cy * f->gw + cx] = n ? select_k (f->tmp, n, n / 2) : 0;
    }
}

static void finder_noise (astro_finder *f)
{
  const astro_plane *p = &f->p;
  int cx, cy, x, y;
  size_t n;
  for (cy = 0; cy < f->gh; cy++)
    for (cx = 0; cx < f->gw; cx++)
      {
        int dx, dy;
        n = 0;
        for (dy = -1; dy <= 1; dy++)
          for (dx = -1; dx <= 1; dx++)
            if (cy + dy >= 0 && cy + dy < f->gh && cx + dx >= 0 && cx + dx < f->gw)
              f->tmp[n++] = f->g[(size_t) (cy + dy) * f->gw + cx + dx];
        f->g2[(size_t) cy * f->gw + cx] = median (f->tmp, n);
      }
  n = 0;
  for (y = 0; y < p->h; y += 4)
    for (x = 0; x < p->w; x += 4)
      {
        float v = p->px[(size_t) y * p->w + x];
        if (v == v && n < f->cap)
          f->tmp[n++] = fabsf (v - f->g2[(size_t) (y / GRID) * f->gw + x / GRID]);
      }
  f->noise = n ? 1.4826f * select_k (f->tmp, n, n / 2) : 0;
  if (!(f->noise > 0))
    f->noise = 1e-6f;                  /* a picture with no noise at all (a test's) */
}

static void finder_row (astro_finder *f, int y)
{
  const astro_plane *p = &f->p;
  int w = p->w, h = p->h, x, k, gw = f->gw, gh = f->gh;
  float noise = f->noise, *skyrow = f->skyrow, *g = f->g2;
  double sigma = f->sigma;
  const float *row = p->px + (size_t) y * w;
  astro_star *c = f->c;
  for (x = 0; x < w; x++)
    skyrow[x] = sky_at (g, gw, gh, (float) x, (float) y);
  for (x = WIN; x < w - WIN; x++)
    {
      float v = row[x], sky = skyrow[x], thr = sky + (float) sigma * noise;
      int dx, dy, peak = 1, near = 0;
      double sw = 0, sx = 0, sy = 0, sxx = 0, cxp, cyp;
      if (!(v > thr))
        continue;
      /* The highest in its 5 x 5 (ties: the first). */
      for (dy = -2; dy <= 2 && peak; dy++)
        for (dx = -2; dx <= 2; dx++)
          {
            float u = p->px[(size_t) (y + dy) * w + x + dx];
            if ((dy || dx) && (u > v || (u == v && (dy < 0 || (dy == 0 && dx < 0)))))
              {
                peak = 0;
                break;
              }
          }
      if (!peak)
        continue;
      /* A star spreads into its neighbours; a hot pixel or a cosmic
         ray doesn't. */
      for (dy = -1; dy <= 1; dy++)
        for (dx = -1; dx <= 1; dx++)
          if ((dy || dx) && p->px[(size_t) (y + dy) * w + x + dx] > sky + 2 * noise)
            near++;
      if (near < 2)
        {
          f->hot++;
          continue;
        }
      /* The centre of its light, twice (the second time around the first's centre). */
      cxp = x;
      cyp = y;
      for (k = 0; k < 2; k++)
        {
          int ix = (int) lround (cxp), iy = (int) lround (cyp);
          sw = sx = sy = sxx = 0;
          if (ix < WIN || iy < WIN || ix >= w - WIN || iy >= h - WIN)
            break;
          for (dy = -WIN; dy <= WIN; dy++)
            for (dx = -WIN; dx <= WIN; dx++)
              {
                double u = p->px[(size_t) (iy + dy) * w + ix + dx] - sky;
                if (u > 0 && u == u && dx * dx + dy * dy <= WIN * WIN)
                  {
                    sw += u;
                    sx += u * (ix + dx);
                    sy += u * (iy + dy);
                  }
              }
          if (sw <= 0)
            break;
          cxp = sx / sw;
          cyp = sy / sw;
        }
      if (sw <= 0 || k < 2)
        continue;
      {
        int ix = (int) lround (cxp), iy = (int) lround (cyp);
        double fw;
        for (dy = -WIN; dy <= WIN; dy++)
          for (dx = -WIN; dx <= WIN; dx++)
            {
              double u = p->px[(size_t) (iy + dy) * w + ix + dx] - sky;
              double ddx = ix + dx - cxp, ddy = iy + dy - cyp;
              if (u > 0 && u == u && dx * dx + dy * dy <= WIN * WIN)
                sxx += u * (ddx * ddx + ddy * ddy);
            }
        fw = 2.3548 * sqrt (sxx / sw / 2);
        if (fw > 2 * WIN)
          continue;                /* a blob (a galaxy's core, a bit of nebula), not a star */
        /* Long and thin: a satellite's or a plane's trail, not a star
           (stars trailed a little by a fixed tripod are kept). */
        {
          double mxx = 0, myy = 0, mxy = 0, tr, det, l1, l2;
          for (dy = -WIN; dy <= WIN; dy++)
            for (dx = -WIN; dx <= WIN; dx++)
              {
                double u = p->px[(size_t) (iy + dy) * w + ix + dx] - sky;
                double ddx = ix + dx - cxp, ddy = iy + dy - cyp;
                if (u > 0 && u == u && dx * dx + dy * dy <= WIN * WIN)
                  mxx += u * ddx * ddx, myy += u * ddy * ddy, mxy += u * ddx * ddy;
              }
          tr = mxx + myy;
          det = mxx * myy - mxy * mxy;
          l1 = tr / 2 + sqrt (tr * tr / 4 - det > 0 ? tr * tr / 4 - det : 0);
          l2 = tr - l1;
          if (l2 <= 0 || l1 / l2 > 9)
            continue;              /* more than three times as long as wide */
        }
        /* Refined with a Gaussian window the star's width (SExtractor's
           XWIN): unbiased by the noise around it, which the plain
           centre of light is pulled towards its peak pixel by. */
        {
          double sg = fw / 2.3548, it;
          if (sg < 0.7) sg = 0.7;
          if (sg > 3) sg = 3;
          for (it = 0; it < 6; it++)
            {
              double ww = 0, wx = 0, wy = 0, nx, ny;
              int jx = (int) lround (cxp), jy = (int) lround (cyp);
              if (jx < WIN || jy < WIN || jx >= w - WIN || jy >= h - WIN)
                break;
              for (dy = -WIN; dy <= WIN; dy++)
                for (dx = -WIN; dx <= WIN; dx++)
                  {
                    double u = p->px[(size_t) (jy + dy) * w + jx + dx] - sky;
                    double ddx = jx + dx - cxp, ddy = jy + dy - cyp;
                    double g = exp (-(ddx * ddx + ddy * ddy) / (2 * sg * sg)) * u;
                    if (u == u)
                      ww += g, wx += g * ddx, wy += g * ddy;
                  }
              if (ww <= 0)
                break;
              nx = cxp + 2 * wx / ww;
              ny = cyp + 2 * wy / ww;
              if (fabs (nx - x) > WIN || fabs (ny - y) > WIN)
                break;
              cxp = nx;
              cyp = ny;
            }
        }
        if (f->n == MAX_CAND)
          {
            /* Too many: keep the brightest half and carry on. */
            qsort (c, (size_t) f->n, sizeof *c, cmp_flux);
            f->n /= 2;
          }
        c[f->n].x = (float) ((cxp + 0.5) * p->scale - 0.5);
        c[f->n].y = (float) ((cyp + 0.5) * p->scale - 0.5);
        c[f->n].flux = (float) sw;
        c[f->n].peak = v - sky;
        c[f->n].fwhm = (float) (fw * p->scale);
        f->n++;
      }
    }
}

int astro_find_step (astro_finder *f, int rows)
{
  while (rows-- > 0)
    switch (f->phase)
      {
      case 0:
        finder_cells (f);
        if (++f->cy >= f->gh)
          f->phase = 1;
        break;
      case 1:
        finder_noise (f);
        f->phase = 2;
        rows -= 8;                     /* about as much work as a few rows of cells */
        break;
      case 2:
        if (f->y < f->p.h - WIN)
          finder_row (f, f->y++);
        if (f->y >= f->p.h - WIN)
          f->phase = 3;
        break;
      default:
        return 1;
      }
  return f->phase == 3;
}

int astro_find_end (astro_finder *f, astro_star *out, int max, astro_stats *st)
{
  int n;
  astro_stats dummy;
  if (!st)
    st = &dummy;
  memset (st, 0, sizeof *st);
  while (!astro_find_step (f, 1 << 20))
    ;
  if (!f->c)
    return 0;
  qsort (f->c, (size_t) f->n, sizeof *f->c, cmp_flux);
  n = f->n < max ? f->n : max;
  memcpy (out, f->c, (size_t) n * sizeof *out);
  st->found = n;
  st->rejected_hot = f->hot;
  st->noise = f->noise;
  st->sky = sky_at (f->g2, f->gw, f->gh, f->p.w / 2.0f, f->p.h / 2.0f);
  return n;
}

void astro_find_free (astro_finder *f)
{
  if (!f)
    return;
  free (f->g); free (f->g2); free (f->tmp); free (f->skyrow); free (f->c);
  free (f);
}

int astro_find_stars (const astro_plane *p, double sigma, astro_star *out, int max, astro_stats *st)
{
  astro_finder *f = astro_find_begin (p, sigma);
  int n;
  if (!f)
    return -1;
  n = astro_find_end (f, out, max, st);
  astro_find_free (f);
  return n;
}

/* --- transforms --- */

astro_xform astro_identity (void)
{
  astro_xform t = { 1, 0, 0, 0 };
  return t;
}

void astro_apply (const astro_xform *t, double x, double y, double *ox, double *oy)
{
  *ox = t->a * x - t->b * y + t->tx;
  *oy = t->b * x + t->a * y + t->ty;
}

/* The least-squares similarity taking (x[i], y[i]) to (u[i], v[i]). */
static int fit_similarity (const double *x, const double *y, const double *u, const double *v, int n, astro_xform *t)
{
  double mx = 0, my = 0, mu = 0, mv = 0, sa = 0, sb = 0, ss = 0;
  int i;
  if (n < 1)
    return -1;
  for (i = 0; i < n; i++)
    mx += x[i], my += y[i], mu += u[i], mv += v[i];
  mx /= n; my /= n; mu /= n; mv /= n;
  if (n == 1)
    {
      t->a = 1; t->b = 0;
      t->tx = mu - mx; t->ty = mv - my;
      return 0;
    }
  for (i = 0; i < n; i++)
    {
      double xc = x[i] - mx, yc = y[i] - my, uc = u[i] - mu, vc = v[i] - mv;
      sa += xc * uc + yc * vc;
      sb += xc * vc - yc * uc;
      ss += xc * xc + yc * yc;
    }
  if (ss < 1e-9)
    return -1;
  t->a = sa / ss;
  t->b = sb / ss;
  t->tx = mu - t->a * mx + t->b * my;
  t->ty = mv - t->b * mx - t->a * my;
  return 0;
}

int astro_xform_from_pairs (const double *r, const double *f, int n, astro_xform *t)
{
  double x[64], y[64], u[64], v[64];
  int i;
  if (n < 1 || n > 64)
    return -1;
  for (i = 0; i < n; i++)
    x[i] = r[2 * i], y[i] = r[2 * i + 1], u[i] = f[2 * i], v[i] = f[2 * i + 1];
  return fit_similarity (x, y, u, v, n, t);
}

/* --- matching --- */

typedef struct
{
  float r1, r2;               /* the middle and shortest sides over the longest */
  float longest;
  unsigned char v[3];         /* the stars opposite the longest, middle and shortest sides */
} tri;

static int cmp_tri (const void *a, const void *b)
{
  float x = ((const tri *) a)->r1, y = ((const tri *) b)->r1;
  return x < y ? -1 : x > y;
}

static int triangles (const astro_star *s, int n, tri *out)
{
  int i, j, k, m = 0;
  for (i = 0; i < n; i++)
    for (j = i + 1; j < n; j++)
      for (k = j + 1; k < n; k++)
        {
          /* d[0] is the side opposite star i (j to k), and so on. */
          double d[3], dx, dy;
          int o[3] = { 0, 1, 2 }, id[3] = { i, j, k }, a, b2;
          dx = s[j].x - s[k].x; dy = s[j].y - s[k].y; d[0] = sqrt (dx * dx + dy * dy);
          dx = s[i].x - s[k].x; dy = s[i].y - s[k].y; d[1] = sqrt (dx * dx + dy * dy);
          dx = s[i].x - s[j].x; dy = s[i].y - s[j].y; d[2] = sqrt (dx * dx + dy * dy);
          for (a = 0; a < 3; a++)
            for (b2 = a + 1; b2 < 3; b2++)
              if (d[o[b2]] > d[o[a]])
                {
                  int t = o[a];
                  o[a] = o[b2];
                  o[b2] = t;
                }
          if (d[o[0]] < TRI_MIN_SIDE || d[o[2]] < 0.1 * d[o[0]])
            continue;
          /* Nearly isosceles or equilateral: which star is which is uncertain. */
          if (d[o[0]] - d[o[1]] < 0.03 * d[o[0]] || d[o[1]] - d[o[2]] < 0.03 * d[o[0]])
            continue;
          out[m].r1 = (float) (d[o[1]] / d[o[0]]);
          out[m].r2 = (float) (d[o[2]] / d[o[0]]);
          out[m].longest = (float) d[o[0]];
          out[m].v[0] = (unsigned char) id[o[0]];
          out[m].v[1] = (unsigned char) id[o[1]];
          out[m].v[2] = (unsigned char) id[o[2]];
          m++;
        }
  return m;
}

/* How many of ref's stars t puts within tol of one of frm's; pairs[] gets them. */
static int agree (const astro_star *ref, int nref, const astro_star *frm, int nfrm, const astro_xform *t, double tol,
                  int *pairs, double *sum2)
{
  int i, j, n = 0;
  double tol2 = tol * tol;
  *sum2 = 0;
  for (i = 0; i < nref; i++)
    {
      double px, py, best = tol2;
      int bj = -1;
      astro_apply (t, ref[i].x, ref[i].y, &px, &py);
      for (j = 0; j < nfrm; j++)
        {
          double dx = frm[j].x - px, dy = frm[j].y - py, d = dx * dx + dy * dy;
          if (d < best)
            best = d, bj = j;
        }
      if (bj >= 0)
        {
          if (pairs)
            pairs[2 * n] = i, pairs[2 * n + 1] = bj;
          *sum2 += best;
          n++;
        }
    }
  return n;
}

static int refit (const astro_star *ref, const astro_star *frm, const int *pairs, int n, astro_xform *t)
{
  double *x = malloc ((size_t) n * 4 * sizeof *x);
  int i, r;
  if (!x)
    return -1;
  for (i = 0; i < n; i++)
    {
      x[i] = ref[pairs[2 * i]].x;
      x[n + i] = ref[pairs[2 * i]].y;
      x[2 * n + i] = frm[pairs[2 * i + 1]].x;
      x[3 * n + i] = frm[pairs[2 * i + 1]].y;
    }
  r = fit_similarity (x, x + n, x + 2 * n, x + 3 * n, n, t);
  free (x);
  return r;
}

int astro_match (const astro_star *ref, int nref, const astro_star *frm, int nfrm, astro_xform *t, astro_fit *fit)
{
  int kr = nref < TRI_STARS ? nref : TRI_STARS, kf = nfrm < TRI_STARS ? nfrm : TRI_STARS;
  int maxtri = TRI_STARS * (TRI_STARS - 1) * (TRI_STARS - 2) / 6;
  tri *tr = malloc ((size_t) maxtri * sizeof *tr), *tf = malloc ((size_t) maxtri * sizeof *tf);
  int (*votes)[TRI_STARS] = calloc (TRI_STARS, sizeof *votes);
  int cand[TRI_STARS][2], nc = 0, ntr, ntf, i, j, k, best_n = 0, need;
  int *pairs = malloc ((size_t) (nref + 1) * 2 * sizeof *pairs);
  astro_xform best = astro_identity ();
  double s2;
  astro_fit dummy;
  if (!fit)
    fit = &dummy;
  memset (fit, 0, sizeof *fit);
  if (!tr || !tf || !votes || !pairs)
    {
      free (tr); free (tf); free (votes); free (pairs);
      return -1;
    }
  need = nref < nfrm ? nref : nfrm;
  if (need > MIN_PAIRS)
    need = MIN_PAIRS;
  if (need < 3)
    goto fail;
  /* Triangles that look alike vote for their stars being the same. */
  ntr = triangles (ref, kr, tr);
  ntf = triangles (frm, kf, tf);
  qsort (tf, (size_t) ntf, sizeof *tf, cmp_tri);
  for (i = 0; i < ntr; i++)
    {
      int lo = 0, hi = ntf;
      while (lo < hi)
        {
          int mid = (lo + hi) / 2;
          if (tf[mid].r1 < tr[i].r1 - TRI_EPS)
            lo = mid + 1;
          else
            hi = mid;
        }
      for (j = lo; j < ntf && tf[j].r1 <= tr[i].r1 + TRI_EPS; j++)
        {
          float s = tf[j].longest / tr[i].longest;
          if (fabsf (tf[j].r2 - tr[i].r2) > TRI_EPS || s < SCALE_LO || s > SCALE_HI)
            continue;
          for (k = 0; k < 3; k++)
            votes[tr[i].v[k]][tf[j].v[k]]++;
        }
    }
  /* Each reference star's most-voted partner, if it's clearly the one. */
  for (i = 0; i < kr; i++)
    {
      int bj = -1, bv = 0, second = 0;
      for (j = 0; j < kf; j++)
        if (votes[i][j] > bv)
          second = bv, bv = votes[i][j], bj = j;
        else if (votes[i][j] > second)
          second = votes[i][j];
      if (bj >= 0 && bv >= 2 && bv > 2 * second)
        cand[nc][0] = i, cand[nc++][1] = bj;
    }
  /* RANSAC: the transform from each two candidate pairs, kept if most stars agree. */
  for (i = 0; i < nc; i++)
    for (j = i + 1; j < nc; j++)
      {
        double r[4], f[4];
        astro_xform h;
        int n;
        r[0] = ref[cand[i][0]].x; r[1] = ref[cand[i][0]].y; r[2] = ref[cand[j][0]].x; r[3] = ref[cand[j][0]].y;
        f[0] = frm[cand[i][1]].x; f[1] = frm[cand[i][1]].y; f[2] = frm[cand[j][1]].x; f[3] = frm[cand[j][1]].y;
        if (astro_xform_from_pairs (r, f, 2, &h))
          continue;
        {
          double sc = sqrt (h.a * h.a + h.b * h.b);
          if (sc < SCALE_LO || sc > SCALE_HI)
            continue;
        }
        n = agree (ref, nref, frm, nfrm, &h, MATCH_TOL, NULL, &s2);
        if (n > best_n)
          best_n = n, best = h;
      }
  if (best_n < need)
    goto fail;
  /* Refined over every star that agrees, twice, the second time more strictly. */
  for (k = 0; k < 2; k++)
    {
      int n = agree (ref, nref, frm, nfrm, &best, k ? 1.5 : MATCH_TOL, pairs, &s2);
      if (n < need || refit (ref, frm, pairs, n, &best))
        goto fail;
    }
  best_n = agree (ref, nref, frm, nfrm, &best, 1.5, pairs, &s2);
  if (best_n < need)
    goto fail;
  *t = best;
  fit->pairs = best_n;
  fit->rms = sqrt (s2 / best_n);
  fit->scale = sqrt (best.a * best.a + best.b * best.b);
  fit->angle = -atan2 (best.b, best.a) * 180 / M_PI;   /* y is down: anticlockwise as shown is negative */
  free (tr); free (tf); free (votes); free (pairs);
  return 0;
fail:
  free (tr); free (tf); free (votes); free (pairs);
  return -1;
}

/* --- moving a frame --- */

/* Catmull-Rom weights for a fraction t. */
static inline void cubic_w (float t, float *k)
{
  float t2 = t * t, t3 = t2 * t;
  k[0] = 0.5f * (-t3 + 2 * t2 - t);
  k[1] = 0.5f * (3 * t3 - 5 * t2 + 2);
  k[2] = 0.5f * (-3 * t3 + 4 * t2 + t);
  k[3] = 0.5f * (t3 - t2);
}

void astro_warp (const astro_img *src, const astro_xform *t, astro_img *dst, int y0, int y1)
{
  int w = src->w, h = src->h, ch = src->ch, x, y, c, i, j;
  size_t stride = (size_t) w * ch;
  int sy0 = src->rows ? src->y0 : 0, srows = src->rows ? src->rows : h;
  if (t->a == 1 && t->b == 0 && t->tx == 0 && t->ty == 0 && dst->w == w && dst->ch == ch)
    {
      /* The reference itself: as it is. */
      for (y = y0; y < y1; y++)
        if (y >= sy0 && y < sy0 + srows && y < h)
          memcpy (dst->px + (size_t) (y - (dst->rows ? dst->y0 : 0)) * stride, src->px + (size_t) (y - sy0) * stride,
                  stride * sizeof (float));
      return;
    }
  for (y = y0; y < y1; y++)
    {
      double sx = -t->b * y + t->tx, sy = t->a * y + t->ty;
      float *o = dst->px + (size_t) (y - (dst->rows ? dst->y0 : 0)) * dst->w * ch;
      for (x = 0; x < dst->w; x++, sx += t->a, sy += t->b, o += ch)
        {
          int ix, iy;
          float kx[4], ky[4];
          const float *rows[4];
          size_t cols[4];
          if (!(sx >= 0 && sy >= 0 && sx <= w - 1 && sy <= h - 1))
            {
              for (c = 0; c < ch; c++)
                o[c] = NAN;
              continue;
            }
          ix = (int) sx;
          iy = (int) sy;
          cubic_w ((float) (sx - ix), kx);
          cubic_w ((float) (sy - iy), ky);
          for (j = 0; j < 4; j++)
            {
              int xx = ix - 1 + j, yy = iy - 1 + j;
              cols[j] = (size_t) (xx < 0 ? 0 : xx >= w ? w - 1 : xx) * ch;
              yy = yy < 0 ? 0 : yy >= h ? h - 1 : yy;
              yy -= sy0;
              yy = yy < 0 ? 0 : yy >= srows ? srows - 1 : yy;
              rows[j] = src->px + (size_t) yy * stride;
            }
          if (ch == 3)
            {
              /* RGB together: the 16 weights once, three sums. */
              float r0 = 0, g0 = 0, b0 = 0, k;
              const float *q;
              for (j = 0; j < 4; j++)
                for (i = 0; i < 4; i++)
                  {
                    q = rows[j] + cols[i];
                    k = kx[i] * ky[j];
                    r0 += k * q[0];
                    g0 += k * q[1];
                    b0 += k * q[2];
                  }
              {
                const float *a = rows[1] + cols[1], *b = rows[1] + cols[2], *d = rows[2] + cols[1], *e = rows[2] + cols[2];
                float v3[3] = { r0, g0, b0 };
                for (c = 0; c < 3; c++)
                  {
                    float lo = fminf (fminf (a[c], b[c]), fminf (d[c], e[c])), hi = fmaxf (fmaxf (a[c], b[c]), fmaxf (d[c], e[c]));
                    o[c] = v3[c] < lo ? lo : v3[c] > hi ? hi : v3[c];
                  }
              }
              continue;
            }
          for (c = 0; c < ch; c++)
            {
              float v = 0, a = rows[1][cols[1] + c], b = rows[1][cols[2] + c], d = rows[2][cols[1] + c], e = rows[2][cols[2] + c];
              float lo = fminf (fminf (a, b), fminf (d, e)), hi = fmaxf (fmaxf (a, b), fmaxf (d, e));
              for (j = 0; j < 4; j++)
                {
                  const float *r = rows[j] + c;
                  float row = 0;
                  for (i = 0; i < 4; i++)
                    row += kx[i] * r[cols[i]];
                  v += ky[j] * row;
                }
              /* Within the four nearest pixels' range: no dark rings round stars. */
              o[c] = v < lo ? lo : v > hi ? hi : v;
            }
        }
    }
}
