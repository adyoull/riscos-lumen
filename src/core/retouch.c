/* Copyright (c) 2026 Andrew Youll
   Released under the MIT licence: see the LICENSE file.
   SPDX-License-Identifier: MIT */
/* retouch.c: see retouch.h. */
#include <math.h>
#include <stdlib.h>
#include <string.h>
#include "retouch.h"

#define EDGE_POINTS 24                 /* round the circle, for healing */

/* One spot on a picture of w x h, channel values read and written through
   get and put (0..maxv). */
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

static void apply_spot (const pic *p, const recipe_spot *s)
{
  int longer = p->w > p->h ? p->w : p->h;
  double cx = s->x * p->w, cy = s->y * p->h, R = s->r * longer;
  double ox = (s->sx - s->x) * p->w, oy = (s->sy - s->y) * p->h;
  double feather = s->f < 0.02f ? 0.02 : s->f;   /* the outer part of the radius that fades */
  double ex[EDGE_POINTS], ey[EDGE_POINTS], delta[EDGE_POINTS][3], plane[3][3] = { { 0 } }, spread = 0;
  int x0, x1, y0, y1, x, y, k, c, bw, bh;
  double *patch;
  if (R < 0.75 || (fabs (ox) < 0.5 && fabs (oy) < 0.5))
    return;
  x0 = clampi ((int) floor (cx - R), 0, p->w - 1);
  x1 = clampi ((int) ceil (cx + R), 0, p->w - 1);
  y0 = clampi ((int) floor (cy - R), 0, p->h - 1);
  y1 = clampi ((int) ceil (cy + R), 0, p->h - 1);
  bw = x1 - x0 + 1;
  bh = y1 - y0 + 1;
  /* The source's pixels first: it may overlap the spot. */
  patch = malloc (sizeof *patch * 3 * (size_t) bw * bh);
  if (!patch)
    return;
  for (y = y0; y <= y1; y++)
    for (x = x0; x <= x1; x++)
      {
        int sx = clampi ((int) floor (x + ox + 0.5), 0, p->w - 1), sy = clampi ((int) floor (y + oy + 0.5), 0, p->h - 1);
        for (c = 0; c < 3; c++)
          patch[(((size_t) (y - y0) * bw) + (x - x0)) * 3 + c] = p->get (p->img, p->w, sx, sy, c);
      }
  /* Healing: the difference between the spot's surroundings and the
     source's, at points just outside the circle. */
  for (k = 0; k < EDGE_POINTS; k++)
    {
      double a = 2 * 3.14159265358979 * k / EDGE_POINTS, d[3], sv[3];
      ex[k] = cx + cos (a) * R * 1.1;
      ey[k] = cy + sin (a) * R * 1.1;
      if (s->clone)
        {
          delta[k][0] = delta[k][1] = delta[k][2] = 0;
          continue;
        }
      sample3 (p, ex[k], ey[k], d);
      sample3 (p, ex[k] + ox, ey[k] + oy, sv);
      for (c = 0; c < 3; c++)
        delta[k][c] = d[c] - sv[c];
    }
  /* The difference's slope across the circle (light that grades across
     it, as water darkening towards a reflection), fitted to a plane: the
     edge points are evenly round the centre, so the least-squares fit is
     the mean and two sums. What the plane leaves is blended in below. */
  if (!s->clone)
    {
      for (k = 0; k < EDGE_POINTS; k++)
        spread += (ex[k] - cx) * (ex[k] - cx);
      for (c = 0; c < 3; c++)
        {
          for (k = 0; k < EDGE_POINTS; k++)
            {
              plane[c][0] += delta[k][c] / EDGE_POINTS;
              plane[c][1] += delta[k][c] * (ex[k] - cx) / spread;
              plane[c][2] += delta[k][c] * (ey[k] - cy) / spread;
            }
          for (k = 0; k < EDGE_POINTS; k++)
            delta[k][c] -= plane[c][0] + plane[c][1] * (ex[k] - cx) + plane[c][2] * (ey[k] - cy);
        }
    }
  for (y = y0; y <= y1; y++)
    for (x = x0; x <= x1; x++)
      {
        double fx = x + 0.5 - cx, fy = y + 0.5 - cy, dist = sqrt (fx * fx + fy * fy), m, add[3] = { 0, 0, 0 };
        if (dist >= R)
          continue;
        /* The edge fades: 1 inside, smoothly to 0 at the circle. */
        m = dist <= R * (1 - feather) ? 1 : (R - dist) / (R * feather);
        m = m * m * (3 - 2 * m);
        if (!s->clone)
          {
            double wsum = 0;
            for (k = 0; k < EDGE_POINTS; k++)
              {
                double qx = x + 0.5 - ex[k], qy = y + 0.5 - ey[k], wgt = 1.0 / (qx * qx + qy * qy + 1);
                wsum += wgt;
                for (c = 0; c < 3; c++)
                  add[c] += wgt * delta[k][c];
              }
            for (c = 0; c < 3; c++)
              add[c] = add[c] / wsum + plane[c][0] + plane[c][1] * (x + 0.5 - cx) + plane[c][2] * (y + 0.5 - cy);
          }
        for (c = 0; c < 3; c++)
          {
            double orig = p->get (p->img, p->w, x, y, c);
            double fill = patch[(((size_t) (y - y0) * bw) + (x - x0)) * 3 + c] + add[c];
            p->put (p->img, p->w, x, y, c, orig + (fill - orig) * m);
          }
      }
  free (patch);
}

/* --- brushed adjustments */

static float lin_of8[256];             /* sRGB 8-bit to linear 0..1 */

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
  return 255 * (v <= 0.0031308 ? v * 12.92 : 1.055 * pow (v, 1 / 2.4) - 0.055);
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

static void apply_stroke (const pic *p, const recipe *r, const recipe_stroke *st)
{
  int longer = p->w > p->h ? p->w : p->h, j, x, y, x0, y0, x1, y1, bw, bh;
  double R = st->r * longer, inner, feather = st->f < 0.02f ? 0.02 : st->f;
  double stops = 2.0 * st->amount, sat = st->amount;
  float *mask;
  if (R < 0.75 || st->n <= 0 || st->amount <= 0)
    return;
  inner = R * (1 - feather);
  /* Its bounds. */
  {
    double mx0 = 1e9, my0 = 1e9, mx1 = -1e9, my1 = -1e9;
    for (j = 0; j < st->n; j++)
      {
        double px = r->pt[st->first + j][0] / 10000.0 * p->w, py = r->pt[st->first + j][1] / 10000.0 * p->h;
        mx0 = px < mx0 ? px : mx0; mx1 = px > mx1 ? px : mx1;
        my0 = py < my0 ? py : my0; my1 = py > my1 ? py : my1;
      }
    x0 = clampi ((int) floor (mx0 - R), 0, p->w - 1); x1 = clampi ((int) ceil (mx1 + R), 0, p->w - 1);
    y0 = clampi ((int) floor (my0 - R), 0, p->h - 1); y1 = clampi ((int) ceil (my1 + R), 0, p->h - 1);
  }
  bw = x1 - x0 + 1;
  bh = y1 - y0 + 1;
  if (!(mask = calloc ((size_t) bw * bh, sizeof *mask)))
    return;
  /* The mask: each segment (or the one point) in turn, the most any gives. */
  for (j = 0; j < st->n; j++)
    {
      int k = j + 1 < st->n ? j + 1 : j;
      double ax = r->pt[st->first + j][0] / 10000.0 * p->w, ay = r->pt[st->first + j][1] / 10000.0 * p->h;
      double bx = r->pt[st->first + k][0] / 10000.0 * p->w, by = r->pt[st->first + k][1] / 10000.0 * p->h;
      int sx0 = clampi ((int) floor ((ax < bx ? ax : bx) - R), x0, x1), sx1 = clampi ((int) ceil ((ax > bx ? ax : bx) + R), x0, x1);
      int sy0 = clampi ((int) floor ((ay < by ? ay : by) - R), y0, y1), sy1 = clampi ((int) ceil ((ay > by ? ay : by) + R), y0, y1);
      if (j + 1 >= st->n && st->n > 1)
        break;                         /* the last point ends the last segment */
      for (y = sy0; y <= sy1; y++)
        for (x = sx0; x <= sx1; x++)
          {
            double d = seg_dist (x + 0.5, y + 0.5, ax, ay, bx, by), m;
            float *mp = &mask[(size_t) (y - y0) * bw + (x - x0)];
            if (d >= R || *mp >= 1)
              continue;
            m = d <= inner ? 1 : (R - d) / (R - inner);
            m = m * m * (3 - 2 * m);
            if (m > *mp)
              *mp = (float) m;
          }
    }
  for (y = y0; y <= y1; y++)
    for (x = x0; x <= x1; x++)
      {
        double m = mask[(size_t) (y - y0) * bw + (x - x0)], c[3], lum;
        int i;
        if (m <= 0)
          continue;
        for (i = 0; i < 3; i++)
          c[i] = to_lin (p, p->get (p->img, p->w, x, y, i));
        switch (st->kind)
          {
          case BRUSH_DODGE:
          case BRUSH_BURN:
            {
              double k = pow (2.0, (st->kind == BRUSH_DODGE ? 1 : -1) * stops * m);
              for (i = 0; i < 3; i++)
                c[i] *= k;
              break;
            }
          default:
            lum = 0.2126 * c[0] + 0.7152 * c[1] + 0.0722 * c[2];
            for (i = 0; i < 3; i++)
              c[i] = lum + (c[i] - lum) * (1 + (st->kind == BRUSH_SATURATE ? sat : -sat) * m);
            break;
          }
        for (i = 0; i < 3; i++)
          p->put (p->img, p->w, x, y, i, from_lin (p, c[i]));
      }
  free (mask);
}

static void apply_all (pic *p, const recipe *r)
{
  int i;
  for (i = 0; i < r->nspots; i++)
    apply_spot (p, &r->spot[i]);
  tables ();
  for (i = 0; i < r->nstrokes; i++)
    apply_stroke (p, r, &r->stroke[i]);
}

void retouch_rgb8 (unsigned char *rgb, int w, int h, const recipe *r)
{
  pic p = { w, h, 255, get8, put8, rgb };
  if ((r->nspots || r->nstrokes) && w > 0 && h > 0)
    apply_all (&p, r);
}

void retouch_lin (uint16_t *lin, int w, int h, const recipe *r)
{
  pic p = { w, h, 65535, get16, put16, lin };
  if ((r->nspots || r->nstrokes) && w > 0 && h > 0)
    apply_all (&p, r);
}

void retouch_auto_source (const uint16_t *lin, int w, int h, float x, float y, float r, float *sx, float *sy)
{
  pic p = { w, h, 65535, get16, put16, (void *) lin };
  int longer = w > h ? w : h, k, j, ring;
  double cx = x * w, cy = y * h, R = r * longer, best = -1;
  double edge[16][3];
  *sx = x + r * 2.5f * (float) longer / w;   /* to the right, if nothing better */
  *sy = y;
  if (w <= 0 || h <= 0)
    return;
  /* The spot's surroundings: a ring just outside it. */
  for (j = 0; j < 16; j++)
    {
      double a = 2 * 3.14159265358979 * j / 16;
      sample3 (&p, cx + cos (a) * R * 1.2, cy + sin (a) * R * 1.2, edge[j]);
    }
  /* Candidates round it, two and a half and four radii away: the one whose
     own ring (and middle) looks most like the spot's ring. */
  for (ring = 0; ring < 2; ring++)
    for (k = 0; k < 16; k++)
      {
        double a = 2 * 3.14159265358979 * (k + 0.5 * ring) / 16, dist = R * (ring ? 4.0 : 2.5);
        double qx = cx + cos (a) * dist, qy = cy + sin (a) * dist, score = 0, mid[3];
        if (qx - R * 1.2 < 0 || qy - R * 1.2 < 0 || qx + R * 1.2 > w - 1 || qy + R * 1.2 > h - 1)
          continue;                    /* off the picture */
        {
          double own[3] = { 0, 0, 0 };
          int c;
          for (j = 0; j < 16; j++)
            {
              double b = 2 * 3.14159265358979 * j / 16, v[3];
              sample3 (&p, qx + cos (b) * R * 1.2, qy + sin (b) * R * 1.2, v);
              for (c = 0; c < 3; c++)
                {
                  score += (v[c] - edge[j][c]) * (v[c] - edge[j][c]);
                  own[c] += v[c] / 16;
                }
            }
          /* Its middle should be like its own edge (no blemish of its own). */
          sample3 (&p, qx, qy, mid);
          for (c = 0; c < 3; c++)
            score += 4 * (mid[c] - own[c]) * (mid[c] - own[c]) * 16;
        }
        score *= ring ? 1.15 : 1;      /* nearer is a little better */
        if (best < 0 || score < best)
          {
            best = score;
            *sx = (float) (qx / w);
            *sy = (float) (qy / h);
          }
      }
}
