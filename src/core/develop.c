/* develop.c: see develop.h. Float maths only when building tables;
   the per-pixel work is integer. */
#include <math.h>
#include <string.h>
#include <stdlib.h>
#include "develop.h"
#include "geometry.h"

static uint16_t s2l[256];
static uint8_t l2s[65536];
static int recip[256];                 /* 65536 / (i + 1): vibrancy's relative spread without dividing */
static int tables_made;

static double srgb_decode (double v)  /* sRGB-encoded 0-1 to linear */
{
  return v <= 0.04045 ? v / 12.92 : pow ((v + 0.055) / 1.055, 2.4);
}

static double srgb_encode (double v)  /* linear 0-1 to sRGB-encoded */
{
  return v <= 0.0031308 ? v * 12.92 : 1.055 * pow (v, 1 / 2.4) - 0.055;
}

static void make_tables (void)
{
  int i;
  if (tables_made)
    return;
  for (i = 0; i < 256; i++)
    s2l[i] = (uint16_t) lround (srgb_decode (i / 255.0) * 65535);
  for (i = 0; i < 65536; i++)
    l2s[i] = (uint8_t) lround (srgb_encode (i / 65535.0) * 255);
  for (i = 0; i < 256; i++)
    recip[i] = 65536 / (i + 1);
  tables_made = 1;
}

const uint16_t *dev_srgb_to_linear (void) { make_tables (); return s2l; }
const uint8_t *dev_linear_to_srgb (void) { make_tables (); return l2s; }

void dev_to_linear (const unsigned char *rgb, uint16_t *lin, size_t pixels)
{
  size_t i;
  make_tables ();
  for (i = 0; i < pixels * 3; i++)
    lin[i] = s2l[rgb[i]];
}

static double clamp01 (double v)
{
  return v < 0 ? 0 : v > 1 ? 1 : v;
}

static float (*curve_tab)[1024];
static int curve_has[CURVE_COUNT];

static double curve_at (int c, double p)
{
  double f = p * 1023;
  int i = (int) f;
  if (i >= 1023)
    return curve_tab[c][1023];
  return curve_tab[c][i] + (curve_tab[c][i + 1] - curve_tab[c][i]) * (f - i);
}

void develop_prepare (develop *d, const recipe *r)
{
  double t = r->v[P_TEMP] / 100.0, n = r->v[P_TINT] / 100.0;
  double gain = pow (2.0, r->v[P_EV]);
  double mul[3], black = r->v[P_BLACK] / 100.0, white = r->v[P_WHITE] / 100.0;
  double gamma = pow (2.0, -r->v[P_MID] / 100.0);   /* +100: midtones a stop-ish brighter */
  double k = r->v[P_CONTRAST] / 100.0;
  int c, i;
  make_tables ();
  /* Warmer: more red, less blue. Tint: magenta (+) takes green down.
     Scaled so mid grey keeps its luminance. */
  mul[0] = 1 + 0.30 * t + 0.05 * n;
  mul[1] = 1 - 0.20 * n;
  mul[2] = 1 - 0.30 * t + 0.05 * n;
  {
    double y = 0.2126 * mul[0] + 0.7152 * mul[1] + 0.0722 * mul[2];
    for (c = 0; c < 3; c++)
      mul[c] /= y;
  }
  if (white - black < 0.05)
    white = black + 0.05;
  /* Curves, on the value as seen (after levels and contrast): RGB, then
     the channel's own. Tables of 1024 steps, read with interpolation. */
  {
    static float ct[CURVE_COUNT][1024];
    int has[CURVE_COUNT];
    for (c = 0; c < CURVE_COUNT; c++)
      if ((has[c] = !recipe_curve_is_identity (&r->curve[c])))
        recipe_curve_table (&r->curve[c], ct[c], 1024);
    curve_tab = ct;
    for (c = 0; c < CURVE_COUNT; c++)
      curve_has[c] = has[c];
  }
  for (c = 0; c < 3; c++)
    for (i = 0; i < DEV_LUT_SIZE; i++)
      {
        /* Each entry is sampled at the start of its range, so black (0)
           and white (the last entry) are exact. */
        double x = (double) i / (DEV_LUT_SIZE - 1), p;
        x *= mul[c] * gain;
        p = srgb_encode (clamp01 (x));
        p = clamp01 ((p - black) / (white - black));
        if (gamma != 1.0)
          p = pow (p, gamma);
        if (k != 0)
          {
            /* An S-curve about the middle: smoothstep, blended by k
               (negative k flattens towards a straight line and past it). */
            double s = p * p * (3 - 2 * p);
            p = clamp01 (p + k * (s - p));
          }
        if (curve_has[CURVE_RGB])
          p = curve_at (CURVE_RGB, p);
        if (curve_has[CURVE_RED + c])
          p = curve_at (CURVE_RED + c, p);
        d->tone[c][i] = (uint16_t) lround (srgb_decode (p) * 65535);
      }
  d->hs_hl = r->v[P_HIGHLIGHTS] / 100.0f;
  d->hs_sh = r->v[P_SHADOWS] / 100.0f;
  d->hs = d->hs_hl > 0 || d->hs_sh > 0;
  d->gw = d->gh = 1;
  d->gain[0] = 4096;                   /* no map yet: unchanged */
  d->fw = d->fh = 1;
  d->rx = d->ry = 0;
  d->sat = (int) lround (65536 * (1 + r->v[P_SATURATION] / 100.0));
  d->vib = (int) lround (65536 * (r->v[P_VIBRANCE] / 100.0));
  /* Split toning: a colour for the shadows and one for the highlights,
     each a hue at half saturation scaled to keep luminance, blended in by
     lightness (as seen) and by its strength. */
  d->toning = r->v[P_TONE_SH] > 0 || r->v[P_TONE_HI] > 0;
  if (d->toning)
    {
      double tc[2][3];
      int w;
      for (w = 0; w < 2; w++)
        {
          double h = (w ? r->v[P_TONE_HI_HUE] : r->v[P_TONE_SH_HUE]) / 60.0, f, rgb[3], y;
          int sec = (int) floor (h) % 6;
          f = h - floor (h);
          switch (sec)
            {
            case 0: rgb[0] = 1; rgb[1] = f; rgb[2] = 0; break;
            case 1: rgb[0] = 1 - f; rgb[1] = 1; rgb[2] = 0; break;
            case 2: rgb[0] = 0; rgb[1] = 1; rgb[2] = f; break;
            case 3: rgb[0] = 0; rgb[1] = 1 - f; rgb[2] = 1; break;
            case 4: rgb[0] = f; rgb[1] = 0; rgb[2] = 1; break;
            default: rgb[0] = 1; rgb[1] = 0; rgb[2] = 1 - f; break;
            }
          for (c = 0; c < 3; c++)
            rgb[c] = srgb_decode (0.5 + 0.5 * rgb[c]);   /* half saturation, in linear light */
          y = 0.2126 * rgb[0] + 0.7152 * rgb[1] + 0.0722 * rgb[2];
          for (c = 0; c < 3; c++)
            tc[w][c] = rgb[c] / y;
        }
      for (i = 0; i < 256; i++)
        {
          double l = srgb_encode ((i + 0.5) / 256.0);
          double ws = (1 - l) * (1 - l) * r->v[P_TONE_SH] / 100.0, wh = l * l * r->v[P_TONE_HI] / 100.0;
          for (c = 0; c < 3; c++)
            {
              double g = 1 + ws * (tc[0][c] - 1) + wh * (tc[1][c] - 1);
              d->tint[c][i] = (uint16_t) lround (g < 0 ? 0 : g > 15 ? 15 * 4096 : g * 4096);
            }
        }
    }
  /* Identity: every table entry maps its bin to its own middle. */
  {
    recipe look = *r;
    int p;
    for (p = P_SHARPEN; p < P_COUNT; p++)   /* sharpening and geometry are not develop's business */
      look.v[p] = recipe_params[p].def;
    d->identity = recipe_is_default (&look);
  }
}

/* One pixel: tone tables, then saturation and vibrancy around luminance.
   All 32-bit: the factor is in 1/256ths (at most 768) and a difference
   from luminance at most 65535, so the products stay under 2^26. No
   division: vibrancy weighs a colour by its channels' spread relative to
   the brightest, through a reciprocal table. */
static inline void pixel (const develop *d, const uint16_t *s, int *r, int *g, int *b)
{
  int R = d->tone[0][s[0] >> (16 - DEV_LUT_BITS)];
  int G = d->tone[1][s[1] >> (16 - DEV_LUT_BITS)];
  int B = d->tone[2][s[2] >> (16 - DEV_LUT_BITS)];
  if (d->sat != 65536 || d->vib != 0)
    {
      /* Rec. 709 luminance; unsigned: 65535 * 65536 only just fits 32 bits. */
      int Y = (int) (((unsigned) R * 13933u + (unsigned) G * 46871u + (unsigned) B * 4732u) >> 16);
      int f = d->sat >> 8;
      if (d->vib)
        {
          int mx = R > G ? (R > B ? R : B) : (G > B ? G : B);
          int mn = R < G ? (R < B ? R : B) : (G < B ? G : B);
          int spread = (((mx - mn) >> 8) * recip[mx >> 8]) >> 8;   /* 0-256: 256 is pure colour */
          if (spread > 256)
            spread = 256;
          f += ((d->vib >> 8) * (256 - spread)) >> 8;
        }
      R = Y + (((R - Y) * f) >> 8);
      G = Y + (((G - Y) * f) >> 8);
      B = Y + (((B - Y) * f) >> 8);
      R = R < 0 ? 0 : R > 65535 ? 65535 : R;
      G = G < 0 ? 0 : G > 65535 ? 65535 : G;
      B = B < 0 ? 0 : B > 65535 ? 65535 : B;
    }
  if (d->toning)
    {
      int Y = (int) (((unsigned) R * 13933u + (unsigned) G * 46871u + (unsigned) B * 4732u) >> 24);
      R = (int) (((unsigned) R * d->tint[0][Y]) >> 12);
      G = (int) (((unsigned) G * d->tint[1][Y]) >> 12);
      B = (int) (((unsigned) B * d->tint[2][Y]) >> 12);
      R = R > 65535 ? 65535 : R;
      G = G > 65535 ? 65535 : G;
      B = B > 65535 ? 65535 : B;
    }
  *r = R; *g = G; *b = B;
}

/* --- Highlights & Shadows */

int develop_needs_mask (const develop *d) { return d->hs; }

/* The gain for a lightness l (0-1, as seen): at 100, shadows lift black
   surroundings by 2.4 stops, fading out by l = 0.65; highlights pull white
   ones down by 1.6 stops, fading out by l = 0.35. */
static double hs_gain (const develop *d, double l)
{
  double ws = l < 0.65 ? (0.65 - l) / 0.65 : 0, wh = l > 0.35 ? (l - 0.35) / 0.65 : 0;
  return pow (2.0, d->hs_sh * 2.4 * ws - d->hs_hl * 1.6 * wh);
}

/* Blurs the map in place: three passes of a 5-cell box each way, which is
   close to a Gaussian, edges held. */
static void blur_map (dev_lummap *m)
{
  float tmp[HS_MAP];
  int pass, x, y, k;
  for (pass = 0; pass < 3; pass++)
    {
      for (y = 0; y < m->h; y++)
        {
          float *row = m->l + y * m->w;
          for (x = 0; x < m->w; x++)
            {
              float sum = 0;
              for (k = -2; k <= 2; k++)
                {
                  int xx = x + k < 0 ? 0 : x + k >= m->w ? m->w - 1 : x + k;
                  sum += row[xx];
                }
              tmp[x] = sum / 5;
            }
          memcpy (row, tmp, sizeof (float) * (size_t) m->w);
        }
      for (x = 0; x < m->w; x++)
        {
          for (y = 0; y < m->h; y++)
            {
              float sum = 0;
              for (k = -2; k <= 2; k++)
                {
                  int yy = y + k < 0 ? 0 : y + k >= m->h ? m->h - 1 : y + k;
                  sum += m->l[yy * m->w + x];
                }
              tmp[y] = sum / 5;
            }
          for (y = 0; y < m->h; y++)
            m->l[y * m->w + x] = tmp[y];
        }
    }
}

/* Cells of the map over a w x h picture: the longer side HS_MAP. */
static void map_size (dev_lummap *m, int w, int h)
{
  if (w >= h)
    {
      m->w = HS_MAP;
      m->h = (int) ((long long) HS_MAP * h / (w > 0 ? w : 1));
    }
  else
    {
      m->h = HS_MAP;
      m->w = (int) ((long long) HS_MAP * w / (h > 0 ? h : 1));
    }
  if (m->w < 1) m->w = 1;
  if (m->h < 1) m->h = 1;
}

/* Lightness (Rec. 709 luminance, sRGB-encoded) averaged over each cell. */
static void make_map (dev_lummap *m, const unsigned char *rgb, const uint16_t *lin, int w, int h)
{
  int cx, cy;
  make_tables ();
  map_size (m, w, h);
  for (cy = 0; cy < m->h; cy++)
    {
      int y0 = (int) ((long long) cy * h / m->h), y1 = (int) ((long long) (cy + 1) * h / m->h);
      for (cx = 0; cx < m->w; cx++)
        {
          int x0 = (int) ((long long) cx * w / m->w), x1 = (int) ((long long) (cx + 1) * w / m->w);
          int x, y, step = (x1 - x0) * (y1 - y0) > 4096 ? 2 : 1;   /* big cells: every other pixel */
          double sum = 0;
          long n = 0;
          if (y1 <= y0) y1 = y0 + 1;
          if (x1 <= x0) x1 = x0 + 1;
          for (y = y0; y < y1 && y < h; y += step)
            for (x = x0; x < x1 && x < w; x += step)
              {
                size_t i = ((size_t) y * w + x) * 3;
                unsigned R, G, B;
                if (rgb)
                  {
                    R = s2l[rgb[i]]; G = s2l[rgb[i + 1]]; B = s2l[rgb[i + 2]];
                  }
                else
                  {
                    R = lin[i]; G = lin[i + 1]; B = lin[i + 2];
                  }
                sum += (R * 0.2126 + G * 0.7152 + B * 0.0722) / 65535.0;
                n++;
              }
          m->l[cy * m->w + cx] = (float) srgb_encode (n ? sum / n : 0);
        }
    }
  blur_map (m);
}

void dev_lummap_rgb8 (dev_lummap *m, const unsigned char *rgb, int w, int h) { make_map (m, rgb, NULL, w, h); }
void dev_lummap_lin (dev_lummap *m, const uint16_t *lin, int w, int h) { make_map (m, NULL, lin, w, h); }

/* The map's lightness at (u, v), fractions of the source: bilinear. */
static double map_at (const dev_lummap *m, double u, double v)
{
  double fx = u * m->w - 0.5, fy = v * m->h - 0.5, ax, ay;
  int x0, y0, x1, y1;
  if (fx < 0) fx = 0;
  if (fy < 0) fy = 0;
  if (fx > m->w - 1) fx = m->w - 1;
  if (fy > m->h - 1) fy = m->h - 1;
  x0 = (int) fx; y0 = (int) fy;
  x1 = x0 + 1 < m->w ? x0 + 1 : x0;
  y1 = y0 + 1 < m->h ? y0 + 1 : y0;
  ax = fx - x0; ay = fy - y0;
  return (m->l[y0 * m->w + x0] * (1 - ax) + m->l[y0 * m->w + x1] * ax) * (1 - ay)
       + (m->l[y1 * m->w + x0] * (1 - ax) + m->l[y1 * m->w + x1] * ax) * ay;
}

void develop_mask (develop *d, const dev_lummap *m, const geom_plan *g, int src_w, int src_h)
{
  int ow = g ? g->ow : src_w, oh = g ? g->oh : src_h, x, y;
  if (ow < 1 || oh < 1 || src_w < 1 || src_h < 1)
    return;
  if (ow >= oh)
    {
      d->gw = HS_GRID;
      d->gh = (int) ((long long) HS_GRID * oh / ow);
    }
  else
    {
      d->gh = HS_GRID;
      d->gw = (int) ((long long) HS_GRID * ow / oh);
    }
  if (d->gw < 2) d->gw = 2;
  if (d->gh < 2) d->gh = 2;
  for (y = 0; y < d->gh; y++)
    for (x = 0; x < d->gw; x++)
      {
        /* The cell's centre in the output, back to the source. */
        double ox = (x + 0.5) * ow / d->gw, oy = (y + 0.5) * oh / d->gh, sx = ox, sy = oy;
        double gn;
        if (g)
          geom_to_source (g, ox, oy, &sx, &sy);
        gn = hs_gain (d, map_at (m, sx / src_w, sy / src_h));
        d->gain[y * d->gw + x] = (uint16_t) lround (gn * 4096 > 65535 ? 65535 : gn * 4096);
      }
  d->fw = ow;
  d->fh = oh;
  d->rx = d->ry = 0;
}

void develop_region (develop *d, int fw, int fh, int x0, int y0)
{
  d->fw = fw > 0 ? fw : 1;
  d->fh = fh > 0 ? fh : 1;
  d->rx = x0;
  d->ry = y0;
}

/* The gains along one row of the output: grid rows interpolated. */
static void gain_row (const develop *d, int y, float *row)
{
  double fy = (y + 0.5) * d->gh / d->fh - 0.5, ay;
  int y0, y1, x;
  if (fy < 0) fy = 0;
  if (fy > d->gh - 1) fy = d->gh - 1;
  y0 = (int) fy;
  y1 = y0 + 1 < d->gh ? y0 + 1 : y0;
  ay = fy - y0;
  for (x = 0; x < d->gw; x++)
    row[x] = (float) (d->gain[y0 * d->gw + x] * (1 - ay) + d->gain[y1 * d->gw + x] * ay);
}

void develop_rows (const develop *d, const uint16_t *src, int w, int y0, int y1,
                   uint32_t *dst, dev_hist *hist)
{
  int y, x;
  make_tables ();
  for (y = y0; y < y1; y++)
    {
      const uint16_t *s = src + (size_t) y * w * 3;
      uint32_t *o = dst + (size_t) y * w;
      if (d->hs)
        {
          /* Highlights & Shadows: each pixel scaled by its place's gain,
             then the rest as usual. */
          float row[HS_GRID];
          double step = (double) d->gw / d->fw, fx = (d->rx + 0.5) * step - 0.5;
          gain_row (d, y + d->ry, row);
          for (x = 0; x < w; x++, s += 3, fx += step)
            {
              double f = fx < 0 ? 0 : fx > d->gw - 1 ? d->gw - 1 : fx;
              int i = (int) f, j = i + 1 < d->gw ? i + 1 : i;
              unsigned gn = (unsigned) (row[i] + (row[j] - row[i]) * (f - i));
              uint16_t t[3];
              int R, G, B, c;
              unsigned r, g, b;
              for (c = 0; c < 3; c++)
                {
                  unsigned v = (s[c] * gn) >> 12;
                  t[c] = (uint16_t) (v > 65535 ? 65535 : v);
                }
              pixel (d, t, &R, &G, &B);
              r = l2s[R]; g = l2s[G]; b = l2s[B];
              o[x] = r | g << 8 | b << 16;
              if (hist)
                {
                  hist->c[HIST_R][r]++;
                  hist->c[HIST_G][g]++;
                  hist->c[HIST_B][b]++;
                  hist->c[HIST_LUM][(r * 54 + g * 183 + b * 19) >> 8]++;
                }
            }
        }
      else if (d->identity)
        for (x = 0; x < w; x++, s += 3)
          {
            unsigned r = l2s[s[0]], g = l2s[s[1]], b = l2s[s[2]];
            o[x] = r | g << 8 | b << 16;
            if (hist)
              {
                hist->c[HIST_R][r]++;
                hist->c[HIST_G][g]++;
                hist->c[HIST_B][b]++;
                hist->c[HIST_LUM][(r * 54 + g * 183 + b * 19) >> 8]++;
              }
          }
      else
        for (x = 0; x < w; x++, s += 3)
          {
            int R, G, B;
            unsigned r, g, b;
            pixel (d, s, &R, &G, &B);
            r = l2s[R]; g = l2s[G]; b = l2s[B];
            o[x] = r | g << 8 | b << 16;
            if (hist)
              {
                hist->c[HIST_R][r]++;
                hist->c[HIST_G][g]++;
                hist->c[HIST_B][b]++;
                hist->c[HIST_LUM][(r * 54 + g * 183 + b * 19) >> 8]++;
              }
          }
    }
}

int develop_image (const recipe *r, rgb_image *img)
{
  develop *d = malloc (sizeof *d);
  geom_plan g;
  uint16_t *lin = NULL, *out = NULL;
  uint32_t *px = NULL;
  unsigned char *rgb = NULL;
  int geometry = !recipe_geometry_is_default (r), ow = img->w, oh = img->h;
  size_t i, n;
  if (!d)
    return -1;
  develop_prepare (d, r);
  if (!geometry && !d->hs)
    {
      develop_rgb8 (d, img);
      free (d);
      return 0;
    }
  if (geometry)
    {
      geom_plan_make (&g, r, img->w, img->h, 1, 0, 0);
      ow = g.ow;
      oh = g.oh;
    }
  n = (size_t) ow * oh;
  lin = malloc ((size_t) img->w * img->h * 6);
  out = geometry ? malloc (n * 6) : lin;
  px = malloc (n * 4);
  rgb = malloc (n * 3);
  if (!lin || !out || !px || !rgb)
    {
      free (d); free (lin); if (geometry) free (out); free (px); free (rgb);
      return -1;
    }
  dev_to_linear (img->rgb, lin, (size_t) img->w * img->h);
  if (d->hs)
    {
      dev_lummap *m = malloc (sizeof *m);
      if (m)
        {
          dev_lummap_lin (m, lin, img->w, img->h);
          develop_mask (d, m, geometry ? &g : NULL, img->w, img->h);
          free (m);
        }
    }
  if (geometry)
    geom_rows (&g, lin, out, 0, oh);
  develop_rows (d, out, ow, 0, oh, px, NULL);
  for (i = 0; i < n; i++)
    {
      rgb[3 * i] = (unsigned char) (px[i] & 255);
      rgb[3 * i + 1] = (unsigned char) (px[i] >> 8 & 255);
      rgb[3 * i + 2] = (unsigned char) (px[i] >> 16 & 255);
    }
  free (img->rgb);
  img->rgb = rgb;
  img->w = ow;
  img->h = oh;
  free (lin); if (geometry) free (out); free (px); free (d);
  return 0;
}

void develop_rgb8 (const develop *d, rgb_image *img)
{
  size_t i, n = (size_t) img->w * img->h;
  make_tables ();
  if (d->identity)
    return;
  for (i = 0; i < n; i++)
    {
      unsigned char *p = img->rgb + 3 * i;
      uint16_t s[3];
      int R, G, B;
      s[0] = s2l[p[0]]; s[1] = s2l[p[1]]; s[2] = s2l[p[2]];
      pixel (d, s, &R, &G, &B);
      p[0] = l2s[R]; p[1] = l2s[G]; p[2] = l2s[B];
    }
}
