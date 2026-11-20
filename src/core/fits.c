/* Copyright (c) 2026 Andrew Youll
   Released under the MIT licence: see the LICENSE file.
   SPDX-License-Identifier: MIT */
/* fits.c: see fits.h. Byte-at-a-time conversion of the big-endian data
   (no unaligned loads); every size checked against the file. */
#include <ctype.h>
#include <math.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include "fits.h"

#define BLOCK 2880
#define CARD 80
#define MAX_HEADER_BLOCKS 2000         /* 5.7 MB of header: more is not a picture */
#define MAX_HDUS 16
#define STAT_ROWS 256                  /* rows (pairs, for a mosaic) sampled for the stretch */
#define STAT_COLS 512
#define BRIGHT_MEDIAN 0.2              /* a median above this: shown as it is */
#define TARGET_BG 0.25                 /* the auto-stretch puts the sky here */
#define SHADOWS_MADN (-2.8)            /* and clips this many (normalised) MADs below it */

/* --- the header --- */

int fits_is (const unsigned char *buf, size_t len)
{
  return len >= 30 && memcmp (buf, "SIMPLE  =", 9) == 0 && buf[29] == 'T';
}

int fits_has_extension (const char *name)
{
  static const char *const ext[] = { "fits", "fit", "fts" };
  const char *end, *dot = NULL, *q;
  size_t n, k;
  if (!name)
    return 0;
  end = name + strlen (name);
  if (end - name > 4 && end[-4] == ',' && isxdigit ((unsigned char) end[-3]) && isxdigit ((unsigned char) end[-2])
      && isxdigit ((unsigned char) end[-1]))
    end -= 4;                          /* ",xxx": a filetype, as the PC side of IXFS writes it */
  for (q = name; q < end; q++)
    if (*q == '/' || *q == '.')
      dot = q;
  if (!dot)
    return 0;
  n = (size_t) (end - dot - 1);
  for (k = 0; k < sizeof ext / sizeof ext[0]; k++)
    if (n == strlen (ext[k]))
      {
        size_t i;
        for (i = 0; i < n && tolower ((unsigned char) dot[1 + i]) == ext[k][i]; i++)
          ;
        if (i == n)
          return 1;
      }
  return 0;
}

/* One card's keyword (trimmed) and value: a string (quotes removed, ''
   made '), or the text of a number or logical. 1 if it has a value. */
static int card (const char *c, char *key, char *val, size_t vn, int *is_string)
{
  int i, j = 0;
  for (i = 0; i < 8 && c[i] != ' '; i++)
    key[i] = c[i];
  key[i] = '\0';
  val[0] = '\0';
  *is_string = 0;
  if (c[8] != '=' || c[9] != ' ')
    return 0;
  for (i = 10; i < CARD && c[i] == ' '; i++)
    ;
  if (i < CARD && c[i] == '\'')
    {
      *is_string = 1;
      for (i++; i < CARD; i++)
        {
          if (c[i] == '\'')
            {
              if (i + 1 < CARD && c[i + 1] == '\'')
                i++;
              else
                break;
            }
          if ((size_t) j + 1 < vn)
            val[j++] = c[i];
        }
      while (j > 0 && val[j - 1] == ' ')
        j--;
    }
  else
    for (; i < CARD && c[i] != '/' && c[i] != ' '; i++)
      if ((size_t) j + 1 < vn)
        val[j++] = c[i] == 'D' || c[i] == 'd' ? 'E' : c[i];
  val[j] = '\0';
  return 1;
}

/* "+51 30 15.5", "51:30:15.5", "-3.25": degrees, in 1e-7. 0 if unreadable. */
static int angle (const char *s, int *out)
{
  double part[3] = { 0, 0, 0 }, v;
  int n = 0, neg = 0;
  char *e;
  while (*s == ' ')
    s++;
  if (*s == '-' || *s == '+')
    neg = *s++ == '-';
  while (n < 3 && *s)
    {
      part[n] = strtod (s, &e);
      if (e == s)
        break;
      n++;
      s = e;
      while (*s == ' ' || *s == ':')
        s++;
    }
  if (n == 0)
    return 0;
  v = part[0] + part[1] / 60 + part[2] / 3600;
  if (v > 360)
    return 0;
  *out = (int) lround ((neg ? -v : v) * 1e7);
  return 1;
}

static void copy (char *dst, size_t n, const char *src)
{
  snprintf (dst, n, "%s", src);
}

int fits_read_header (FILE *f, fits_hdr *h, char *err, size_t en)
{
  unsigned char block[BLOCK];
  char key[9], val[72];
  long pos = 0, fsize;
  int hdu, s;
  if (fseek (f, 0, SEEK_END) != 0 || (fsize = ftell (f)) < BLOCK)
    {
      snprintf (err, en, "This FITS file is too short");
      return -1;
    }
  for (hdu = 0; hdu < MAX_HDUS; hdu++)
    {
      int naxis = 0, nax[4] = { 0, 0, 0, 0 }, bitpix = 0, simple = 0, ended = 0, nblocks = 0, zimage = 0;
      long pcount = 0, gcount = 1;
      char xtension[24] = "";
      double lat = 0, lon = 0;
      int has_lat = 0, has_lon = 0;
      memset (h, 0, sizeof *h);
      h->bscale = 1;
      h->bottom_up = 1;
      h->hdu = hdu;
      while (!ended)
        {
          int k;
          if (nblocks++ >= MAX_HEADER_BLOCKS || fseek (f, pos, SEEK_SET) != 0 || fread (block, 1, BLOCK, f) != BLOCK)
            {
              snprintf (err, en, hdu ? "No picture in this FITS file" : "This FITS file's header is cut short");
              return -1;
            }
          pos += BLOCK;
          for (k = 0; k < BLOCK / CARD && !ended; k++)
            {
              const char *c = (const char *) block + k * CARD;
              int has = card (c, key, val, sizeof val, &s);
              if (!strcmp (key, "END"))
                ended = 1;
              if (!has)
                continue;
              if (!strcmp (key, "SIMPLE")) simple = val[0] == 'T';
              else if (!strcmp (key, "XTENSION")) copy (xtension, sizeof xtension, val);
              else if (!strcmp (key, "BITPIX")) bitpix = atoi (val);
              else if (!strcmp (key, "NAXIS")) naxis = atoi (val);
              else if (!strncmp (key, "NAXIS", 5) && key[5] >= '1' && key[5] <= '4' && !key[6]) nax[key[5] - '1'] = atoi (val);
              else if (!strcmp (key, "PCOUNT")) pcount = atol (val);
              else if (!strcmp (key, "GCOUNT")) gcount = atol (val);
              else if (!strcmp (key, "ZIMAGE")) zimage = val[0] == 'T';
              else if (!strcmp (key, "BZERO")) h->bzero = atof (val);
              else if (!strcmp (key, "BSCALE")) h->bscale = atof (val);
              else if (!strcmp (key, "ROWORDER")) h->bottom_up = strncmp (val, "TOP-DOWN", 8) != 0;
              else if (!strcmp (key, "BAYERPAT") || !strcmp (key, "COLORTYP"))
                {
                  if (strlen (val) == 4 && strspn (val, "RGB") == 4)
                    copy (h->bayer, sizeof h->bayer, val);
                }
              else if (!strcmp (key, "XBAYROFF")) h->xoff = atoi (val) & 1;
              else if (!strcmp (key, "YBAYROFF")) h->yoff = atoi (val) & 1;
              else if (!strcmp (key, "DATE-OBS")) copy (h->date_obs, sizeof h->date_obs, val);
              else if (!strcmp (key, "OBJECT")) copy (h->object, sizeof h->object, val);
              else if (!strcmp (key, "TELESCOP")) copy (h->telescop, sizeof h->telescop, val);
              else if (!strcmp (key, "INSTRUME")) copy (h->instrume, sizeof h->instrume, val);
              else if (!strcmp (key, "FILTER")) copy (h->filter, sizeof h->filter, val);
              else if (!strcmp (key, "IMAGETYP") || !strcmp (key, "FRAME")) copy (h->imagetyp, sizeof h->imagetyp, val);
              else if (!strcmp (key, "CREATOR") || !strcmp (key, "PROGRAM") || !strcmp (key, "SWCREATE"))
                {
                  if (!h->creator[0])
                    copy (h->creator, sizeof h->creator, val);
                }
              else if (!strcmp (key, "EXPTIME") || !strcmp (key, "EXPOSURE")) h->exptime = atof (val);
              else if (!strcmp (key, "FOCALLEN")) h->focal = atof (val);
              else if (!strcmp (key, "ISOSPEED") || !strcmp (key, "ISO")) h->iso = atoi (val);
              else if (!strcmp (key, "GAIN")) h->gain = atof (val), h->has_gain = 1;
              else if (!strcmp (key, "CCD-TEMP") || !strcmp (key, "CCD_TEMP")) h->ccdtemp = atof (val), h->has_temp = 1;
              else if (!strcmp (key, "STACKCNT") || !strcmp (key, "NCOMBINE")) h->stacked = atoi (val);
              else if (!strcmp (key, "DATAMIN")) h->datamin = atof (val), h->has_range |= 1;
              else if (!strcmp (key, "DATAMAX")) h->datamax = atof (val), h->has_range |= 2;
              else if (!strcmp (key, "SITELAT") || !strcmp (key, "OBSGEO-B") || !strcmp (key, "LAT-OBS"))
                {
                  int a;
                  if (!has_lat && angle (val, &a))
                    lat = a, has_lat = 1;
                }
              else if (!strcmp (key, "SITELONG") || !strcmp (key, "OBSGEO-L") || !strcmp (key, "LONG-OBS"))
                {
                  int a;
                  if (!has_lon && angle (val, &a))
                    lon = a, has_lon = 1;
                }
            }
        }
      if (hdu == 0 && !simple)
        {
          snprintf (err, en, "Not a FITS file");
          return -1;
        }
      h->bitpix = bitpix;
      h->data = pos;
      if (bitpix != 8 && bitpix != 16 && bitpix != 32 && bitpix != -32 && bitpix != -64)
        {
          snprintf (err, en, "This FITS file's data type (BITPIX %d) isn't one Lumen reads", bitpix);
          return -1;
        }
      if (hdu > 0 && !strcmp (xtension, "BINTABLE") && zimage)
        {
          snprintf (err, en, "Lumen can't read compressed FITS files (fpack): unpack it with funpack first");
          return -1;
        }
      if ((hdu == 0 || !strcmp (xtension, "IMAGE")) && naxis >= 2 && nax[0] > 0 && nax[1] > 0)
        {
          long long bytes;
          if (nax[0] > 65535 || nax[1] > 65535)
            {
              snprintf (err, en, "This FITS picture is too big (%d x %d)", nax[0], nax[1]);
              return -1;
            }
          h->w = nax[0];
          h->h = nax[1];
          h->planes = naxis >= 3 && nax[2] == 3 ? 3 : 1;
          if (h->planes == 3 || h->w < 2 || h->h < 2)
            h->bayer[0] = '\0';
          bytes = (long long) h->w * h->h * h->planes * (bitpix < 0 ? -bitpix : bitpix) / 8;
          if (h->data + bytes > fsize)
            {
              snprintf (err, en, "This FITS file is cut short");
              return -1;
            }
          if (has_lat && has_lon && (lat != 0 || lon != 0) && fabs (lat) <= 90e7 && fabs (lon) <= 180e7)
            {
              h->geo = GEO_POS;
              h->lat = (int) lat;
              h->lon = (int) lon;
            }
          return 0;
        }
      /* No picture here: the next HDU, past this one's data. */
      {
        long long n = 0;
        int k;
        if (naxis > 0)
          {
            n = 1;
            for (k = 0; k < naxis && k < 4; k++)
              n *= nax[k] > 0 ? nax[k] : 0;
          }
        n = (n + pcount) * gcount * (bitpix < 0 ? -bitpix : bitpix) / 8;
        n = (n + BLOCK - 1) / BLOCK * BLOCK;
        if (n < 0 || pos + n >= fsize)
          break;
        pos += (long) n;
      }
    }
  snprintf (err, en, "No picture in this FITS file");
  return -1;
}

void fits_encoding (const fits_hdr *h, char *out, size_t n)
{
  const char *type = h->bitpix == 8 ? "8-bit integers" : h->bitpix == 16 ? "16-bit integers"
                   : h->bitpix == 32 ? "32-bit integers" : h->bitpix == -32 ? "32-bit floating point"
                   : "64-bit floating point";
  if (h->planes == 3)
    snprintf (out, n, "FITS, %s, RGB", type);
  else if (h->bayer[0])
    snprintf (out, n, "FITS, %s, colour mosaic (%s)", type, h->bayer);
  else
    snprintf (out, n, "FITS, %s, mono", type);
}

void fits_to_info (const fits_hdr *h, img_info *in)
{
  int Y, M, D, hh = 0, mm = 0, ss = 0;
  memset (in, 0, sizeof *in);
  in->format = IMG_FITS;
  in->width = h->w;
  in->height = h->h;
  in->orientation = 1;
  if (sscanf (h->date_obs, "%4d-%2d-%2dT%2d:%2d:%2d", &Y, &M, &D, &hh, &mm, &ss) >= 3)
    snprintf (in->taken, sizeof in->taken, "%04d:%02d:%02d %02d:%02d:%02d", Y, M, D, hh, mm, ss);
  snprintf (in->model, sizeof in->model, "%.*s", (int) sizeof in->model - 1, h->instrume);
  in->exposure = h->exptime;
  in->iso = h->iso;
  in->focal = h->focal;
  fits_encoding (h, in->encoding, sizeof in->encoding);
  in->geo = h->geo;
  in->lat = h->lat;
  in->lon = h->lon;
}

/* --- the stretch --- */

double fits_mtf (double m, double x)
{
  if (x <= 0)
    return 0;
  if (x >= 1)
    return 1;
  if (m == 0.5)
    return x;
  return (m - 1) * x / ((2 * m - 1) * x - m);
}

int fits_stretch_params (double med, double mad, double *c0, double *m)
{
  double madn = 1.4826 * mad;
  *c0 = 0;
  *m = 0.5;
  if (med > BRIGHT_MEDIAN || madn <= 0)
    return -1;
  *c0 = med + SHADOWS_MADN * madn;
  if (*c0 < 0)
    *c0 = 0;
  /* The midtones balance that puts the median at TARGET_BG. */
  *m = fits_mtf (TARGET_BG, med - *c0);
  return 0;
}

/* --- reading --- */

struct fits_reader
{
  FILE *f;
  fits_hdr h;
  int bpp;                    /* bytes a sample */
  int half, ow, oh;           /* what is read */
  int stretched;
  double lo, inv;             /* physical value v: (v - lo) * inv is 0-1 */
  unsigned char *raw;         /* one file row */
  uint16_t *cache[3];         /* normalised display rows (a mosaic's, and RGB's planes) */
  int cached[3];              /* the display row in each, or -1 */
  uint16_t *planes;           /* an RGB row's three planes, normalised */
  uint16_t *row3;             /* a row read, three values a pixel */
  unsigned char lut[3][65536];
  int cfa[2][2];              /* the mosaic's colours (0 R, 1 G, 2 B) at the parities of the row and column */
  /* For full-size demosaicking: the neighbours averaged for each parity and channel. */
  signed char nb[2][2][3][4][2];
  int nn[2][2][3];
};

static double sample (const fits_reader *r, const unsigned char *p)
{
  switch (r->h.bitpix)
    {
    case 8:
      return p[0];
    case 16:
      return (int16_t) (uint16_t) ((p[0] << 8) | p[1]);
    case 32:
      return (int32_t) ((uint32_t) p[0] << 24 | (uint32_t) p[1] << 16 | (uint32_t) p[2] << 8 | p[3]);
    case -32:
      {
        uint32_t u = (uint32_t) p[0] << 24 | (uint32_t) p[1] << 16 | (uint32_t) p[2] << 8 | p[3];
        float v;
        memcpy (&v, &u, 4);
        return v;
      }
    default:
      {
        uint64_t u = 0;
        double v;
        int k;
        for (k = 0; k < 8; k++)
          u = u << 8 | p[k];
        memcpy (&v, &u, 8);
        return v;
      }
    }
}

/* File row fr of plane p into r->raw. */
static int read_raw (fits_reader *r, int plane, int fr)
{
  size_t len = (size_t) r->h.w * r->bpp;
  long off = r->h.data + (long) (((long long) plane * r->h.h + fr) * (long long) len);
  if (fseek (r->f, off, SEEK_SET) != 0 || fread (r->raw, 1, len, r->f) != len)
    return -1;
  return 0;
}

static int file_row (const fits_reader *r, int y)
{
  return r->h.bottom_up ? r->h.h - 1 - y : y;
}

/* Display row y of plane p, normalised to 0-65535. */
static int read_norm (fits_reader *r, int plane, int y, uint16_t *out)
{
  int x, w = r->h.w;
  const unsigned char *p = r->raw;
  if (read_raw (r, plane, file_row (r, y)))
    return -1;
  if (r->h.bitpix == 16 && r->h.bscale == 1 && r->h.bzero == 32768 && r->lo == 0 && r->inv * 65535 > 0.99999
      && r->inv * 65535 < 1.00001)
    {
      for (x = 0; x < w; x++, p += 2)
        out[x] = (uint16_t) (((p[0] << 8) | p[1]) ^ 0x8000);
      return 0;
    }
  for (x = 0; x < w; x++, p += r->bpp)
    {
      double v = (r->h.bzero + r->h.bscale * sample (r, p) - r->lo) * r->inv;
      out[x] = v != v || v <= 0 ? 0 : v >= 1 ? 65535 : (uint16_t) (v * 65535 + 0.5);
    }
  return 0;
}

/* Display row y (mirrored at the edges, keeping the mosaic's parity) of
   plane 0, from the cache. A row read replaces the one furthest from
   centre, so the rows within one of it stay. */
static const uint16_t *row_of (fits_reader *r, int y, int centre)
{
  int k, far = 0, farthest = -1;
  if (y < 0)
    y = -y;
  if (y >= r->h.h)
    y = 2 * r->h.h - 2 - y;
  if (y < 0)
    y = 0;
  for (k = 0; k < 3; k++)
    if (r->cached[k] == y)
      return r->cache[k];
  for (k = 0; k < 3; k++)
    {
      int d = r->cached[k] < 0 ? 1 << 30 : abs (r->cached[k] - centre);
      if (d > farthest)
        farthest = d, far = k;
    }
  r->cached[far] = -1;
  if (read_norm (r, 0, y, r->cache[far]))
    return NULL;
  r->cached[far] = y;
  return r->cache[far];
}

static int cmp_d (const void *a, const void *b)
{
  double x = *(const double *) a, y = *(const double *) b;
  return x < y ? -1 : x > y;
}

/* The median and MAD of a 0-65535 histogram, as 0-1. */
static void hist_stats (const uint32_t *hist, uint32_t total, double *med, double *mad)
{
  static uint32_t dev[65536];
  uint32_t acc = 0;
  int i, m = 0;
  for (i = 0; i < 65536; i++)
    if ((acc += hist[i]) * 2 >= total)
      {
        m = i;
        break;
      }
  memset (dev, 0, sizeof dev);
  for (i = 0; i < 65536; i++)
    dev[abs (i - m)] += hist[i];
  acc = 0;
  for (i = 0; i < 65536; i++)
    if ((acc += dev[i]) * 2 >= total)
      break;
  *med = m / 65535.0;
  *mad = (i < 65536 ? i : 0) / 65535.0;
}

/* Samples the picture: its range (for floating point and 32-bit data),
   then each channel's median and MAD, and the stretch made from them. */
static int measure (fits_reader *r, char *err, size_t en)
{
  const fits_hdr *h = &r->h;
  int nch = h->planes == 3 || h->bayer[0] ? 3 : 1;
  int step_y = h->h > STAT_ROWS * 2 ? h->h / STAT_ROWS : 2, step_x = h->w > STAT_COLS * 2 ? h->w / STAT_COLS : 2;
  size_t cap = ((size_t) h->h / step_y + 2) * ((size_t) h->w / step_x + 2) * 4 * (h->planes == 3 ? 3 : 1);
  double *vals = malloc (cap * sizeof *vals);
  unsigned char *chan = malloc (cap);
  uint32_t *hist = calloc ((size_t) nch * 65536, sizeof *hist), tot[3] = { 0, 0, 0 };
  size_t n = 0, i;
  double mn = HUGE_VAL, mx = -HUGE_VAL, med[3], mad[3], c0[3], m[3];
  int y, x, p, c, stretch = 1;
  if (!vals || !chan || !hist)
    {
      free (vals); free (chan); free (hist);
      snprintf (err, en, "Not enough memory to read this FITS picture");
      return -1;
    }
  for (p = 0; p < h->planes; p++)
    for (y = 0; y + 1 < h->h || (y == 0 && h->h == 1); y += step_y)
      {
        int dy;
        for (dy = 0; dy < (h->bayer[0] ? 2 : 1); dy++)
          {
            int yy = (y & ~1) + dy;
            if (yy >= h->h || read_raw (r, p, file_row (r, yy)))
              continue;
            for (x = 0; x < h->w && n + 2 <= cap; x += step_x)
              {
                int dx;
                for (dx = 0; dx < (h->bayer[0] ? 2 : 1); dx++)
                  {
                    int xx = (x & ~1) + dx;
                    double v;
                    if (xx >= h->w)
                      continue;
                    v = h->bzero + h->bscale * sample (r, r->raw + (size_t) xx * r->bpp);
                    if (v != v)
                      continue;
                    vals[n] = v;
                    chan[n++] = (unsigned char) (h->planes == 3 ? p : h->bayer[0] ? r->cfa[yy & 1][xx & 1] : 0);
                    if (v < mn) mn = v;
                    if (v > mx) mx = v;
                  }
              }
          }
      }
  if (!n)
    {
      free (vals); free (chan); free (hist);
      snprintf (err, en, "This FITS picture's data can't be read");
      return -1;
    }
  /* The range: an integer type's own (from 0 when nothing is below it);
     floating point's 0-1 if that's where it is, else what's there. */
  if (h->bitpix == 8 || h->bitpix == 16)
    {
      double tlo = h->bitpix == 8 ? 0 : -32768, thi = h->bitpix == 8 ? 255 : 32767;
      double plo = h->bzero + h->bscale * tlo, phi = h->bzero + h->bscale * thi;
      if (plo > phi) { double t = plo; plo = phi; phi = t; }
      r->lo = mn < 0 ? plo : 0;
      if (r->lo < plo)
        r->lo = plo;
      r->inv = phi > r->lo ? 1 / (phi - r->lo) : 1;
    }
  else
    {
      double lo, hi;
      if (h->has_range == 3 && h->datamax > h->datamin)
        lo = h->datamin, hi = h->datamax;
      else if (h->bitpix < 0 && mn >= -1e-3 && mx <= 1 + 1e-3)
        lo = 0, hi = 1;
      else
        {
          /* The 99.99th percentile, not the brightest pixel (a hot one). */
          qsort (vals, n, sizeof *vals, cmp_d);
          lo = mn < 0 ? mn : 0;
          hi = vals[(size_t) ((n - 1) * 0.9999)];
          if (hi <= lo)
            hi = mx > lo ? mx : lo + 1;
        }
      r->lo = lo;
      r->inv = 1 / (hi - lo);
    }
  for (i = 0; i < n; i++)
    {
      double v = (vals[i] - r->lo) * r->inv;
      int k = v <= 0 ? 0 : v >= 1 ? 65535 : (int) (v * 65535 + 0.5);
      hist[(size_t) chan[i] * 65536 + (size_t) k]++;
      tot[chan[i]]++;
    }
  for (c = 0; c < nch; c++)
    hist_stats (hist + (size_t) c * 65536, tot[c] ? tot[c] : 1, &med[c], &mad[c]);
  free (vals);
  free (chan);
  free (hist);
  /* Each colour stretched on its own, so the sky comes out grey: a
     camera's colours (a DSLR's are mostly green) or a stack's not yet
     colour-calibrated look natural. */
  for (c = 0; c < nch; c++)
    if (fits_stretch_params (med[c], mad[c], &c0[c], &m[c]))
      stretch = 0;
  r->stretched = stretch;
  for (c = 0; c < 3; c++)
    {
      int cc = c < nch ? c : 0, k;
      for (k = 0; k < 65536; k++)
        {
          double v = k / 65535.0;
          if (stretch)
            v = v <= c0[cc] ? 0 : fits_mtf (m[cc], (v - c0[cc]) / (1 - c0[cc]));
          r->lut[c][k] = (unsigned char) (v * 255 + 0.5);
        }
    }
  return 0;
}

fits_reader *fits_reader_open (FILE *f, const fits_hdr *h, int half, char *err, size_t en)
{
  fits_reader *r = calloc (1, sizeof *r);
  int k, py, px, ch, w = h->w;
  if (!r)
    {
      snprintf (err, en, "Not enough memory to read this FITS picture");
      return NULL;
    }
  r->f = f;
  r->h = *h;
  r->bpp = (h->bitpix < 0 ? -h->bitpix : h->bitpix) / 8;
  r->half = half && h->bayer[0];
  r->ow = r->half ? w / 2 : w;
  r->oh = r->half ? h->h / 2 : h->h;
  r->raw = malloc ((size_t) w * r->bpp);
  for (k = 0; k < 3; k++)
    {
      r->cache[k] = malloc ((size_t) w * sizeof (uint16_t));
      r->cached[k] = -1;
    }
  r->planes = malloc ((size_t) w * 3 * sizeof (uint16_t));
  r->row3 = malloc ((size_t) w * 3 * sizeof (uint16_t));
  if (!r->raw || !r->cache[0] || !r->cache[1] || !r->cache[2] || !r->planes || !r->row3)
    {
      fits_reader_free (r);
      snprintf (err, en, "Not enough memory to read this FITS picture");
      return NULL;
    }
  if (h->bayer[0])
    {
      for (py = 0; py < 2; py++)
        for (px = 0; px < 2; px++)
          {
            char c = h->bayer[((py + h->yoff) & 1) * 2 + ((px + h->xoff) & 1)];
            r->cfa[py][px] = c == 'R' ? 0 : c == 'G' ? 1 : 2;
          }
      /* Bilinear: a missing colour is the average of the nearest
         neighbours that have it (the 3 x 3 around the pixel holds two,
         or four, of each colour it hasn't got). */
      for (py = 0; py < 2; py++)
        for (px = 0; px < 2; px++)
          for (ch = 0; ch < 3; ch++)
            {
              int dy, dx, nn = 0;
              for (dy = -1; dy <= 1; dy++)
                for (dx = -1; dx <= 1; dx++)
                  if ((dy || dx) && r->cfa[(py + dy) & 1][(px + dx) & 1] == ch && nn < 4)
                    {
                      r->nb[py][px][ch][nn][0] = (signed char) dy;
                      r->nb[py][px][ch][nn++][1] = (signed char) dx;
                    }
              r->nn[py][px][ch] = nn;
            }
    }
  if (measure (r, err, en))
    {
      fits_reader_free (r);
      return NULL;
    }
  return r;
}

void fits_reader_size (const fits_reader *r, int *w, int *h)
{
  *w = r->ow;
  *h = r->oh;
}

int fits_reader_stretched (const fits_reader *r)
{
  return r->stretched;
}

/* Row y as three 0-65535 linear values a pixel (grey: the same three). */
static int row16 (fits_reader *r, int y, uint16_t *o)
{
  int x, w = r->h.w;
  if (r->h.planes == 3)
    {
      int p;
      for (p = 0; p < 3; p++)
        if (read_norm (r, p, y, r->planes + (size_t) p * w))
          return -1;
      for (x = 0; x < w; x++, o += 3)
        {
          o[0] = r->planes[x];
          o[1] = r->planes[w + x];
          o[2] = r->planes[2 * w + x];
        }
      return 0;
    }
  if (!r->h.bayer[0])
    {
      const uint16_t *s = row_of (r, y, y);
      if (!s)
        return -1;
      for (x = 0; x < w; x++, o += 3)
        o[0] = o[1] = o[2] = s[x];
      return 0;
    }
  if (r->half)
    {
      /* One pixel for each 2 x 2: its red, its blue, its two greens' average. */
      const uint16_t *a = row_of (r, 2 * y, 2 * y), *b = a ? row_of (r, 2 * y + 1, 2 * y) : NULL;
      if (!b)
        return -1;
      for (x = 0; x < r->ow; x++, o += 3)
        {
          unsigned v[3] = { 0, 0, 0 }, n[3] = { 0, 0, 0 };
          int dy, dx, c;
          for (dy = 0; dy < 2; dy++)
            for (dx = 0; dx < 2; dx++)
              {
                c = r->cfa[dy][dx];
                v[c] += (dy ? b : a)[2 * x + dx];
                n[c]++;
              }
          for (c = 0; c < 3; c++)
            o[c] = (uint16_t) (n[c] ? (v[c] + n[c] / 2) / n[c] : 0);
        }
      return 0;
    }
  {
    const uint16_t *rows[3];
    int py = y & 1;
    rows[0] = row_of (r, y - 1, y);
    rows[1] = row_of (r, y, y);
    rows[2] = row_of (r, y + 1, y);
    if (!rows[0] || !rows[1] || !rows[2])
      return -1;
    for (x = 0; x < w; x++, o += 3)
      {
        int px = x & 1, ch;
        for (ch = 0; ch < 3; ch++)
          {
            unsigned v;
            if (r->cfa[py][px] == ch)
              v = rows[1][x];
            else
              {
                int k, nn = r->nn[py][px][ch];
                unsigned sum = 0;
                for (k = 0; k < nn; k++)
                  {
                    int xx = x + r->nb[py][px][ch][k][1];
                    if (xx < 0)
                      xx = 1;
                    else if (xx >= w)
                      xx = w - 2;
                    sum += rows[1 + r->nb[py][px][ch][k][0]][xx];
                  }
                v = nn ? (sum + (unsigned) nn / 2) / (unsigned) nn : 0;
              }
            o[ch] = (uint16_t) v;
          }
      }
  }
  return 0;
}

int fits_reader_row (fits_reader *r, int y, unsigned char *o)
{
  int x;
  if (row16 (r, y, r->row3))
    return -1;
  for (x = 0; x < r->ow; x++, o += 3)
    {
      o[0] = r->lut[0][r->row3[3 * x]];
      o[1] = r->lut[1][r->row3[3 * x + 1]];
      o[2] = r->lut[2][r->row3[3 * x + 2]];
    }
  return 0;
}

int fits_reader_row_linear (fits_reader *r, int y, float *o)
{
  int x;
  if (row16 (r, y, r->row3))
    return -1;
  for (x = 0; x < 3 * r->ow; x++)
    o[x] = r->row3[x] * (1.0f / 65535);
  return 0;
}

void fits_reader_free (fits_reader *r)
{
  int k;
  if (!r)
    return;
  free (r->raw);
  for (k = 0; k < 3; k++)
    free (r->cache[k]);
  free (r->planes);
  free (r->row3);
  free (r);
}

/* --- writing --- */

struct fits_writer
{
  FILE *f;
  char path[1024];
  int w, h, planes, bitpix, bad;
  long data;
  unsigned char *buf;
};

static void put_card (FILE *f, const char *c, int *n)
{
  char b[81];
  snprintf (b, sizeof b, "%-80.80s", c);
  fwrite (b, 1, 80, f);
  (*n)++;
}

fits_writer *fits_writer_open (const char *path, int w, int h, int planes, int bitpix, const char *const *cards,
                               char *err, size_t en)
{
  fits_writer *wr;
  char c[100];
  int n = 0;
  if (w < 1 || h < 1 || (planes != 1 && planes != 3) || (bitpix != 16 && bitpix != -32))
    {
      snprintf (err, en, "Can't write a FITS picture like that");
      return NULL;
    }
  wr = calloc (1, sizeof *wr);
  if (!wr || !(wr->buf = malloc ((size_t) w * 4)))
    {
      free (wr);
      snprintf (err, en, "Not enough memory to write the FITS file");
      return NULL;
    }
  snprintf (wr->path, sizeof wr->path, "%s", path);
  wr->w = w; wr->h = h; wr->planes = planes; wr->bitpix = bitpix;
  wr->f = fopen (path, "wb");
  if (!wr->f)
    {
      free (wr->buf);
      free (wr);
      snprintf (err, en, "Can't write %s", path);
      return NULL;
    }
  put_card (wr->f, "SIMPLE  =                    T / conforms to the FITS standard", &n);
  snprintf (c, sizeof c, "BITPIX  = %20d", bitpix); put_card (wr->f, c, &n);
  snprintf (c, sizeof c, "NAXIS   = %20d", planes == 3 ? 3 : 2); put_card (wr->f, c, &n);
  snprintf (c, sizeof c, "NAXIS1  = %20d", w); put_card (wr->f, c, &n);
  snprintf (c, sizeof c, "NAXIS2  = %20d", h); put_card (wr->f, c, &n);
  if (planes == 3)
    {
      snprintf (c, sizeof c, "NAXIS3  = %20d", 3);
      put_card (wr->f, c, &n);
    }
  if (bitpix == 16)
    {
      put_card (wr->f, "BZERO   =              32768.0", &n);
      put_card (wr->f, "BSCALE  =                  1.0", &n);
    }
  put_card (wr->f, "ROWORDER= 'BOTTOM-UP'", &n);
  for (; cards && *cards; cards++)
    put_card (wr->f, *cards, &n);
  put_card (wr->f, "END", &n);
  while (n % 36)
    put_card (wr->f, "", &n);
  wr->data = (long) n * CARD;
  /* The file at its full size at once (padded to whole blocks), so rows
     can be written in any order inside it. */
  {
    long long end = wr->data + (long long) w * h * planes * (bitpix == 16 ? 2 : 4);
    end = (end + BLOCK - 1) / BLOCK * BLOCK;
    if (end > 0x7FFFFFFFLL || fseek (wr->f, (long) end - 1, SEEK_SET) != 0 || fputc (0, wr->f) == EOF)
      {
        fclose (wr->f);
        remove (path);
        free (wr->buf);
        free (wr);
        snprintf (err, en, "Can't make the FITS file (is the disc full?)");
        return NULL;
      }
  }
  return wr;
}

int fits_writer_row (fits_writer *wr, int y, const float *v)
{
  int p, x, bpp = wr->bitpix == 16 ? 2 : 4;
  size_t len = (size_t) wr->w * bpp;
  if (y < 0 || y >= wr->h)
    return -1;
  for (p = 0; p < wr->planes; p++)
    {
      long off = wr->data + (long) (((long long) p * wr->h + (wr->h - 1 - y)) * (long long) len);
      unsigned char *o = wr->buf;
      for (x = 0; x < wr->w; x++)
        {
          float f = v[(size_t) x * wr->planes + p];
          if (f != f)
            f = 0;
          if (bpp == 2)
            {
              int u = f <= 0 ? 0 : f >= 1 ? 65535 : (int) (f * 65535 + 0.5f);
              u -= 32768;
              *o++ = (unsigned char) ((unsigned) u >> 8);
              *o++ = (unsigned char) u;
            }
          else
            {
              uint32_t u;
              memcpy (&u, &f, 4);
              *o++ = (unsigned char) (u >> 24);
              *o++ = (unsigned char) (u >> 16);
              *o++ = (unsigned char) (u >> 8);
              *o++ = (unsigned char) u;
            }
        }
      if (fseek (wr->f, off, SEEK_SET) != 0 || fwrite (wr->buf, 1, len, wr->f) != len)
        {
          wr->bad = 1;
          return -1;
        }
    }
  return 0;
}

int fits_writer_close (fits_writer *wr)
{
  int bad;
  if (!wr)
    return -1;
  if (fclose (wr->f))
    wr->bad = 1;
  bad = wr->bad;
  if (bad)
    remove (wr->path);
  free (wr->buf);
  free (wr);
  return bad ? -1 : 0;
}
