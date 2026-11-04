/* geometry.c: see geometry.h. */
#include <math.h>
#include <stdlib.h>
#include "geometry.h"

#define PERSP_SCALE 0.6                /* +100 = edges 30% wider or narrower than the middle */
#define MIN_CROP 0.02                  /* a crop is at least 2% of the canvas each way */

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

void geom_rows (const geom_plan *g, const uint16_t *src, uint16_t *dst, int y0, int y1)
{
  int x, y, c;
  int simple = g->quarter == 0 && g->sn == 0 && g->kv == 0 && g->kh == 0;
  for (y = y0; y < y1; y++)
    {
      uint16_t *o = dst + (size_t) y * g->ow * 3;
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
  int simple = g->quarter == 0 && g->sn == 0 && g->kv == 0 && g->kh == 0;
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
