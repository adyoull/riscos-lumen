/* Copyright (c) 2026 Andrew Youll
   Released under the MIT licence: see the LICENSE file.
   SPDX-License-Identifier: MIT */
/* retouch.c: see retouch.h. */
#include <math.h>
#include <stdlib.h>
#include <string.h>
#include "retouch.h"

#define PI 3.14159265358979
#define MASK_GRID_MAX 2048             /* a mask's coverage: cells across the longer side, at most */

/* A picture of w x h, channel values read and written through get and put
   (0..maxv). */
typedef struct
{
  int w, h, maxv;
  double (*get) (const void *img, int w, int x, int y, int c);
  void (*put) (void *img, int w, int x, int y, int c, double v);
  void *img;
} pic;

static double get8 (const void *img, int w, int x, int y, int c)
{
  return ((const unsigned char *) img)[((size_t) y * w + x) * 3 + c];
}

static void put8 (void *img, int w, int x, int y, int c, double v)
{
  ((unsigned char *) img)[((size_t) y * w + x) * 3 + c] = (unsigned char) (v < 0 ? 0 : v > 255 ? 255 : v + 0.5);
}

static double get16 (const void *img, int w, int x, int y, int c)
{
  return ((const uint16_t *) img)[((size_t) y * w + x) * 3 + c];
}

static void put16 (void *img, int w, int x, int y, int c, double v)
{
  ((uint16_t *) img)[((size_t) y * w + x) * 3 + c] = (uint16_t) (v < 0 ? 0 : v > 65535 ? 65535 : v + 0.5);
}

static int clampi (int v, int lo, int hi)
{
  return v < lo ? lo : v > hi ? hi : v;
}

static double smooth (double m)
{
  return m <= 0 ? 0 : m >= 1 ? 1 : m * m * (3 - 2 * m);
}

/* How far (x, y) is from the segment a-b. */
static double seg_dist (double x, double y, double ax, double ay, double bx, double by)
{
  double dx = bx - ax, dy = by - ay, l = dx * dx + dy * dy, t = 0;
  if (l > 0)
    {
      t = ((x - ax) * dx + (y - ay) * dy) / l;
      t = t < 0 ? 0 : t > 1 ? 1 : t;
    }
  dx = ax + t * dx - x;
  dy = ay + t * dy - y;
  return sqrt (dx * dx + dy * dy);
}

/* A path in pixels: n points (n >= 1); how far (x, y) is from it. */
static double path_dist (const double *pts, int n, double x, double y)
{
  int j;
  double best;
  if (n == 1)
    return sqrt ((x - pts[0]) * (x - pts[0]) + (y - pts[1]) * (y - pts[1]));
  best = 1e30;
  for (j = 0; j + 1 < n; j++)
    {
      double d = seg_dist (x, y, pts[2 * j], pts[2 * j + 1], pts[2 * j + 2], pts[2 * j + 3]);
      if (d < best)
        best = d;
    }
  return best;
}

/* The mean of the 3 x 3 pixels round (x, y), each channel. */
static void sample3 (const pic *p, double x, double y, double out[3])
{
  int cx = (int) floor (x + 0.5), cy = (int) floor (y + 0.5), dx, dy, c;
  out[0] = out[1] = out[2] = 0;
  for (dy = -1; dy <= 1; dy++)
    for (dx = -1; dx <= 1; dx++)
      {
        int sx = clampi (cx + dx, 0, p->w - 1), sy = clampi (cy + dy, 0, p->h - 1);
        for (c = 0; c < 3; c++)
          out[c] += p->get (p->img, p->w, sx, sy, c);
      }
  for (c = 0; c < 3; c++)
    out[c] /= 9;
}

/* --- spots */

/* A spot's path in pixels (points: room for RECIPE_STROKE_MAX); how many. */
static int spot_path (const pic *p, const recipe *r, const recipe_spot *s, double *pts)
{
  int j, n = s->n >= 2 ? s->n : 1;
  if (n == 1)
    {
      pts[0] = s->x * p->w;
      pts[1] = s->y * p->h;
      return 1;
    }
  for (j = 0; j < n; j++)
    {
      pts[2 * j] = r->spt[s->first + j][0] / 10000.0 * p->w;
      pts[2 * j + 1] = r->spt[s->first + j][1] / 10000.0 * p->h;
    }
  return n;
}

/* Healing's field: target - source at the edge cells, smoothed through
   the inside cells (Gauss-Seidel, over-relaxed, until it settles). */
typedef struct
{
  int gw, gh, cs, gx0, gy0;
  unsigned char *kind;                 /* 0 neither, 1 inside, 2 edge */
  float *v;                            /* 3 a cell */
} field;

static int heal_field (field *f, const pic *p, const double *pts, int n, double R, double ox, double oy,
                       int x0, int y0, int x1, int y1)
{
  int i, j, c, it, ninside = 0;
  double cs;
  f->cs = (int) floor (R / 5);
  if (f->cs < 1)
    f->cs = 1;
  cs = f->cs;
  f->gx0 = x0 - f->cs;
  f->gy0 = y0 - f->cs;
  f->gw = (x1 - x0 + 1) / f->cs + 3;
  f->gh = (y1 - y0 + 1) / f->cs + 3;
  f->kind = calloc ((size_t) f->gw * f->gh, 1);
  f->v = calloc ((size_t) f->gw * f->gh * 3, sizeof *f->v);
  if (!f->kind || !f->v)
    return -1;
  /* Inside: the cell's middle is within the brush (and half a cell). */
  for (j = 0; j < f->gh; j++)
    for (i = 0; i < f->gw; i++)
      {
        double cx = f->gx0 + (i + 0.5) * cs, cy = f->gy0 + (j + 0.5) * cs;
        if (path_dist (pts, n, cx, cy) < R + 0.5 * cs)
          {
            f->kind[(size_t) j * f->gw + i] = 1;
            ninside++;
          }
      }
  if (!ninside)
    return -1;
  /* The edge: next to the inside, on the picture; its value the mean
     difference over the cell (and round it). */
  for (j = 0; j < f->gh; j++)
    for (i = 0; i < f->gw; i++)
      {
        size_t k = (size_t) j * f->gw + i;
        int a, b, cnt = 0;
        double sum[3] = { 0, 0, 0 };
        if (f->kind[k] == 1)
          continue;
        if (!((i > 0 && f->kind[k - 1] == 1) || (i + 1 < f->gw && f->kind[k + 1] == 1)
              || (j > 0 && f->kind[k - f->gw] == 1) || (j + 1 < f->gh && f->kind[k + f->gw] == 1)))
          continue;
        {
          int bx0 = f->gx0 + i * f->cs - 1, by0 = f->gy0 + j * f->cs - 1, step = f->cs > 4 ? f->cs / 4 : 1;
          for (b = by0; b <= by0 + f->cs + 1; b += step)
            for (a = bx0; a <= bx0 + f->cs + 1; a += step)
              {
                int sx = (int) floor (a + ox + 0.5), sy = (int) floor (b + oy + 0.5);
                if (a < 0 || b < 0 || a >= p->w || b >= p->h || sx < 0 || sy < 0 || sx >= p->w || sy >= p->h)
                  continue;
                for (c = 0; c < 3; c++)
                  sum[c] += p->get (p->img, p->w, a, b, c) - p->get (p->img, p->w, sx, sy, c);
                cnt++;
              }
        }
        if (!cnt)
          continue;                    /* off the picture: no condition there */
        f->kind[k] = 2;
        for (c = 0; c < 3; c++)
          f->v[k * 3 + c] = (float) (sum[c] / cnt);
      }
  /* Start the inside at the edge's mean, then relax. */
  {
    double mean[3] = { 0, 0, 0 };
    int ne = 0;
    size_t k;
    for (k = 0; k < (size_t) f->gw * f->gh; k++)
      if (f->kind[k] == 2)
        {
          for (c = 0; c < 3; c++)
            mean[c] += f->v[k * 3 + c];
          ne++;
        }
    if (!ne)
      return -1;
    for (k = 0; k < (size_t) f->gw * f->gh; k++)
      if (f->kind[k] == 1)
        for (c = 0; c < 3; c++)
          f->v[k * 3 + c] = (float) (mean[c] / ne);
  }
  for (it = 0; it < 600; it++)
    {
      double most = 0, scale = p->maxv / 255.0;
      for (j = 0; j < f->gh; j++)
        for (i = 0; i < f->gw; i++)
          {
            size_t k = (size_t) j * f->gw + i;
            int nb = 0;
            double s[3] = { 0, 0, 0 };
            if (f->kind[k] != 1)
              continue;
            /* Its neighbours on the grid; one that is neither (off the
               picture) doesn't count: no flow across the picture's edge. */
#define NB(q) do { size_t q_ = (q); if (f->kind[q_]) { for (c = 0; c < 3; c++) s[c] += f->v[q_ * 3 + c]; nb++; } } while (0)
            if (i > 0) NB (k - 1);
            if (i + 1 < f->gw) NB (k + 1);
            if (j > 0) NB (k - f->gw);
            if (j + 1 < f->gh) NB (k + f->gw);
#undef NB
            if (!nb)
              continue;
            for (c = 0; c < 3; c++)
              {
                double old = f->v[k * 3 + c], nv = old + 1.8 * (s[c] / nb - old);
                f->v[k * 3 + c] = (float) nv;
                if (fabs (nv - old) > most)
                  most = fabs (nv - old);
              }
          }
      if (most < 0.02 * scale)
        break;
    }
  return 0;
}

/* The field at pixel (x, y): from the cells round it that have a value. */
static void field_at (const field *f, double x, double y, double out[3])
{
  double gx = (x + 0.5 - f->gx0) / f->cs - 0.5, gy = (y + 0.5 - f->gy0) / f->cs - 0.5, wsum = 0;
  int i0 = (int) floor (gx), j0 = (int) floor (gy), a, b, c;
  double fx = gx - i0, fy = gy - j0;
  out[0] = out[1] = out[2] = 0;
  for (b = 0; b < 2; b++)
    for (a = 0; a < 2; a++)
      {
        int i = i0 + a, j = j0 + b;
        double wgt = (a ? fx : 1 - fx) * (b ? fy : 1 - fy);
        size_t k;
        if (i < 0 || j < 0 || i >= f->gw || j >= f->gh || wgt <= 0)
          continue;
        k = (size_t) j * f->gw + i;
        if (!f->kind[k])
          continue;
        for (c = 0; c < 3; c++)
          out[c] += wgt * f->v[k * 3 + c];
        wsum += wgt;
      }
  if (wsum > 0)
    for (c = 0; c < 3; c++)
      out[c] /= wsum;
}

static void apply_spot (const pic *p, const recipe *r, const recipe_spot *s)
{
  static double pts[RECIPE_STROKE_MAX * 2];
  int longer = p->w > p->h ? p->w : p->h, n, x0, x1, y0, y1, x, y, c, bw, bh, j;
  double R = s->r * longer, ox = (s->sx - s->x) * p->w, oy = (s->sy - s->y) * p->h;
  double feather = s->f < 0.02f ? 0.02 : s->f, inner;
  unsigned char *m;
  size_t count = 0, at;
  int *where;
  float *patch;
  field f;
  if (R < 0.75 || (fabs (ox) < 0.5 && fabs (oy) < 0.5))
    return;
  inner = R * (1 - feather);
  n = spot_path (p, r, s, pts);
  {
    double mx0 = 1e30, my0 = 1e30, mx1 = -1e30, my1 = -1e30;
    for (j = 0; j < n; j++)
      {
        mx0 = pts[2 * j] < mx0 ? pts[2 * j] : mx0; mx1 = pts[2 * j] > mx1 ? pts[2 * j] : mx1;
        my0 = pts[2 * j + 1] < my0 ? pts[2 * j + 1] : my0; my1 = pts[2 * j + 1] > my1 ? pts[2 * j + 1] : my1;
      }
    x0 = clampi ((int) floor (mx0 - R), 0, p->w - 1); x1 = clampi ((int) ceil (mx1 + R), 0, p->w - 1);
    y0 = clampi ((int) floor (my0 - R), 0, p->h - 1); y1 = clampi ((int) ceil (my1 + R), 0, p->h - 1);
  }
  bw = x1 - x0 + 1;
  bh = y1 - y0 + 1;
  /* How much each pixel is painted out: 1 inside, fading to 0 at the
     brush's edge; each segment in turn, the most any gives. */
  if (!(m = calloc ((size_t) bw * bh, 1)))
    return;
  for (j = 0; j < (n > 1 ? n - 1 : 1); j++)
    {
      double ax = pts[2 * j], ay = pts[2 * j + 1];
      double bx = n > 1 ? pts[2 * j + 2] : ax, by = n > 1 ? pts[2 * j + 3] : ay;
      int sx0 = clampi ((int) floor ((ax < bx ? ax : bx) - R), x0, x1), sx1 = clampi ((int) ceil ((ax > bx ? ax : bx) + R), x0, x1);
      int sy0 = clampi ((int) floor ((ay < by ? ay : by) - R), y0, y1), sy1 = clampi ((int) ceil ((ay > by ? ay : by) + R), y0, y1);
      for (y = sy0; y <= sy1; y++)
        for (x = sx0; x <= sx1; x++)
          {
            unsigned char *mp = &m[(size_t) (y - y0) * bw + (x - x0)];
            double d = seg_dist (x + 0.5, y + 0.5, ax, ay, bx, by), v;
            if (d >= R || *mp == 255)
              continue;
            v = 255 * smooth (d <= inner ? 1 : (R - d) / (R - inner));
            if (v > *mp)
              *mp = (unsigned char) (v + 0.5);
          }
    }
  for (at = 0; at < (size_t) bw * bh; at++)
    count += m[at] != 0;
  where = malloc (sizeof *where * (count ? count : 1));
  patch = malloc (sizeof *patch * 3 * (count ? count : 1));
  memset (&f, 0, sizeof f);
  if (!count || !where || !patch || (!s->clone && heal_field (&f, p, pts, n, R, ox, oy, x0, y0, x1, y1)))
    goto out;
  /* The source's pixels first: it may overlap the spot. */
  count = 0;
  for (y = y0; y <= y1; y++)
    for (x = x0; x <= x1; x++)
      if (m[(size_t) (y - y0) * bw + (x - x0)])
        {
          int sx = clampi ((int) floor (x + ox + 0.5), 0, p->w - 1), sy = clampi ((int) floor (y + oy + 0.5), 0, p->h - 1);
          where[count] = (y - y0) * bw + (x - x0);
          for (c = 0; c < 3; c++)
            patch[count * 3 + c] = (float) p->get (p->img, p->w, sx, sy, c);
          count++;
        }
  for (at = 0; at < count; at++)
    {
      int px = x0 + where[at] % bw, py = y0 + where[at] / bw;
      double k = m[where[at]] / 255.0, add[3] = { 0, 0, 0 };
      if (!s->clone)
        field_at (&f, px, py, add);
      for (c = 0; c < 3; c++)
        {
          double orig = p->get (p->img, p->w, px, py, c), fill = patch[at * 3 + c] + add[c];
          p->put (p->img, p->w, px, py, c, orig + (fill - orig) * k);
        }
    }
out:
  free (f.kind);
  free (f.v);
  free (where);
  free (patch);
  free (m);
}

/* --- masks */

static float lin_of8[256];             /* sRGB 8-bit to linear 0..1 */
static float srgb_of[4097];            /* linear 0..1 (4096ths) to sRGB 0..255 */

static void tables (void)
{
  int i;
  if (lin_of8[255] > 0)
    return;
  for (i = 0; i < 256; i++)
    {
      double v = i / 255.0;
      lin_of8[i] = (float) (v <= 0.04045 ? v / 12.92 : pow ((v + 0.055) / 1.055, 2.4));
    }
  for (i = 0; i <= 4096; i++)
    {
      double v = i / 4096.0;
      srgb_of[i] = (float) (255 * (v <= 0.0031308 ? v * 12.92 : 1.055 * pow (v, 1 / 2.4) - 0.055));
    }
}

static double to_lin (const pic *p, double v)
{
  return p->maxv == 255 ? lin_of8[(int) v] : v / 65535;
}

static double from_lin (const pic *p, double v)
{
  v = v < 0 ? 0 : v > 1 ? 1 : v;
  if (p->maxv != 255)
    return v * 65535;
  if (v < 0.0031308)
    return 255 * 12.92 * v;            /* the straight part, exactly */
  {
    double f = v * 4096;
    int i = (int) f;
    if (i >= 4096)
      return srgb_of[4096];
    return srgb_of[i] + (srgb_of[i + 1] - srgb_of[i]) * (f - i);
  }
}

/* The mask's shape alone at pixel (x, y) of a w x h photo, 0..1. */
static double shape_at (const recipe_mask *mk, int w, int h, double x, double y)
{
  double m = 0;
  if (mk->kind == MASK_GRADIENT)
    {
      double ax = mk->x0 * w, ay = mk->y0 * h, dx = mk->x1 * w - ax, dy = mk->y1 * h - ay, l = dx * dx + dy * dy;
      if (l > 1)
        m = 1 - smooth (((x - ax) * dx + (y - ay) * dy) / l);
    }
  else if (mk->kind == MASK_RADIAL)
    {
      double rx = mk->x1 * w, ry = mk->y1 * h, ex, ey, e, in = 1 - (mk->f < 0.02f ? 0.02 : mk->f);
      if (rx > 0.5 && ry > 0.5)
        {
          ex = (x - mk->x0 * w) / rx;
          ey = (y - mk->y0 * h) / ry;
          e = sqrt (ex * ex + ey * ey);
          m = e <= in ? 1 : e >= 1 ? 0 : smooth ((1 - e) / (1 - in));
        }
    }
  return mk->invert ? 1 - m : m;
}

/* A stroke's own coverage at pixel (x, y) (its brush along its path). */
static double stroke_at (const recipe *r, const recipe_stroke *st, int w, int h, double x, double y)
{
  int longer = w > h ? w : h, j;
  double R = st->r * longer, inner = R * (1 - (st->f < 0.02f ? 0.02 : st->f)), best = 1e30;
  for (j = 0; j < (st->n > 1 ? st->n - 1 : 1); j++)
    {
      int k = st->n > 1 ? j + 1 : j;
      double d = seg_dist (x, y, r->pt[st->first + j][0] / 10000.0 * w, r->pt[st->first + j][1] / 10000.0 * h,
                           r->pt[st->first + k][0] / 10000.0 * w, r->pt[st->first + k][1] / 10000.0 * h);
      if (d < best)
        best = d;
    }
  return best >= R ? 0 : smooth (best <= inner ? 1 : (R - best) / (R - inner));
}

float retouch_mask_at (const recipe *r, int mi, int w, int h, float u, float v)
{
  double x = u * w, y = v * h, m;
  int i;
  if (mi < 0 || mi >= r->nmasks || w <= 0 || h <= 0)
    return 0;
  m = shape_at (&r->mask[mi], w, h, x, y);
  for (i = 0; i < r->nstrokes; i++)
    if (r->stroke[i].mask == mi)
      {
        double s = r->stroke[i].amount * stroke_at (r, &r->stroke[i], w, h, x, y);
        m = r->stroke[i].kind == BRUSH_ERASE ? m * (1 - s) : (s > m ? s : m);
      }
  return (float) m;
}

/* A mask's coverage on a grid of gw x gh cells, each g x g pixels; 65535 = all. */
typedef struct { int g, gw, gh; uint16_t *v; } mgrid;

static int mask_grid (mgrid *G, const recipe *r, int mi, int w, int h)
{
  const recipe_mask *mk = &r->mask[mi];
  int longer = w > h ? w : h, i, j, k;
  G->g = (longer + MASK_GRID_MAX - 1) / MASK_GRID_MAX;
  G->gw = (w + G->g - 1) / G->g;
  G->gh = (h + G->g - 1) / G->g;
  if (!(G->v = malloc (sizeof *G->v * (size_t) G->gw * G->gh)))
    return -1;
  for (j = 0; j < G->gh; j++)
    for (i = 0; i < G->gw; i++)
      G->v[(size_t) j * G->gw + i] = (uint16_t) lround (65535 * shape_at (mk, w, h, (i + 0.5) * G->g, (j + 0.5) * G->g));
  /* The strokes on it, in the order painted. */
  for (k = 0; k < r->nstrokes; k++)
    {
      const recipe_stroke *st = &r->stroke[k];
      double R = st->r * longer, mx0 = 1e30, my0 = 1e30, mx1 = -1e30, my1 = -1e30;
      int q, c0, c1, r0, r1;
      if (st->mask != mi || st->n <= 0 || st->amount <= 0)
        continue;
      for (q = 0; q < st->n; q++)
        {
          double px = r->pt[st->first + q][0] / 10000.0 * w, py = r->pt[st->first + q][1] / 10000.0 * h;
          mx0 = px < mx0 ? px : mx0; mx1 = px > mx1 ? px : mx1;
          my0 = py < my0 ? py : my0; my1 = py > my1 ? py : my1;
        }
      c0 = clampi ((int) floor ((mx0 - R) / G->g), 0, G->gw - 1); c1 = clampi ((int) ceil ((mx1 + R) / G->g), 0, G->gw - 1);
      r0 = clampi ((int) floor ((my0 - R) / G->g), 0, G->gh - 1); r1 = clampi ((int) ceil ((my1 + R) / G->g), 0, G->gh - 1);
      for (j = r0; j <= r1; j++)
        for (i = c0; i <= c1; i++)
          {
            double s = st->amount * stroke_at (r, st, w, h, (i + 0.5) * G->g, (j + 0.5) * G->g), m;
            uint16_t *gp = &G->v[(size_t) j * G->gw + i];
            if (s <= 0)
              continue;
            m = *gp / 65535.0;
            m = st->kind == BRUSH_ERASE ? m * (1 - s) : (s > m ? s : m);
            *gp = (uint16_t) lround (m * 65535);
          }
    }
  return 0;
}

static double grid_at (const mgrid *G, int x, int y)
{
  double gx, gy, fx, fy, a, b, c, d;
  int i, j, i1, j1;
  if (G->g == 1)
    return G->v[(size_t) y * G->gw + x] / 65535.0;
  gx = (x + 0.5) / G->g - 0.5;
  gy = (y + 0.5) / G->g - 0.5;
  gx = gx < 0 ? 0 : gx;
  gy = gy < 0 ? 0 : gy;
  i = (int) gx; j = (int) gy;
  i = i >= G->gw ? G->gw - 1 : i; j = j >= G->gh ? G->gh - 1 : j;
  i1 = i + 1 < G->gw ? i + 1 : i; j1 = j + 1 < G->gh ? j + 1 : j;
  fx = gx - i; fy = gy - j;
  fx = fx > 1 ? 1 : fx; fy = fy > 1 ? 1 : fy;
  a = G->v[(size_t) j * G->gw + i]; b = G->v[(size_t) j * G->gw + i1];
  c = G->v[(size_t) j1 * G->gw + i]; d = G->v[(size_t) j1 * G->gw + i1];
  return ((a * (1 - fx) + b * fx) * (1 - fy) + (c * (1 - fx) + d * fx) * fy) / 65535.0;
}

/* Lightness as seen (near enough: the square root of luminance). */
static double light_at (const pic *p, int x, int y)
{
  double l = 0.2126 * to_lin (p, p->get (p->img, p->w, x, y, 0)) + 0.7152 * to_lin (p, p->get (p->img, p->w, x, y, 1))
             + 0.0722 * to_lin (p, p->get (p->img, p->w, x, y, 2));
  return l > 0 ? sqrt (l) : 0;
}

/* The surroundings' lightness for clarity: the grid's cells' lightness,
   blurred (three box passes) by about 1.5% of the longer side. */
static float *clarity_map (const pic *p, const mgrid *G)
{
  int i, j, pass, longer = p->w > p->h ? p->w : p->h, rad;
  float *a = malloc (sizeof *a * (size_t) G->gw * G->gh), *t = malloc (sizeof *t * (size_t) (G->gw > G->gh ? G->gw : G->gh));
  if (!a || !t)
    {
      free (a);
      free (t);
      return NULL;
    }
  for (j = 0; j < G->gh; j++)
    for (i = 0; i < G->gw; i++)
      {
        /* A few pixels of the cell. */
        int x0 = i * G->g, y0 = j * G->g, step = G->g > 2 ? G->g / 2 : 1, x, y, n = 0;
        double s = 0;
        for (y = y0; y < y0 + G->g && y < p->h; y += step)
          for (x = x0; x < x0 + G->g && x < p->w; x += step)
            {
              s += light_at (p, x, y);
              n++;
            }
        a[(size_t) j * G->gw + i] = (float) (n ? s / n : 0);
      }
  rad = (int) lround (0.015 * longer / G->g / 1.7);
  if (rad < 1)
    rad = 1;
  for (pass = 0; pass < 6; pass++)
    {
      int horiz = pass % 2 == 0, len = horiz ? G->gw : G->gh, lines = horiz ? G->gh : G->gw, l, q;
      for (l = 0; l < lines; l++)
        {
          double s = 0;
          int cnt = 0;
#define AT(q_) a[horiz ? (size_t) l * G->gw + (q_) : (size_t) (q_) * G->gw + l]
          for (q = 0; q < rad && q < len; q++)
            {
              s += AT (q);
              cnt++;
            }
          for (q = 0; q < len; q++)
            {
              if (q + rad < len) { s += AT (q + rad); cnt++; }
              if (q - rad - 1 >= 0) { s -= AT (q - rad - 1); cnt--; }
              t[q] = (float) (s / cnt);
            }
          for (q = 0; q < len; q++)
            AT (q) = t[q];
#undef AT
        }
    }
  free (t);
  return a;
}

static int tone_used (const recipe_mask *mk)
{
  return mk->v[M_CONTRAST] != 0 || mk->v[M_HIGHLIGHTS] != 0 || mk->v[M_SHADOWS] != 0 || mk->v[M_CLARITY] != 0
         || mk->v[M_SHARPNESS] != 0;
}

static void apply_mask (const pic *p, const recipe *r, int mi)
{
  const recipe_mask *mk = &r->mask[mi];
  mgrid G;
  float *cmap = NULL, *rows = NULL;
  int x, y, i, x0, x1, y0, y1, tone = tone_used (mk), bw;
  double T = mk->v[M_TEMP] / 100.0, N = mk->v[M_TINT] / 100.0;
  double wb[3] = { 0.5 * T + 0.25 * N, -0.5 * N, -0.5 * T + 0.25 * N };
  int k;
  static float gain[3][1025];          /* by coverage, 1024ths */
  for (k = 0; k < M_COUNT && mk->v[k] == 0; k++)
    ;
  for (i = 0; i < 3; i++)
    for (x = 0; x <= 1024; x++)
      gain[i][x] = (float) pow (2.0, x / 1024.0 * (mk->v[M_EV] + wb[i]));
  if (k == M_COUNT || mask_grid (&G, r, mi, p->w, p->h))
    return;
  /* Where it covers anything. */
  x0 = G.gw; x1 = -1; y0 = G.gh; y1 = -1;
  for (y = 0; y < G.gh; y++)
    for (x = 0; x < G.gw; x++)
      if (G.v[(size_t) y * G.gw + x])
        {
          x0 = x < x0 ? x : x0; x1 = x > x1 ? x : x1;
          y0 = y < y0 ? y : y0; y1 = y > y1 ? y : y1;
        }
  if (x1 < 0)
    goto out;
  x0 = clampi ((x0 - 1) * G.g, 0, p->w - 1); x1 = clampi ((x1 + 2) * G.g, 0, p->w - 1);
  y0 = clampi ((y0 - 1) * G.g, 0, p->h - 1); y1 = clampi ((y1 + 2) * G.g, 0, p->h - 1);
  bw = x1 - x0 + 1;
  if (tone && mk->v[M_CLARITY] != 0 && !(cmap = clarity_map (p, &G)))
    goto out;
  /* Sharpness looks at the next pixels as they were: three rows of their
     lightness, kept ahead of the changes (bw + 2 wide). */
  if (tone && !(rows = malloc (sizeof *rows * 3 * (size_t) (bw + 2))))
    goto out;
#define ROW(yy) (rows + (size_t) (((yy) % 3 + 3) % 3) * (bw + 2))
  if (rows)
    for (y = y0 - 1; y <= y0; y++)
      {
        int yy = clampi (y, 0, p->h - 1);
        for (x = -1; x <= bw; x++)
          ROW (y)[x + 1] = (float) light_at (p, clampi (x0 + x, 0, p->w - 1), yy);
      }
  for (y = y0; y <= y1; y++)
    {
      if (rows)
        {
          int yy = clampi (y + 1, 0, p->h - 1);
          for (x = -1; x <= bw; x++)
            ROW (y + 1)[x + 1] = (float) light_at (p, clampi (x0 + x, 0, p->w - 1), yy);
        }
      for (x = x0; x <= x1; x++)
        {
          double m = grid_at (&G, x, y), c[3], lum;
          if (m <= 1.0 / 65535)
            continue;
          {
            double gi = m * 1024;
            int g0 = (int) gi;
            double gf = gi - g0;
            g0 = g0 > 1023 ? 1023 : g0;
            for (i = 0; i < 3; i++)
              c[i] = to_lin (p, p->get (p->img, p->w, x, y, i)) * (gain[i][g0] + (gain[i][g0 + 1] - gain[i][g0]) * gf);
          }
          if (tone)
            {
              double L = 0.2126 * c[0] + 0.7152 * c[1] + 0.0722 * c[2], pl = L > 0 ? sqrt (L) : 0, q = pl > 1 ? 1 : pl, np = pl, L2;
              double p0 = ROW (y)[x - x0 + 1];
              np += m * mk->v[M_CONTRAST] / 100.0 * (q - 0.5) * 2 * q * (1 - q);
              np += m * mk->v[M_HIGHLIGHTS] / 100.0 * 1.2 * q * q * q * (1 - q);
              np += m * mk->v[M_SHADOWS] / 100.0 * 1.2 * q * (1 - q) * (1 - q) * (1 - q);
              if (cmap)
                {
                  /* The surroundings: the blurred map, between its cells. */
                  double gx = (x + 0.5) / G.g - 0.5, gy = (y + 0.5) / G.g - 0.5, fx, fy, pb;
                  int ci, cj, ci1, cj1;
                  gx = gx < 0 ? 0 : gx; gy = gy < 0 ? 0 : gy;
                  ci = (int) gx; cj = (int) gy;
                  ci = ci >= G.gw ? G.gw - 1 : ci; cj = cj >= G.gh ? G.gh - 1 : cj;
                  ci1 = ci + 1 < G.gw ? ci + 1 : ci; cj1 = cj + 1 < G.gh ? cj + 1 : cj;
                  fx = gx - ci; fy = gy - cj;
                  fx = fx > 1 ? 1 : fx; fy = fy > 1 ? 1 : fy;
                  pb = (cmap[(size_t) cj * G.gw + ci] * (1 - fx) + cmap[(size_t) cj * G.gw + ci1] * fx) * (1 - fy)
                       + (cmap[(size_t) cj1 * G.gw + ci] * (1 - fx) + cmap[(size_t) cj1 * G.gw + ci1] * fx) * fy;
                  np += m * mk->v[M_CLARITY] / 100.0 * 0.8 * (p0 - pb) * 4 * q * (1 - q);
                }
              if (mk->v[M_SHARPNESS] != 0)
                {
                  int a = x - x0 + 1;
                  double near = (ROW (y - 1)[a - 1] + ROW (y - 1)[a] + ROW (y - 1)[a + 1] + ROW (y)[a - 1] + ROW (y)[a]
                                 + ROW (y)[a + 1] + ROW (y + 1)[a - 1] + ROW (y + 1)[a] + ROW (y + 1)[a + 1]) / 9;
                  np += m * mk->v[M_SHARPNESS] / 100.0 * 1.2 * (p0 - near);
                }
              np = np < 0 ? 0 : np;
              L2 = np * np;
              if (L > 1e-7)
                for (i = 0; i < 3; i++)
                  c[i] *= L2 / L;
              else
                for (i = 0; i < 3; i++)
                  c[i] = L2;
            }
          if (mk->v[M_SATURATION] != 0)
            {
              lum = 0.2126 * c[0] + 0.7152 * c[1] + 0.0722 * c[2];
              for (i = 0; i < 3; i++)
                c[i] = lum + (c[i] - lum) * (1 + mk->v[M_SATURATION] / 100.0 * m);
            }
          for (i = 0; i < 3; i++)
            p->put (p->img, p->w, x, y, i, from_lin (p, c[i]));
        }
    }
#undef ROW
out:
  free (rows);
  free (cmap);
  free (G.v);
}

static void apply_all (pic *p, const recipe *r)
{
  int i;
  for (i = 0; i < r->nspots; i++)
    apply_spot (p, r, &r->spot[i]);
  tables ();
  for (i = 0; i < r->nmasks; i++)
    if (!r->mask[i].off)
      apply_mask (p, r, i);
}

void retouch_rgb8 (unsigned char *rgb, int w, int h, const recipe *r)
{
  pic p = { w, h, 255, get8, put8, rgb };
  if ((r->nspots || r->nmasks) && w > 0 && h > 0)
    apply_all (&p, r);
}

void retouch_lin (uint16_t *lin, int w, int h, const recipe *r)
{
  pic p = { w, h, 65535, get16, put16, lin };
  if ((r->nspots || r->nmasks) && w > 0 && h > 0)
    apply_all (&p, r);
}

void retouch_lin_show (uint16_t *lin, int w, int h, const recipe *r, int show)
{
  mgrid G;
  int x, y;
  retouch_lin (lin, w, h, r);
  if (show < 0 || show >= r->nmasks || w <= 0 || h <= 0 || mask_grid (&G, r, show, w, h))
    return;
  for (y = 0; y < h; y++)
    for (x = 0; x < w; x++)
      {
        double m = grid_at (&G, x, y) * 0.6;
        uint16_t *px = &lin[((size_t) y * w + x) * 3];
        if (m <= 0)
          continue;
        px[0] = (uint16_t) lround (px[0] * (1 - m) + 60000 * m);
        px[1] = (uint16_t) lround (px[1] * (1 - m) + 1500 * m);
        px[2] = (uint16_t) lround (px[2] * (1 - m) + 1500 * m);
      }
  free (G.v);
}

/* --- finding a source */

void retouch_auto_offset (const uint16_t *lin, int w, int h, const float *xy, int n, float r, float *odx, float *ody)
{
  pic p = { w, h, 65535, get16, put16, (void *) lin };
  int longer = w > h ? w : h, k, j, ring, ns = 0, nm = 0;
  double R = r * longer, best = -1;
  static double sx_[64], sy_[64], sv[64][3], mx_[32], my_[32], mside[32][2];
  *odx = r * 2.5f * (float) longer / w;   /* to the right, if nothing better */
  *ody = 0;
  if (w <= 0 || h <= 0 || n <= 0)
    return;
  /* The spot's surroundings: round the circle, or on both sides of the
     path (and past its ends); and points along the middle. */
  if (n == 1)
    for (j = 0; j < 16; j++)
      {
        double a = 2 * PI * j / 16;
        sx_[ns] = xy[0] * w + cos (a) * R * 1.2;
        sy_[ns] = xy[1] * h + sin (a) * R * 1.2;
        ns++;
      }
  else
    {
      int step = n > 28 ? (n + 27) / 28 : 1;
      for (j = 0; j < n && ns + 2 <= 60; j += step)
        {
          int a = j > 0 ? j - 1 : 0, b = j + 1 < n ? j + 1 : n - 1;
          double dx = (xy[2 * b] - xy[2 * a]) * w, dy = (xy[2 * b + 1] - xy[2 * a + 1]) * h, l = sqrt (dx * dx + dy * dy);
          double px = xy[2 * j] * w, py = xy[2 * j + 1] * h;
          if (l <= 0) { dx = 1; dy = 0; l = 1; }
          sx_[ns] = px - dy / l * R * 1.2; sy_[ns] = py + dx / l * R * 1.2; ns++;
          sx_[ns] = px + dy / l * R * 1.2; sy_[ns] = py - dx / l * R * 1.2; ns++;
          if (nm < 30)
            {
              mx_[nm] = px; my_[nm] = py;
              mside[nm][0] = ns - 2; mside[nm][1] = ns - 1;
              nm++;
            }
        }
    }
  for (j = 0; j < ns; j++)
    sample3 (&p, sx_[j], sy_[j], sv[j]);
  /* Candidates round it, two and a half, four and six radii away: the one
     whose own surroundings (and middle) look most like the spot's. */
  for (ring = 0; ring < 3; ring++)
    for (k = 0; k < 16; k++)
      {
        double a = 2 * PI * (k + 0.5 * (ring & 1)) / 16, dist = R * (ring == 0 ? 2.5 : ring == 1 ? 4.0 : 6.0);
        double ox = cos (a) * dist, oy = sin (a) * dist, score = 0;
        int c, off = 0;
        for (j = 0; j < ns && !off; j++)
          {
            double v[3];
            double qx = sx_[j] + ox, qy = sy_[j] + oy;
            if (qx < 0 || qy < 0 || qx > w - 1 || qy > h - 1)
              {
                off = 1;
                break;
              }
            sample3 (&p, qx, qy, v);
            for (c = 0; c < 3; c++)
              score += (v[c] - sv[j][c]) * (v[c] - sv[j][c]);
          }
        if (off)
          continue;                    /* off the picture */
        if (n == 1)
          {
            /* Its middle should be like its own edge (no blemish of its own). */
            double own[3] = { 0, 0, 0 }, mid[3];
            double qx = xy[0] * w + ox, qy = xy[1] * h + oy;
            for (j = 0; j < 16; j++)
              {
                double v[3];
                sample3 (&p, sx_[j] + ox, sy_[j] + oy, v);
                for (c = 0; c < 3; c++)
                  own[c] += v[c] / 16;
              }
            sample3 (&p, qx, qy, mid);
            for (c = 0; c < 3; c++)
              score += 4 * (mid[c] - own[c]) * (mid[c] - own[c]) * 16;
          }
        else
          for (j = 0; j < nm; j++)
            {
              /* Along the middle, like its own two sides. */
              double mid[3], s0[3], s1[3];
              sample3 (&p, mx_[j] + ox, my_[j] + oy, mid);
              sample3 (&p, sx_[(int) mside[j][0]] + ox, sy_[(int) mside[j][0]] + oy, s0);
              sample3 (&p, sx_[(int) mside[j][1]] + ox, sy_[(int) mside[j][1]] + oy, s1);
              for (c = 0; c < 3; c++)
                {
                  double d = mid[c] - (s0[c] + s1[c]) / 2;
                  score += 2 * d * d * ns / (nm ? nm : 1);
                }
            }
        score *= ring == 0 ? 1 : ring == 1 ? 1.15 : 1.3;   /* nearer is a little better */
        if (best < 0 || score < best)
          {
            best = score;
            *odx = (float) (ox / w);
            *ody = (float) (oy / h);
          }
      }
}

void retouch_auto_source (const uint16_t *lin, int w, int h, float x, float y, float r, float *sx, float *sy)
{
  float xy[2] = { x, y }, dx, dy;
  retouch_auto_offset (lin, w, h, xy, 1, r, &dx, &dy);
  *sx = x + dx;
  *sy = y + dy;
}
