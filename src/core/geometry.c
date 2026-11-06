/* Copyright (c) 2026 Andrew Youll
   Released under the MIT licence: see the LICENSE file.
   SPDX-License-Identifier: MIT */
/* geometry.c: see geometry.h. */
#include <math.h>
#include <stdlib.h>
#include "geometry.h"

#define PERSP_SCALE 0.6                /* +100 = edges 30% wider or narrower than the middle */
#define MIN_CROP 0.02                  /* a crop is at least 2% of the canvas each way */
#define MANUAL_SCALE 0.2               /* Manual +100: the corners taken from 20% further in */

/* --- lens correction (recipe.h has the models) */

/* rd / ru for the profile's distortion. */
static double dist_ratio (const geom_plan *g, double ru)
{
  double r2 = ru * ru;
  switch (g->dm)
    {
    case LENS_D_PTLENS: return g->d[0] * r2 * ru + g->d[1] * r2 + g->d[2] * ru + 1 - g->d[0] - g->d[1] - g->d[2];
    case LENS_D_POLY3: return 1 - g->d[0] + g->d[0] * r2;
    case LENS_D_POLY5: return 1 + g->d[0] * r2 + g->d[1] * r2 * r2;
    }
  return 1;
}

/* A point of the corrected picture (pixels from the source's centre) is
   at this many times the distance in the photo. */
static double lens_factor (const geom_plan *g, double px, double py)
{
  double ux = px * g->ls, uy = py * g->ls, h, m = 1;
  if (!g->dm && g->man == 0)
    return g->ls;
  h = sqrt (ux * ux + uy * uy) / g->lhd;   /* 1 at the corner */
  if (g->dm)
    m = 1 + g->damt * (dist_ratio (g, h * g->rh) - 1);
  if (g->man != 0)
    m *= 1 + g->man * h * h;
  return g->ls * m;
}

static void lens_apply (const geom_plan *g, double *sx, double *sy)
{
  double px = *sx - g->lcx, py = *sy - g->lcy, f = lens_factor (g, px, py);
  *sx = g->lcx + px * f;
  *sy = g->lcy + py * f;
}

/* Whether the whole corrected frame, at fill ls, comes from inside the photo. */
static int lens_fills (geom_plan *g, double ls)
{
  int k, e;
  g->ls = ls;
  for (k = 0; k <= 32; k++)
    {
      double t = k / 32.0 - 0.5, hw = 0.5 * g->sw, hh = 0.5 * g->sh;
      double pts[4][2] = { { t * g->sw, -hh }, { t * g->sw, hh }, { -hw, t * g->sh }, { hw, t * g->sh } };
      for (e = 0; e < 4; e++)
        {
          double f = lens_factor (g, pts[e][0], pts[e][1]);
          if (fabs (pts[e][0] * f) > hw * 1.0001 || fabs (pts[e][1] * f) > hh * 1.0001)
            return 0;
        }
    }
  return 1;
}

static void lens_plan (geom_plan *g, const recipe *r)
{
  const recipe_lens *L = &r->lens;
  double lo = 0.2, hi = 1.6;
  int i;
  g->lens = 0;
  g->dm = g->tm = g->vm = 0;
  g->man = -r->v[P_LENS_MANUAL] / 100.0 * MANUAL_SCALE;
  g->damt = r->v[P_LENS_DIST] / 100.0;
  g->tamt = r->v[P_LENS_TCA] / 100.0;
  g->vamt = r->v[P_LENS_VIG] / 100.0;
  g->rh = L->rh;
  g->rv = L->rv;
  g->ls = 1;
  if (L->on)
    {
      if (L->dist && g->damt > 0)
        g->dm = L->dist;
      if (L->tca && g->tamt > 0)
        g->tm = L->tca;
      if (L->vig && g->vamt > 0)
        g->vm = 1;
      for (i = 0; i < 3; i++)
        {
          g->d[i] = L->d[i];
          g->k[i] = L->k[i];
        }
      for (i = 0; i < 6; i++)
        g->t[i] = L->t[i];
    }
  if (!g->dm && !g->tm && !g->vm && g->man == 0)
    return;
  g->lens = 1;
  g->lcx = g->sw / 2.0;
  g->lcy = g->sh / 2.0;
  g->lhd = sqrt (g->lcx * g->lcx + g->lcy * g->lcy);
  if (!g->dm && g->man == 0)
    return;
  /* The most of the photo that still fills the frame. */
  if (lens_fills (g, hi))
    lo = hi;
  else
    for (i = 0; i < 40; i++)
      {
        double mid = (lo + hi) / 2;
        if (lens_fills (g, mid))
          lo = mid;
        else
          hi = mid;
      }
  g->ls = lo / 1.0005;                 /* a hair in, so bilinear sampling stays inside */
}

/* Red (c 0) or blue (2) is this many times further out than green, at r. */
static double tca_factor (const geom_plan *g, int c, double r)
{
  double f;
  if (g->tm == LENS_T_LINEAR)
    f = c == 0 ? g->t[0] : g->t[1];
  else
    {
      const double *t = g->t + (c == 0 ? 0 : 3);
      f = t[0] + t[1] * r + t[2] * r * r;
    }
  return 1 + g->tamt * (f - 1);
}

/* Canvas point (pixels from the canvas centre, y down) to the canvas
   before straightening and perspective, at zoom 1. */
static void undo_straighten (const geom_plan *g, double X, double Y, double *x, double *y)
{
  double u, v;
  /* Perspective: the forward transform scales x by (1 + kv * y/ch) and y
     by (1 + kh * x/cw); undo horizontal, then vertical. */
  v = Y / (1 + g->kh * X / g->cw);
  u = X / (1 + g->kv * v / g->ch);
  /* Straightening: the picture was turned clockwise by the angle. */
  *x = u * g->cs + v * g->sn;
  *y = -u * g->sn + v * g->cs;
}

/* Canvas pixel (from its top left) to source pixel, undoing the quarter turns. */
static void canvas_to_source (const geom_plan *g, double cx, double cy, double *sx, double *sy)
{
  switch (g->quarter)
    {
    case 1: *sx = cy; *sy = g->sh - cx; break;            /* 90 clockwise */
    case 2: *sx = g->sw - cx; *sy = g->sh - cy; break;    /* 180 */
    case 3: *sx = g->sw - cy; *sy = cx; break;            /* 90 anticlockwise */
    default: *sx = cx; *sy = cy; break;
    }
}

void geom_to_source (const geom_plan *g, double x, double y, double *sx, double *sy)
{
  /* Output pixel -> fraction of the canvas -> pixels from its centre. */
  double fu = g->x0 + x / g->ow * (g->x1 - g->x0);
  double fv = g->y0 + y / g->oh * (g->y1 - g->y0);
  double X = (fu - 0.5) * g->cw / g->zoom, Y = (fv - 0.5) * g->ch / g->zoom, u, v;
  undo_straighten (g, X, Y, &u, &v);
  canvas_to_source (g, u + g->cw / 2.0, v + g->ch / 2.0, sx, sy);
  if (g->lens)
    lens_apply (g, sx, sy);
}

void geom_to_source_rgb (const geom_plan *g, double x, double y, double sx[3], double sy[3], double *gain)
{
  double qx, qy, q;
  geom_to_source (g, x, y, &sx[1], &sy[1]);
  sx[0] = sx[2] = sx[1];
  sy[0] = sy[2] = sy[1];
  *gain = 1;
  if (!g->lens || (!g->tm && !g->vm))
    return;
  qx = sx[1] - g->lcx;
  qy = sy[1] - g->lcy;
  q = sqrt (qx * qx + qy * qy) / g->lhd;
  if (g->tm)
    {
      double fr = tca_factor (g, 0, q * g->rh), fb = tca_factor (g, 2, q * g->rh);
      sx[0] = g->lcx + qx * fr;
      sy[0] = g->lcy + qy * fr;
      sx[2] = g->lcx + qx * fb;
      sy[2] = g->lcy + qy * fb;
    }
  if (g->vm)
    {
      double r2 = q * g->rv * q * g->rv;
      double c = 1 + g->k[0] * r2 + g->k[1] * r2 * r2 + g->k[2] * r2 * r2 * r2;
      if (c < 0.2)
        c = 0.2;
      *gain = 1 + g->vamt * (1 / c - 1);
      if (*gain < 0.2)
        *gain = 0.2;
    }
}

int geom_plan_is_simple (const geom_plan *g)
{
  return g->quarter == 0 && g->sn == 0 && g->kv == 0 && g->kh == 0 && !g->lens;
}

/* The other way: where a source pixel ends up in the output (it may be
   outside it). Newton's method on geom_to_source, from a first guess that
   ignores straightening and perspective; a few steps are plenty. */
void geom_from_source (const geom_plan *g, double sx, double sy, double *x, double *y)
{
  double ox = g->ow / 2.0, oy = g->oh / 2.0;
  int it;
  for (it = 0; it < 12; it++)
    {
      double fx, fy, ax, ay, bx, by, j00, j01, j10, j11, det, ex, ey, h = 0.5;
      geom_to_source (g, ox, oy, &fx, &fy);
      ex = fx - sx;
      ey = fy - sy;
      if (ex * ex + ey * ey < 1e-6)
        break;
      geom_to_source (g, ox + h, oy, &ax, &ay);
      geom_to_source (g, ox, oy + h, &bx, &by);
      j00 = (ax - fx) / h; j10 = (ay - fy) / h;
      j01 = (bx - fx) / h; j11 = (by - fy) / h;
      det = j00 * j11 - j01 * j10;
      if (det == 0)
        break;
      ox -= (j11 * ex - j01 * ey) / det;
      oy -= (-j10 * ex + j00 * ey) / det;
    }
  *x = ox;
  *y = oy;
}

static double clampd (double v, double lo, double hi)
{
  return v < lo ? lo : v > hi ? hi : v;
}

void geom_plan_make (geom_plan *g, const recipe *r, int sw, int sh, int with_crop, int out_w, int out_h)
{
  double a = r->v[P_ANGLE] * M_PI / 180.0;
  int k;
  g->sw = sw;
  g->sh = sh;
  g->quarter = ((int) r->v[P_ROTATE]) & 3;
  g->cw = g->quarter & 1 ? sh : sw;
  g->ch = g->quarter & 1 ? sw : sh;
  g->cs = cos (a);
  g->sn = sin (a);
  g->kv = -r->v[P_PERSP_V] / 100.0 * PERSP_SCALE;  /* + widens the top */
  g->kh = r->v[P_PERSP_H] / 100.0 * PERSP_SCALE;   /* + widens the right */
  /* Zoom: enough that every point along the canvas's edges comes from
     inside the picture. */
  g->zoom = 1;
  for (k = 0; k <= 32; k++)
    {
      double t = k / 32.0 - 0.5;
      double pts[4][2] = { { t * g->cw, -0.5 * g->ch }, { t * g->cw, 0.5 * g->ch },
                           { -0.5 * g->cw, t * g->ch }, { 0.5 * g->cw, t * g->ch } };
      int e;
      for (e = 0; e < 4; e++)
        {
          double x, y, need;
          undo_straighten (g, pts[e][0], pts[e][1], &x, &y);
          need = fabs (x) / (0.5 * g->cw);
          if (fabs (y) / (0.5 * g->ch) > need)
            need = fabs (y) / (0.5 * g->ch);
          if (need > g->zoom)
            g->zoom = need;
        }
    }
  g->zoom *= 1.0005;                   /* a hair more, so bilinear sampling stays inside */
  lens_plan (g, r);
  if (with_crop)
    {
      g->x0 = clampd (r->v[P_CROP_X0], 0, 1 - MIN_CROP);
      g->y0 = clampd (r->v[P_CROP_Y0], 0, 1 - MIN_CROP);
      g->x1 = clampd (r->v[P_CROP_X1], g->x0 + MIN_CROP, 1);
      g->y1 = clampd (r->v[P_CROP_Y1], g->y0 + MIN_CROP, 1);
    }
  else
    {
      g->x0 = g->y0 = 0;
      g->x1 = g->y1 = 1;
    }
  if (out_w > 0 && out_h > 0)
    {
      g->ow = out_w;
      g->oh = out_h;
    }
  else
    {
      g->ow = (int) lround ((g->x1 - g->x0) * g->cw);
      g->oh = (int) lround ((g->y1 - g->y0) * g->ch);
      if (g->ow < 1) g->ow = 1;
      if (g->oh < 1) g->oh = 1;
    }
}

/* One channel at (sx, sy) of 3-channel pixels, bilinear, clamped. */
static double sample16 (const uint16_t *src, int w, int h, double sx, double sy, int c)
{
  int ix, iy, ix1, iy1;
  double fx, fy, top, bot;
  const uint16_t *r0, *r1;
  sx = clampd (sx - 0.5, 0, w - 1);
  sy = clampd (sy - 0.5, 0, h - 1);
  ix = (int) sx;
  iy = (int) sy;
  fx = sx - ix;
  fy = sy - iy;
  ix1 = ix + 1 < w ? ix + 1 : ix;
  iy1 = iy + 1 < h ? iy + 1 : iy;
  r0 = src + (size_t) iy * w * 3;
  r1 = src + (size_t) iy1 * w * 3;
  top = r0[ix * 3 + c] + (r0[ix1 * 3 + c] - r0[ix * 3 + c]) * fx;
  bot = r1[ix * 3 + c] + (r1[ix1 * 3 + c] - r1[ix * 3 + c]) * fx;
  return top + (bot - top) * fy;
}

static double sample8 (const unsigned char *src, const uint16_t *s2l, int w, int h, double sx, double sy, int c)
{
  int ix, iy, ix1, iy1;
  double fx, fy, top, bot;
  const unsigned char *r0, *r1;
  sx = clampd (sx - 0.5, 0, w - 1);
  sy = clampd (sy - 0.5, 0, h - 1);
  ix = (int) sx;
  iy = (int) sy;
  fx = sx - ix;
  fy = sy - iy;
  ix1 = ix + 1 < w ? ix + 1 : ix;
  iy1 = iy + 1 < h ? iy + 1 : iy;
  r0 = src + (size_t) iy * w * 3;
  r1 = src + (size_t) iy1 * w * 3;
  top = s2l[r0[ix * 3 + c]] + (s2l[r0[ix1 * 3 + c]] - s2l[r0[ix * 3 + c]]) * fx;
  bot = s2l[r1[ix * 3 + c]] + (s2l[r1[ix1 * 3 + c]] - s2l[r1[ix * 3 + c]]) * fx;
  return top + (bot - top) * fy;
}

static uint16_t to16 (double v)
{
  return (uint16_t) (v <= 0 ? 0 : v >= 65535 ? 65535 : v + 0.5);
}

void geom_rows (const geom_plan *g, const uint16_t *src, uint16_t *dst, int y0, int y1)
{
  int x, y, c;
  int simple = geom_plan_is_simple (g);
  for (y = y0; y < y1; y++)
    {
      uint16_t *o = dst + (size_t) y * g->ow * 3;
      if (g->lens)
        {
          for (x = 0; x < g->ow; x++, o += 3)
            {
              double sx[3], sy[3], gain;
              geom_to_source_rgb (g, x + 0.5, y + 0.5, sx, sy, &gain);
              for (c = 0; c < 3; c++)
                o[c] = to16 (sample16 (src, g->sw, g->sh, sx[c], sy[c], c) * gain);
            }
          continue;
        }
      for (x = 0; x < g->ow; x++, o += 3)
        {
          double sx, sy, fx, fy;
          int ix, iy, ix1, iy1;
          const uint16_t *p00, *p10, *p01, *p11;
          if (simple)
            {
              /* Only a crop and/or a resize: no trigonometry. */
              sx = (g->x0 + (x + 0.5) / g->ow * (g->x1 - g->x0)) * g->sw;
              sy = (g->y0 + (y + 0.5) / g->oh * (g->y1 - g->y0)) * g->sh;
            }
          else
            geom_to_source (g, x + 0.5, y + 0.5, &sx, &sy);
          /* Bilinear, pixel centres at +0.5, clamped to the picture. */
          sx -= 0.5;
          sy -= 0.5;
          sx = clampd (sx, 0, g->sw - 1);
          sy = clampd (sy, 0, g->sh - 1);
          ix = (int) sx;
          iy = (int) sy;
          fx = sx - ix;
          fy = sy - iy;
          ix1 = ix + 1 < g->sw ? ix + 1 : ix;
          iy1 = iy + 1 < g->sh ? iy + 1 : iy;
          p00 = src + ((size_t) iy * g->sw + ix) * 3;
          p10 = src + ((size_t) iy * g->sw + ix1) * 3;
          p01 = src + ((size_t) iy1 * g->sw + ix) * 3;
          p11 = src + ((size_t) iy1 * g->sw + ix1) * 3;
          for (c = 0; c < 3; c++)
            {
              double top = p00[c] + (p10[c] - p00[c]) * fx;
              double bot = p01[c] + (p11[c] - p01[c]) * fx;
              o[c] = (uint16_t) (top + (bot - top) * fy + 0.5);
            }
        }
    }
}

void geom_crop_aspect (double *x0, double *y0, double *x1, double *y1, double aspect, int cw, int ch)
{
  double w = (*x1 - *x0) * cw, h = (*y1 - *y0) * ch;
  double cx = (*x0 + *x1) / 2 * cw, cy = (*y0 + *y1) / 2 * ch;
  if (aspect <= 0 || w <= 0 || h <= 0)
    return;
  /* Keep the area's larger side that fits, shrink the other. */
  if (w / h > aspect)
    w = h * aspect;
  else
    h = w / aspect;
  if (w > cw) { w = cw; h = w / aspect; }
  if (h > ch) { h = ch; w = h * aspect; }
  if (cx - w / 2 < 0) cx = w / 2;
  if (cx + w / 2 > cw) cx = cw - w / 2;
  if (cy - h / 2 < 0) cy = h / 2;
  if (cy + h / 2 > ch) cy = ch - h / 2;
  *x0 = (cx - w / 2) / cw;
  *x1 = (cx + w / 2) / cw;
  *y0 = (cy - h / 2) / ch;
  *y1 = (cy + h / 2) / ch;
}

void geom_rows_rgb8 (const geom_plan *g, const unsigned char *src, const uint16_t *s2l,
                     uint16_t *dst, int y0, int y1, int dst_row0)
{
  geom_region_rgb8 (g, src, s2l, dst + (size_t) (y0 - dst_row0) * g->ow * 3, 0, y0, g->ow, y1 - y0);
}

void geom_region_rgb8 (const geom_plan *g, const unsigned char *src, const uint16_t *s2l,
                       uint16_t *dst, int rx, int ry, int rw, int rh)
{
  int x, y, c;
  int simple = geom_plan_is_simple (g);
  /* Not turned, straightened or put in perspective: where each column
     comes from is the same on every row, so it is worked out once (the
     same sums as below, so the same pixels; no divisions per pixel). */
  if (simple)
    {
      int *cix = malloc ((size_t) rw * 2 * sizeof *cix);
      double *cfx = malloc ((size_t) rw * sizeof *cfx);
      if (cix && cfx)
        {
          for (x = 0; x < rw; x++)
            {
              double sx = (g->x0 + (rx + x + 0.5) / g->ow * (g->x1 - g->x0)) * g->sw;
              int ix;
              sx = clampd (sx - 0.5, 0, g->sw - 1);
              ix = (int) sx;
              cfx[x] = sx - ix;
              cix[2 * x] = ix * 3;
              cix[2 * x + 1] = (ix + 1 < g->sw ? ix + 1 : ix) * 3;
            }
          for (y = ry; y < ry + rh; y++)
            {
              uint16_t *o = dst + (size_t) (y - ry) * rw * 3;
              double sy = (g->y0 + (y + 0.5) / g->oh * (g->y1 - g->y0)) * g->sh, fy;
              int iy, iy1;
              const unsigned char *r0, *r1;
              sy = clampd (sy - 0.5, 0, g->sh - 1);
              iy = (int) sy;
              fy = sy - iy;
              iy1 = iy + 1 < g->sh ? iy + 1 : iy;
              r0 = src + (size_t) iy * g->sw * 3;
              r1 = src + (size_t) iy1 * g->sw * 3;
              for (x = 0; x < rw; x++, o += 3)
                {
                  const unsigned char *p00 = r0 + cix[2 * x], *p10 = r0 + cix[2 * x + 1];
                  const unsigned char *p01 = r1 + cix[2 * x], *p11 = r1 + cix[2 * x + 1];
                  double fx = cfx[x];
                  if (fx == 0 && fy == 0)
                    {
                      /* On a source pixel (exporting at full size, uncropped). */
                      o[0] = s2l[p00[0]];
                      o[1] = s2l[p00[1]];
                      o[2] = s2l[p00[2]];
                      continue;
                    }
                  for (c = 0; c < 3; c++)
                    {
                      double top = s2l[p00[c]] + (s2l[p10[c]] - s2l[p00[c]]) * fx;
                      double bot = s2l[p01[c]] + (s2l[p11[c]] - s2l[p01[c]]) * fx;
                      o[c] = (uint16_t) (top + (bot - top) * fy + 0.5);
                    }
                }
            }
          free (cix);
          free (cfx);
          return;
        }
      free (cix);
      free (cfx);
    }
  for (y = ry; y < ry + rh; y++)
    {
      uint16_t *o = dst + (size_t) (y - ry) * rw * 3;
      if (g->lens)
        {
          for (x = rx; x < rx + rw; x++, o += 3)
            {
              double sx[3], sy[3], gain;
              geom_to_source_rgb (g, x + 0.5, y + 0.5, sx, sy, &gain);
              for (c = 0; c < 3; c++)
                o[c] = to16 (sample8 (src, s2l, g->sw, g->sh, sx[c], sy[c], c) * gain);
            }
          continue;
        }
      for (x = rx; x < rx + rw; x++, o += 3)
        {
          double sx, sy, fx, fy;
          int ix, iy, ix1, iy1;
          const unsigned char *p00, *p10, *p01, *p11;
          if (simple)
            {
              sx = (g->x0 + (x + 0.5) / g->ow * (g->x1 - g->x0)) * g->sw;
              sy = (g->y0 + (y + 0.5) / g->oh * (g->y1 - g->y0)) * g->sh;
            }
          else
            geom_to_source (g, x + 0.5, y + 0.5, &sx, &sy);
          sx = clampd (sx - 0.5, 0, g->sw - 1);
          sy = clampd (sy - 0.5, 0, g->sh - 1);
          ix = (int) sx;
          iy = (int) sy;
          fx = sx - ix;
          fy = sy - iy;
          ix1 = ix + 1 < g->sw ? ix + 1 : ix;
          iy1 = iy + 1 < g->sh ? iy + 1 : iy;
          p00 = src + ((size_t) iy * g->sw + ix) * 3;
          p10 = src + ((size_t) iy * g->sw + ix1) * 3;
          p01 = src + ((size_t) iy1 * g->sw + ix) * 3;
          p11 = src + ((size_t) iy1 * g->sw + ix1) * 3;
          /* Interpolated in linear light, as the proxy is. */
          for (c = 0; c < 3; c++)
            {
              double top = s2l[p00[c]] + (s2l[p10[c]] - s2l[p00[c]]) * fx;
              double bot = s2l[p01[c]] + (s2l[p11[c]] - s2l[p01[c]]) * fx;
              o[c] = (uint16_t) (top + (bot - top) * fy + 0.5);
            }
        }
    }
}
