/* Copyright (c) 2026 Andrew Youll
   Released under the MIT licence: see the LICENSE file.
   SPDX-License-Identifier: MIT */
/* retouch.c: see retouch.h. */
#include <math.h>
#include <stdlib.h>
#include <string.h>
#include "retouch.h"
#include "simd.h"

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
#define SRGB8_STEPS 16384
static unsigned char srgb8[SRGB8_STEPS + 1];   /* linear 0..1 (16384ths) to sRGB 0..255 */

static double srgb_encode (double v)
{
  return 255 * (v <= 0.0031308 ? v * 12.92 : 1.055 * pow (v, 1 / 2.4) - 0.055);
}

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
  for (i = 0; i <= SRGB8_STEPS; i++)
    {
      double o = srgb_encode ((double) i / SRGB8_STEPS);
      srgb8[i] = (unsigned char) (o > 255 ? 255 : o < 0 ? 0 : o + 0.5);
    }
  /* A pixel a layer leaves as it was comes back as it was. */
  for (i = 0; i < 256; i++)
    srgb8[(int) (lin_of8[i] * SRGB8_STEPS + 0.5f)] = (unsigned char) i;
}

/* A pixel's channel in linear light, 0..1, and back (the photo as 8-bit
   sRGB or 16-bit linear): straight from the pixels, no calls through
   the pic's functions (this is where the time goes); back to 8 bits in
   one look in a table fine enough to be within half a level. */
static inline float lin_px (const pic *p, size_t i)
{
  return p->maxv == 255 ? lin_of8[((const unsigned char *) p->img)[i]] : ((const uint16_t *) p->img)[i] * (1.0f / 65535);
}

static inline void put_px (const pic *p, size_t i, float v)
{
  v = v < 0 ? 0 : v > 1 ? 1 : v;
  if (p->maxv != 255)
    ((uint16_t *) p->img)[i] = (uint16_t) (v * 65535 + 0.5f);
  else
    ((unsigned char *) p->img)[i] = srgb8[(int) (v * SRGB8_STEPS + 0.5f)];
}

/* A gradient's or oval's shape, worked out once for a w x h photo (and
   the oval's turn), to be found at any pixel. */
typedef struct
{
  int kind, invert, none;              /* none: nothing (a brush mask's shape, or too small) */
  double ax, ay, dx, dy, l;            /* gradient: from (ax, ay) along (dx, dy); l: its length squared */
  double cx, cy, rx, ry, cs, sn;       /* oval: its centre, half sizes and turn */
  double irx2, iry2, in2, fin;         /* 1 / rx^2, 1 / ry^2, the inner part's e, 1 / the fading part */
} shape;

static void shape_prep (shape *S, const recipe_mask *mk, int w, int h)
{
  memset (S, 0, sizeof *S);
  S->kind = mk->kind;
  S->invert = mk->invert;
  S->none = 1;
  if (mk->kind == MASK_GRADIENT)
    {
      S->ax = mk->x0 * w; S->ay = mk->y0 * h;
      S->dx = mk->x1 * w - S->ax; S->dy = mk->y1 * h - S->ay;
      S->l = S->dx * S->dx + S->dy * S->dy;
      S->none = !(S->l > 1);
    }
  else if (mk->kind == MASK_RADIAL)
    {
      double in = 1 - (mk->f < 0.02f ? 0.02 : mk->f), a = mk->angle * PI / 180;
      S->cx = mk->x0 * w; S->cy = mk->y0 * h;
      S->rx = mk->x1 * w; S->ry = mk->y1 * h;
      /* turned clockwise by angle: (u, v) in the oval's own axes */
      S->cs = mk->angle != 0 ? cos (a) : 1;
      S->sn = mk->angle != 0 ? sin (a) : 0;
      S->in2 = in * in;
      S->fin = 1 / (1 - in);
      if (S->rx > 0.5 && S->ry > 0.5)
        {
          S->none = 0;
          S->irx2 = 1 / (S->rx * S->rx);
          S->iry2 = 1 / (S->ry * S->ry);
        }
    }
}

/* The oval's coverage from e, its (u/rx)^2 + (v/ry)^2: the square root
   only in the fading part. */
static inline float oval_of (const shape *S, float e)
{
  float t;
  if (e <= S->in2)
    return 1;
  if (e >= 1)
    return 0;
  t = (1 - sqrtf (e)) * (float) S->fin;
  return t * t * (3 - 2 * t);
}

static double shape_eval (const shape *S, double x, double y)
{
  double m = 0;
  if (!S->none)
    {
      if (S->kind == MASK_GRADIENT)
        m = 1 - smooth (((x - S->ax) * S->dx + (y - S->ay) * S->dy) / S->l);
      else
        {
          double dx = x - S->cx, dy = y - S->cy, u = dx * S->cs + dy * S->sn, v = -dx * S->sn + dy * S->cs;
          m = oval_of (S, (float) (u * u * S->irx2 + v * v * S->iry2));
        }
    }
  return S->invert ? 1 - m : m;
}

/* The shape along row y, pixels x0..x0+n-1 (their centres), into out:
   a gradient is a straight line's distance, an oval's e a quadratic
   along the row, outside the part of the row it crosses nothing. */
static void shape_row (const shape *S, int y, int x0, int n, float *out)
{
  int x;
  float outside = S->invert ? 1.0f : 0.0f;
  double py = y + 0.5;
  if (S->none)
    {
      for (x = 0; x < n; x++)
        out[x] = outside;
      return;
    }
  if (S->kind == MASK_GRADIENT)
    {
      float t0 = (float) (((x0 + 0.5 - S->ax) * S->dx + (py - S->ay) * S->dy) / S->l), dt = (float) (S->dx / S->l);
      for (x = 0; x < n; x++)
        {
          float t = t0 + dt * x, m = t <= 0 ? 1 : t >= 1 ? 0 : 1 - t * t * (3 - 2 * t);
          out[x] = S->invert ? 1 - m : m;
        }
      return;
    }
  {
    /* e = a d^2 + b d + c, d the distance along the row from the centre */
    double dy = py - S->cy, a = S->cs * S->cs * S->irx2 + S->sn * S->sn * S->iry2;
    double b = 2 * dy * S->cs * S->sn * (S->irx2 - S->iry2);
    double c = dy * dy * (S->sn * S->sn * S->irx2 + S->cs * S->cs * S->iry2), disc = b * b - 4 * a * (c - 1);
    int s0 = n, s1 = -1;
    if (disc > 0)
      {
        double q = sqrt (disc), lo = (-b - q) / (2 * a) + S->cx - 0.5 - x0, hi = (-b + q) / (2 * a) + S->cx - 0.5 - x0;
        s0 = lo <= 0 ? 0 : lo >= n ? n : (int) lo;
        s1 = hi < 0 ? -1 : hi >= n - 1 ? n - 1 : (int) hi + 1;
      }
    for (x = 0; x < s0 && x < n; x++)
      out[x] = outside;
    {
      float fa = (float) a, fb = (float) b, fc = (float) c, d = (float) (x0 + s0 + 0.5 - S->cx);
      for (x = s0; x <= s1; x++, d += 1)
        {
          float m = oval_of (S, (fa * d + fb) * d + fc);
          out[x] = S->invert ? 1 - m : m;
        }
    }
    for (x = s1 + 1 > s0 ? s1 + 1 : s0; x < n; x++)
      out[x] = outside;
  }
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
  {
    shape S;
    shape_prep (&S, &r->mask[mi], w, h);
    m = shape_eval (&S, x, y);
  }
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
  shape S;
  double band = 1e30;                  /* the narrowest part that fades, in pixels */
  shape_prep (&S, mk, w, h);
  /* Cells a quarter of that across are fine enough to blend between. */
  if (!S.none)
    band = S.kind == MASK_GRADIENT ? sqrt (S.l) : (1 - sqrt (S.in2)) * (S.rx < S.ry ? S.rx : S.ry);
  for (k = 0; k < r->nstrokes; k++)
    if (r->stroke[k].mask == mi && r->stroke[k].n > 0 && r->stroke[k].amount > 0)
      {
        double R = r->stroke[k].r * longer * (r->stroke[k].f < 0.02f ? 0.02 : r->stroke[k].f);
        band = R < band ? R : band;
      }
  G->g = (longer + MASK_GRID_MAX - 1) / MASK_GRID_MAX;
  if (band < 1e29 && (int) (band / 4) > G->g)
    G->g = (int) (band / 4);
  G->gw = (w + G->g - 1) / G->g;
  G->gh = (h + G->g - 1) / G->g;
  if (!(G->v = malloc (sizeof *G->v * (size_t) G->gw * G->gh)))
    return -1;
  if (S.none)
    {
      uint16_t v = S.invert ? 65535 : 0;
      for (i = 0; i < G->gw * G->gh; i++)
        G->v[i] = v;
    }
  else
    for (j = 0; j < G->gh; j++)
      for (i = 0; i < G->gw; i++)
        G->v[(size_t) j * G->gw + i] = (uint16_t) (65535 * shape_eval (&S, (i + 0.5) * G->g, (j + 0.5) * G->g) + 0.5);
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
      {
        /* The distance to the path at each cell: each segment only where
           it reaches (within R), the nearest kept. */
        int bw = c1 - c0 + 1, bh = r1 - r0 + 1, nseg = st->n > 1 ? st->n - 1 : 1, sgi;
        double inner = R * (1 - (st->f < 0.02f ? 0.02 : st->f));
        float *dist = malloc (sizeof *dist * (size_t) bw * bh);
        if (!dist)
          continue;
        for (q = 0; q < bw * bh; q++)
          dist[q] = 1e30f;
        for (sgi = 0; sgi < nseg; sgi++)
          {
            int kk = st->n > 1 ? sgi + 1 : sgi, s0, s1, t0, t1;
            double ax = r->pt[st->first + sgi][0] / 10000.0 * w, ay = r->pt[st->first + sgi][1] / 10000.0 * h;
            double bx = r->pt[st->first + kk][0] / 10000.0 * w, by = r->pt[st->first + kk][1] / 10000.0 * h;
            s0 = clampi ((int) floor (((ax < bx ? ax : bx) - R) / G->g), c0, c1);
            s1 = clampi ((int) ceil (((ax > bx ? ax : bx) + R) / G->g), c0, c1);
            t0 = clampi ((int) floor (((ay < by ? ay : by) - R) / G->g), r0, r1);
            t1 = clampi ((int) ceil (((ay > by ? ay : by) + R) / G->g), r0, r1);
            for (j = t0; j <= t1; j++)
              for (i = s0; i <= s1; i++)
                {
                  double d = seg_dist ((i + 0.5) * G->g, (j + 0.5) * G->g, ax, ay, bx, by);
                  float *dp = &dist[(size_t) (j - r0) * bw + (i - c0)];
                  if (d < *dp)
                    *dp = (float) d;
                }
          }
        for (j = r0; j <= r1; j++)
          for (i = c0; i <= c1; i++)
            {
              double best = dist[(size_t) (j - r0) * bw + (i - c0)], s, m;
              uint16_t *gp = &G->v[(size_t) j * G->gw + i];
              if (best >= R)
                continue;
              s = st->amount * smooth (best <= inner ? 1 : (R - best) / (R - inner));
              if (s <= 0)
                continue;
              m = *gp / 65535.0;
              m = st->kind == BRUSH_ERASE ? m * (1 - s) : (s > m ? s : m);
              *gp = (uint16_t) (m * 65535 + 0.5);
            }
        free (dist);
      }
    }
  return 0;
}

/* Reading a grid along rows: for each pixel of x0..x0+n-1 its two cells
   across and how far between (found once), and for each row the two
   rows of cells blended (once a row), so a pixel is one blend. */
typedef struct
{
  int g, gw, gh, n;
  int *i0, *i1;
  float *fx, *line;
} gsampler;

static int gs_init (gsampler *s, int g, int gw, int gh, int x0, int n)
{
  int x;
  s->g = g; s->gw = gw; s->gh = gh; s->n = n;
  s->i0 = malloc (sizeof *s->i0 * (size_t) n);
  s->i1 = malloc (sizeof *s->i1 * (size_t) n);
  s->fx = malloc (sizeof *s->fx * (size_t) n);
  s->line = malloc (sizeof *s->line * (size_t) gw);
  if (!s->i0 || !s->i1 || !s->fx || !s->line)
    return -1;
  for (x = 0; x < n; x++)
    {
      double gx = (x0 + x + 0.5) / g - 0.5, f;
      int i;
      gx = gx < 0 ? 0 : gx;
      i = (int) gx;
      i = i >= gw ? gw - 1 : i;
      f = gx - i;
      s->i0[x] = i;
      s->i1[x] = i + 1 < gw ? i + 1 : i;
      s->fx[x] = (float) (f > 1 ? 1 : f);
    }
  return 0;
}

static void gs_free (gsampler *s)
{
  free (s->i0); free (s->i1); free (s->fx); free (s->line);
  s->i0 = s->i1 = NULL; s->fx = s->line = NULL;
}

/* Row y's two rows of cells and how far between. */
static void gs_rows (const gsampler *s, int y, int *j0, int *j1, float *fy)
{
  double gy = (y + 0.5) / s->g - 0.5, f;
  int j;
  gy = gy < 0 ? 0 : gy;
  j = (int) gy;
  j = j >= s->gh ? s->gh - 1 : j;
  f = gy - j;
  *j0 = j;
  *j1 = j + 1 < s->gh ? j + 1 : j;
  *fy = (float) (f > 1 ? 1 : f);
}

static void gs_across (const gsampler *s, float *out)
{
  int x;
  for (x = 0; x < s->n; x++)
    {
      float a = s->line[s->i0[x]];
      out[x] = a + (s->line[s->i1[x]] - a) * s->fx[x];
    }
}

/* A mask's grid (65535 = all) along row y, 0..1. */
static void gs_row16 (const gsampler *s, const uint16_t *v, int y, float *out)
{
  int i, j0, j1;
  float fy;
  gs_rows (s, y, &j0, &j1, &fy);
  for (i = 0; i < s->gw; i++)
    {
      float a = v[(size_t) j0 * s->gw + i];
      s->line[i] = (a + (v[(size_t) j1 * s->gw + i] - a) * fy) * (1.0f / 65535);
    }
  gs_across (s, out);
}

static void gs_rowf (const gsampler *s, const float *v, int y, float *out)
{
  int i, j0, j1;
  float fy;
  gs_rows (s, y, &j0, &j1, &fy);
  for (i = 0; i < s->gw; i++)
    {
      float a = v[(size_t) j0 * s->gw + i];
      s->line[i] = a + (v[(size_t) j1 * s->gw + i] - a) * fy;
    }
  gs_across (s, out);
}

/* A mask's coverage of a w x h picture, a row at a time: a gradient or
   oval without strokes found at each pixel from its shape, anything else
   from its grid. x0..x1, y0..y1: the part it covers (none if x1 < x0). */
typedef struct
{
  int direct, x0, x1, y0, y1;
  shape S;
  mgrid G;
  gsampler s;
} cover;

static void cover_free (cover *C)
{
  free (C->G.v);
  C->G.v = NULL;
  gs_free (&C->s);
}

static int cover_make (cover *C, const recipe *r, int mi, int w, int h)
{
  const recipe_mask *mk = &r->mask[mi];
  int i, x, y;
  memset (C, 0, sizeof *C);
  C->x0 = 0; C->x1 = w - 1; C->y0 = 0; C->y1 = h - 1;
  C->direct = mk->kind == MASK_GRADIENT || mk->kind == MASK_RADIAL;
  for (i = 0; i < r->nstrokes && C->direct; i++)
    if (r->stroke[i].mask == mi && r->stroke[i].n > 0 && r->stroke[i].amount > 0)
      C->direct = 0;
  if (C->direct)
    {
      shape_prep (&C->S, mk, w, h);
      if (C->S.none && !C->S.invert)
        C->x1 = -1;
      else if (!C->S.none && !C->S.invert && C->S.kind == MASK_RADIAL)
        {
          /* the turned oval's box */
          double hx = sqrt (C->S.rx * C->S.cs * C->S.rx * C->S.cs + C->S.ry * C->S.sn * C->S.ry * C->S.sn);
          double hy = sqrt (C->S.rx * C->S.sn * C->S.rx * C->S.sn + C->S.ry * C->S.cs * C->S.ry * C->S.cs);
          C->x0 = clampi ((int) floor (C->S.cx - hx) - 1, 0, w - 1); C->x1 = clampi ((int) ceil (C->S.cx + hx) + 1, 0, w - 1);
          C->y0 = clampi ((int) floor (C->S.cy - hy) - 1, 0, h - 1); C->y1 = clampi ((int) ceil (C->S.cy + hy) + 1, 0, h - 1);
          if (C->S.cx + hx < 0 || C->S.cx - hx > w || C->S.cy + hy < 0 || C->S.cy - hy > h)
            C->x1 = -1;
        }
      return 0;
    }
  if (mask_grid (&C->G, r, mi, w, h))
    return -1;
  {
    mgrid *G = &C->G;
    int gx0 = G->gw, gx1 = -1, gy0 = G->gh, gy1 = -1;
    for (y = 0; y < G->gh; y++)
      for (x = 0; x < G->gw; x++)
        if (G->v[(size_t) y * G->gw + x])
          {
            gx0 = x < gx0 ? x : gx0; gx1 = x > gx1 ? x : gx1;
            gy0 = y < gy0 ? y : gy0; gy1 = y > gy1 ? y : gy1;
          }
    if (gx1 < 0)
      {
        C->x1 = -1;
        return 0;
      }
    C->x0 = clampi ((gx0 - 1) * G->g, 0, w - 1); C->x1 = clampi ((gx1 + 2) * G->g, 0, w - 1);
    C->y0 = clampi ((gy0 - 1) * G->g, 0, h - 1); C->y1 = clampi ((gy1 + 2) * G->g, 0, h - 1);
    if (G->g > 1 && gs_init (&C->s, G->g, G->gw, G->gh, C->x0, C->x1 - C->x0 + 1))
      return -1;
  }
  return 0;
}

/* Row y's coverage, x0..x1, into out. */
static void cover_row (const cover *C, int y, float *out)
{
  int n = C->x1 - C->x0 + 1, x;
  if (C->direct)
    shape_row (&C->S, y, C->x0, n, out);
  else if (C->G.g == 1)
    {
      const uint16_t *v = C->G.v + (size_t) y * C->G.gw + C->x0;
      for (x = 0; x < n; x++)
        out[x] = v[x] * (1.0f / 65535);
    }
  else
    gs_row16 (&C->s, C->G.v, y, out);
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
              size_t at = ((size_t) y * p->w + x) * 3;
              float l = 0.2126f * lin_px (p, at) + 0.7152f * lin_px (p, at + 1) + 0.0722f * lin_px (p, at + 2);
              s += l > 0 ? sqrtf (l) : 0;
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

/* Lightness as seen of a row's pixels x0-1 .. x0+bw (clamped), into out. */
static void light_row (const pic *p, int y, int x0, int bw, float *out)
{
  int x;
  y = clampi (y, 0, p->h - 1);
  for (x = -1; x <= bw; x++)
    {
      size_t i = ((size_t) y * p->w + clampi (x0 + x, 0, p->w - 1)) * 3;
      float l = 0.2126f * lin_px (p, i) + 0.7152f * lin_px (p, i + 1) + 0.0722f * lin_px (p, i + 2);
      out[x + 1] = l > 0 ? sqrtf (l) : 0;
    }
}

/* A layer's settings, for its NEON row. */
typedef struct
{
  float kc, kh, ks, kl, kp, ksat;
  int tone;
  const float (*gain)[1025];
} mask_k;

#if defined(__arm__)
#pragma GCC push_options
#pragma GCC target ("fpu=neon")
#include <arm_neon.h>
/* apply_mask's pixels four at a time, x0 on (to x1): the arithmetic in
   NEON's lanes, done in the same order as the plain code, so the pixels
   are exactly its own; the tables (the gains, 8-bit sRGB and back), the
   square root and the division a lane at a time, as NEON has none it
   could use that are exact. u, c, d: the rows of lightness above, on and
   below (sharpness), NULL without tone. Where it got to. */
static int mask_row_neon (const pic *p, int y, int x0, int x1, const float *mrow, const float *pb_row,
                          const float *u, const float *c, const float *d, const mask_k *K)
{
  int x, i, k;
  const float32x4_t zero = vdupq_n_f32 (0), one = vdupq_n_f32 (1);
  for (x = x0; x + 3 <= x1; x += 4)
    {
      float32x4_t m = vld1q_f32 (mrow + x - x0), ch[3];
      uint32x4_t skip = vcleq_f32 (m, vdupq_n_f32 (1.0f / 65535));
      uint32_t sk[4] __attribute__ ((aligned (16)));
      int32_t gi[4] __attribute__ ((aligned (16)));
      float gv[3][4] __attribute__ ((aligned (16)));
      size_t at = ((size_t) y * p->w + x) * 3;
      uint16x4x3_t raw16;
      vst1q_u32 (sk, skip);
      if (sk[0] && sk[1] && sk[2] && sk[3])
        continue;
      if (p->maxv == 255)
        {
          const unsigned char *b = (const unsigned char *) p->img + at;
          for (i = 0; i < 3; i++)
            for (k = 0; k < 4; k++)
              gv[i][k] = lin_of8[b[k * 3 + i]];
          for (i = 0; i < 3; i++)
            ch[i] = vld1q_f32 (gv[i]);
        }
      else
        {
          raw16 = vld3_u16 ((const uint16_t *) p->img + at);
          for (i = 0; i < 3; i++)
            ch[i] = vmulq_f32 (vcvtq_f32_u32 (vmovl_u16 (raw16.val[i])), vdupq_n_f32 (1.0f / 65535));
        }
      vst1q_s32 (gi, vminq_s32 (vcvtq_s32_f32 (vaddq_f32 (vmulq_n_f32 (m, 1024), vdupq_n_f32 (0.5f))), vdupq_n_s32 (1024)));
      for (i = 0; i < 3; i++)
        {
          for (k = 0; k < 4; k++)
            gv[i][k] = K->gain[i][gi[k]];
          ch[i] = vmulq_f32 (ch[i], vld1q_f32 (gv[i]));
        }
      if (K->tone)
        {
          float32x4_t L = vaddq_f32 (vaddq_f32 (vmulq_n_f32 (ch[0], 0.2126f), vmulq_n_f32 (ch[1], 0.7152f)), vmulq_n_f32 (ch[2], 0.0722f));
          float32x4_t pl, q, omq, np, L2, t;
          float lv[4] __attribute__ ((aligned (16))), fv[4] __attribute__ ((aligned (16)));
          int a = x - x0 + 1;
          uint32x4_t big;
          vst1q_f32 (lv, L);
          for (k = 0; k < 4; k++)
            fv[k] = lv[k] > 0 ? sqrtf (lv[k]) : 0;
          pl = vld1q_f32 (fv);
          q = vminq_f32 (pl, one);
          omq = vsubq_f32 (one, q);
          np = pl;
          if (K->kc != 0)
            {
              t = vmulq_f32 (vmulq_f32 (vmulq_f32 (vmulq_n_f32 (m, K->kc), vsubq_f32 (q, vdupq_n_f32 (0.5f))), vdupq_n_f32 (2)), q);
              np = vaddq_f32 (np, vmulq_f32 (t, omq));
            }
          if (K->kh != 0)
            np = vaddq_f32 (np, vmulq_f32 (vmulq_f32 (vmulq_f32 (vmulq_f32 (vmulq_n_f32 (m, K->kh), q), q), q), omq));
          if (K->ks != 0)
            np = vaddq_f32 (np, vmulq_f32 (vmulq_f32 (vmulq_f32 (vmulq_f32 (vmulq_n_f32 (m, K->ks), q), omq), omq), omq));
          if (pb_row)
            {
              float32x4_t p0 = vld1q_f32 (c + a), pb = vld1q_f32 (pb_row + x - x0);
              t = vmulq_f32 (vmulq_n_f32 (vmulq_f32 (vmulq_n_f32 (m, K->kl), vsubq_f32 (p0, pb)), 4), q);
              np = vaddq_f32 (np, vmulq_f32 (t, omq));
            }
          if (K->kp != 0)
            {
              float32x4_t p0 = vld1q_f32 (c + a), near;
              near = vaddq_f32 (vld1q_f32 (u + a - 1), vld1q_f32 (u + a));
              near = vaddq_f32 (near, vld1q_f32 (u + a + 1));
              near = vaddq_f32 (near, vld1q_f32 (c + a - 1));
              near = vaddq_f32 (near, p0);
              near = vaddq_f32 (near, vld1q_f32 (c + a + 1));
              near = vaddq_f32 (near, vld1q_f32 (d + a - 1));
              near = vaddq_f32 (near, vld1q_f32 (d + a));
              near = vaddq_f32 (near, vld1q_f32 (d + a + 1));
              near = vmulq_n_f32 (near, 1.0f / 9);
              np = vaddq_f32 (np, vmulq_f32 (vmulq_n_f32 (m, K->kp), vsubq_f32 (p0, near)));
            }
          np = vmaxq_f32 (np, zero);
          L2 = vmulq_f32 (np, np);
          big = vcgtq_f32 (L, vdupq_n_f32 (1e-7f));
          {
            float l2v[4] __attribute__ ((aligned (16)));
            float32x4_t f;
            vst1q_f32 (l2v, L2);
            for (k = 0; k < 4; k++)
              fv[k] = lv[k] > 1e-7f ? l2v[k] / lv[k] : 0;
            f = vld1q_f32 (fv);
            for (i = 0; i < 3; i++)
              ch[i] = vbslq_f32 (big, vmulq_f32 (ch[i], f), L2);
          }
        }
      if (K->ksat != 0)
        {
          float32x4_t lum = vaddq_f32 (vaddq_f32 (vmulq_n_f32 (ch[0], 0.2126f), vmulq_n_f32 (ch[1], 0.7152f)), vmulq_n_f32 (ch[2], 0.0722f));
          float32x4_t f = vaddq_f32 (one, vmulq_n_f32 (m, K->ksat));
          for (i = 0; i < 3; i++)
            ch[i] = vaddq_f32 (lum, vmulq_f32 (vsubq_f32 (ch[i], lum), f));
        }
      for (i = 0; i < 3; i++)
        ch[i] = vminq_f32 (vmaxq_f32 (ch[i], zero), one);
      if (p->maxv == 255)
        {
          unsigned char *b = (unsigned char *) p->img + at;
          for (i = 0; i < 3; i++)
            {
              int32_t ix[4] __attribute__ ((aligned (16)));
              vst1q_s32 (ix, vcvtq_s32_f32 (vaddq_f32 (vmulq_n_f32 (ch[i], SRGB8_STEPS), vdupq_n_f32 (0.5f))));
              for (k = 0; k < 4; k++)
                if (!sk[k])
                  b[k * 3 + i] = srgb8[ix[k]];
            }
        }
      else
        {
          uint16x4x3_t out;
          for (i = 0; i < 3; i++)
            {
              uint16x4_t v = vmovn_u32 (vcvtq_u32_f32 (vaddq_f32 (vmulq_n_f32 (ch[i], 65535), vdupq_n_f32 (0.5f))));
              out.val[i] = vbsl_u16 (vmovn_u32 (skip), raw16.val[i], v);
            }
          vst3_u16 ((uint16_t *) p->img + at, out);
        }
    }
  return x;
}
#pragma GCC pop_options
#endif

static void apply_mask (const pic *p, const recipe *r, int mi)
{
  const recipe_mask *mk = &r->mask[mi];
  cover V;
  mgrid C;
  gsampler cs;
  float *cmap = NULL, *rows = NULL, *mrow = NULL;
  int x, y, i, x0, x1, y0, y1, tone = tone_used (mk), bw;
  double T = mk->v[M_TEMP] / 100.0, N = mk->v[M_TINT] / 100.0;
  double wb[3] = { 0.5 * T + 0.25 * N, -0.5 * N, -0.5 * T + 0.25 * N };
  float kc = mk->v[M_CONTRAST] / 100.0f, kh = mk->v[M_HIGHLIGHTS] / 100.0f * 1.2f, ks = mk->v[M_SHADOWS] / 100.0f * 1.2f;
  float kl = mk->v[M_CLARITY] / 100.0f * 0.8f, kp = mk->v[M_SHARPNESS] / 100.0f * 1.2f, ksat = mk->v[M_SATURATION] / 100.0f;
  int k;
  static float gain[3][1025];          /* by coverage, 1024ths */
  memset (&cs, 0, sizeof cs);
  for (k = 0; k < M_COUNT && mk->v[k] == 0; k++)
    ;
  if (k == M_COUNT)
    return;
  for (i = 0; i < 3; i++)
    for (x = 0; x <= 1024; x++)
      gain[i][x] = (float) pow (2.0, x / 1024.0 * (mk->v[M_EV] + wb[i]));
  if (cover_make (&V, r, mi, p->w, p->h) || V.x1 < 0)
    goto out;
  x0 = V.x0; x1 = V.x1; y0 = V.y0; y1 = V.y1;
  bw = x1 - x0 + 1;
  /* Clarity's surroundings (a blur of about 1.5% of the longer side) on
     a grid of their own, at most 512 cells across: no finer is needed. */
  if (tone && mk->v[M_CLARITY] != 0)
    {
      int longer = p->w > p->h ? p->w : p->h, cg = (longer + 511) / 512;
      C.g = cg;
      C.gw = (p->w + C.g - 1) / C.g;
      C.gh = (p->h + C.g - 1) / C.g;
      C.v = NULL;
      if (!(cmap = clarity_map (p, &C)) || gs_init (&cs, C.g, C.gw, C.gh, x0, bw))
        goto out;
    }
  /* Sharpness looks at the next pixels as they were: three rows of their
     lightness, kept ahead of the changes (bw + 2 wide). */
  if (tone && !(rows = malloc (sizeof *rows * 3 * (size_t) (bw + 2))))
    goto out;
  if (!(mrow = malloc (sizeof *mrow * (size_t) bw * (cmap ? 2 : 1))))
    goto out;
#define ROW(yy) (rows + (size_t) (((yy) % 3 + 3) % 3) * (bw + 2))
  if (rows)
    for (y = y0 - 1; y <= y0; y++)
      light_row (p, y, x0, bw, ROW (y));
  for (y = y0; y <= y1; y++)
    {
      float *pb_row = cmap ? mrow + bw : NULL;
      if (rows)
        light_row (p, y + 1, x0, bw, ROW (y + 1));
      /* This row's coverage (and surroundings, for clarity). */
      cover_row (&V, y, mrow);
      if (cmap)
        gs_rowf (&cs, cmap, y, pb_row);
      x = x0;
#if defined(__arm__)
      if (simd_neon ())
        {
          mask_k K = { kc, kh, ks, kl, kp, ksat, tone, (const float (*)[1025]) gain };
          x = mask_row_neon (p, y, x0, x1, mrow, pb_row, rows ? ROW (y - 1) : NULL, rows ? ROW (y) : NULL,
                             rows ? ROW (y + 1) : NULL, &K);
        }
#endif
      for (; x <= x1; x++)
        {
          float m = mrow[x - x0], c[3];
          size_t at = ((size_t) y * p->w + x) * 3;
          if (m <= 1.0f / 65535)
            continue;
          {
            int g = (int) (m * 1024 + 0.5f);
            g = g > 1024 ? 1024 : g;
            for (i = 0; i < 3; i++)
              c[i] = lin_px (p, at + i) * gain[i][g];
          }
          if (tone)
            {
              float L = 0.2126f * c[0] + 0.7152f * c[1] + 0.0722f * c[2], pl = L > 0 ? sqrtf (L) : 0, q = pl > 1 ? 1 : pl, np = pl, L2;
              int a = x - x0 + 1;
              float p0 = ROW (y)[a];
              np += m * kc * (q - 0.5f) * 2 * q * (1 - q);
              np += m * kh * q * q * q * (1 - q);
              np += m * ks * q * (1 - q) * (1 - q) * (1 - q);
              if (pb_row)
                np += m * kl * (p0 - pb_row[x - x0]) * 4 * q * (1 - q);
              if (kp != 0)
                {
                  const float *u = ROW (y - 1), *c0 = ROW (y), *d = ROW (y + 1);
                  float near = (u[a - 1] + u[a] + u[a + 1] + c0[a - 1] + c0[a] + c0[a + 1] + d[a - 1] + d[a] + d[a + 1]) * (1.0f / 9);
                  np += m * kp * (p0 - near);
                }
              np = np < 0 ? 0 : np;
              L2 = np * np;
              if (L > 1e-7f)
                {
                  float f = L2 / L;
                  c[0] *= f; c[1] *= f; c[2] *= f;
                }
              else
                c[0] = c[1] = c[2] = L2;
            }
          if (ksat != 0)
            {
              float lum = 0.2126f * c[0] + 0.7152f * c[1] + 0.0722f * c[2], f = 1 + ksat * m;
              for (i = 0; i < 3; i++)
                c[i] = lum + (c[i] - lum) * f;
            }
          for (i = 0; i < 3; i++)
            put_px (p, at + i, c[i]);
        }
    }
#undef ROW
out:
  free (mrow);
  free (rows);
  free (cmap);
  gs_free (&cs);
  cover_free (&V);
}

/* The spots (if spots), then masks m0..m1-1 that are on. */
static void apply_part (pic *p, const recipe *r, int spots, int m0, int m1)
{
  int i;
  if (spots)
    for (i = 0; i < r->nspots; i++)
      apply_spot (p, r, &r->spot[i]);
  tables ();
  for (i = m0 < 0 ? 0 : m0; i < m1 && i < r->nmasks; i++)
    if (!r->mask[i].off)
      apply_mask (p, r, i);
}

void retouch_rgb8 (unsigned char *rgb, int w, int h, const recipe *r)
{
  pic p = { w, h, 255, get8, put8, rgb };
  if ((r->nspots || r->nmasks) && w > 0 && h > 0)
    apply_part (&p, r, 1, 0, r->nmasks);
}

void retouch_lin (uint16_t *lin, int w, int h, const recipe *r)
{
  pic p = { w, h, 65535, get16, put16, lin };
  if ((r->nspots || r->nmasks) && w > 0 && h > 0)
    apply_part (&p, r, 1, 0, r->nmasks);
}

void retouch_lin_part (uint16_t *lin, int w, int h, const recipe *r, int spots, int m0, int m1, int show)
{
  pic p = { w, h, 65535, get16, put16, lin };
  cover V;
  float *row;
  int x, y;
  if (w <= 0 || h <= 0)
    return;
  if ((spots && r->nspots) || m0 < m1)
    apply_part (&p, r, spots, m0, m1);
  if (show < 0 || show >= r->nmasks || cover_make (&V, r, show, w, h))
    return;
  if (V.x1 >= 0 && (row = malloc (sizeof *row * (size_t) (V.x1 - V.x0 + 1))) != NULL)
    {
      for (y = V.y0; y <= V.y1; y++)
        {
          cover_row (&V, y, row);
          for (x = V.x0; x <= V.x1; x++)
            {
              float m = row[x - V.x0] * 0.6f;
              uint16_t *px = &lin[((size_t) y * w + x) * 3];
              if (m <= 0)
                continue;
              px[0] = (uint16_t) (px[0] * (1 - m) + 60000 * m + 0.5f);
              px[1] = (uint16_t) (px[1] * (1 - m) + 1500 * m + 0.5f);
              px[2] = (uint16_t) (px[2] * (1 - m) + 1500 * m + 0.5f);
            }
        }
      free (row);
    }
  cover_free (&V);
}

void retouch_lin_show (uint16_t *lin, int w, int h, const recipe *r, int show)
{
  retouch_lin_part (lin, w, h, r, 1, 0, r->nmasks, show);
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
