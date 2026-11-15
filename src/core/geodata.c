/* Copyright (c) 2026 Andrew Youll
   Released under the MIT licence: see the LICENSE file.
   SPDX-License-Identifier: MIT */
/* geodata.c: see geodata.h. Layouts from Sine Nomine's "RISC OS geodata
   messaging system" (Message_GeoData_*, &546D0-&546DD). */
#include <stdio.h>
#include <string.h>
#include "geodata.h"

/* The length of the UTF-8 character starting at p (1-4), 0 if it isn't one. */
static int utf8_char (const unsigned char *p)
{
  int n, k;
  if (p[0] < 0x80)
    return 1;
  n = p[0] >= 0xF0 && p[0] <= 0xF4 ? 4 : p[0] >= 0xE0 ? 3 : p[0] >= 0xC2 && p[0] < 0xE0 ? 2 : 0;
  if (p[0] >= 0xF5)
    n = 0;
  for (k = 1; k < n; k++)
    if ((p[k] & 0xC0) != 0x80)
      return 0;
  return n;
}

static int is_utf8 (const char *s)
{
  const unsigned char *p = (const unsigned char *) s;
  int multi = 0;
  while (*p)
    {
      int n = utf8_char (p);
      if (!n)
        return 0;
      multi |= n > 1;
      p += n;
    }
  return multi;
}

size_t geodata_utf8 (const char *in, char *out, size_t n)
{
  const unsigned char *p = (const unsigned char *) (in ? in : "");
  size_t len = 0;
  int utf8 = is_utf8 ((const char *) p);
  if (!n)
    return 0;
  while (*p)
    {
      unsigned char one[4];
      int k = 1;
      if (utf8)
        {
          k = utf8_char (p);
          memcpy (one, p, (size_t) k);
        }
      else if (*p < 0x80)
        one[0] = *p;
      else
        {
          one[0] = (unsigned char) (0xC0 | *p >> 6);
          one[1] = (unsigned char) (0x80 | (*p & 0x3F));
          k = 2;
        }
      if (len + (size_t) k + 1 > n)
        break;
      memcpy (out + len, one, (size_t) k);
      len += (size_t) k;
      p += utf8 ? k : 1;
    }
  out[len] = '\0';
  return len;
}

void geodata_latin1 (const char *in, char *out, size_t n)
{
  const unsigned char *p = (const unsigned char *) in;
  size_t len = 0;
  if (!n)
    return;
  while (*p && len + 1 < n)
    {
      int k = utf8_char (p);
      if (k == 2)
        {
          unsigned c = (unsigned) (p[0] & 0x1F) << 6 | (p[1] & 0x3F);
          out[len++] = c < 0x100 ? (char) c : '?';
        }
      else if (k > 2)
        out[len++] = '?';
      else
        {
          out[len++] = (char) *p;
          k = 1;
        }
      p += k;
    }
  out[len] = '\0';
}

/* Days from 1 March 0000 (proleptic Gregorian) to y-m-d. */
static long days_from_civil (long y, int m, int d)
{
  long era, yoe, doy, doe;
  y -= m <= 2;
  era = (y >= 0 ? y : y - 399) / 400;
  yoe = y - era * 400;
  doy = (153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + d - 1;
  doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
  return era * 146097 + doe;
}

int geodata_time (const char *taken, unsigned char out[5])
{
  int y, mo, d, h = 0, mi = 0, s = 0;
  unsigned long long cs;
  long days;
  int k;
  memset (out, 0, 5);
  if (!taken || sscanf (taken, "%d:%d:%d %d:%d:%d", &y, &mo, &d, &h, &mi, &s) < 3
      || y < 1900 || y > 2200 || mo < 1 || mo > 12 || d < 1 || d > 31 || h < 0 || h > 23 || mi < 0 || mi > 59
      || s < 0 || s > 60)
    return -1;
  days = days_from_civil (y, mo, d) - days_from_civil (1900, 1, 1);
  cs = ((unsigned long long) days * 86400 + (unsigned long long) h * 3600 + (unsigned long long) mi * 60
        + (unsigned long long) s) * 100;
  for (k = 0; k < 5; k++)
    out[k] = (unsigned char) (cs >> (8 * k));
  return 0;
}

void geodata_transmit (int *block, int map, int dataset, const cat_geo_hit *h, int flags)
{
  unsigned char *b = (unsigned char *) block;
  size_t at = 76, room;
  const char *title = h->caption[0] ? h->caption : h->name;
  const char *description = h->caption[0] ? h->name : "";
  memset (block, 0, GEODATA_WORDS * 4);
  block[4] = GeoData_TransmitData;
  block[5] = map;
  block[6] = dataset;
  block[7] = (int) h->version;
  block[8] = GD_PHOTOS;
  flags |= GD_LATLON;
  block[10] = h->geo.lon;
  block[11] = h->geo.lat;
  if (h->geo.geo & GEO_ALT)
    {
      flags |= GD_ALTITUDE;
      block[12] = h->geo.alt_mm;
    }
  if (geodata_time (h->taken, b + 52) == 0)
    flags |= GD_TIME;                   /* exact (bits 26 and 27 clear) */
  if (h->geo.geo & GEO_DIR)
    {
      flags |= GD_HAS_BEARING;
      block[16] = h->geo.bearing;
    }
  /* The title and the description after it, in what's left of 256 bytes;
     the description goes first if there isn't room for both. */
  room = GEODATA_WORDS * 4 - at;
  if (title[0])
    {
      size_t tl = geodata_utf8 (title, (char *) b + at, room > 160 && description[0] ? room - 60 : room);
      flags |= GD_TITLE;
      at += tl + 1;
      room = GEODATA_WORDS * 4 - at;
      if (description[0] && room > 8)
        {
          at += geodata_utf8 (description, (char *) b + at, room) + 1;
          flags |= GD_DESCRIPTION;
        }
    }
  block[9] = flags;
  block[0] = (int) ((at + 3) & ~(size_t) 3);
}

void geodata_open_map (int *block, const cat_geo_hit *h, int n, const char *title)
{
  long long x0 = 0, y0 = 0, x1 = 0, y1 = 0;
  int k, flags = 0;
  memset (block, 0, GEODATA_WORDS * 4);
  block[4] = GeoData_OpenMap;
  for (k = 0; k < n; k++)
    {
      long long lo = h[k].geo.lon, la = h[k].geo.lat;
      if (k == 0 || lo < x0) x0 = lo;
      if (k == 0 || lo > x1) x1 = lo;
      if (k == 0 || la < y0) y0 = la;
      if (k == 0 || la > y1) y1 = la;
    }
  /* A few hundred metres (about 0.003 degrees) or less: the centre, at
     the map's own scale; else the box, with a margin. */
  if (n > 1 && (x1 - x0 > 30000 || y1 - y0 > 30000))
    {
      long long mx = (x1 - x0) / 6 + 2000, my = (y1 - y0) / 6 + 2000;
      x0 -= mx; x1 += mx; y0 -= my; y1 += my;
      if (x0 < -1800000000LL) x0 = -1800000000LL;
      if (x1 > 1799999999LL) x1 = 1799999999LL;
      if (y0 < -850000000LL) y0 = -850000000LL;
      if (y1 > 850000000LL) y1 = 850000000LL;
      flags |= GD_OPEN_BOUNDS;
      block[6] = (int) x0;
      block[7] = (int) y0;
      block[8] = (int) x1;
      block[9] = (int) y1;
    }
  else
    {
      block[6] = (int) ((x0 + x1) / 2);
      block[7] = (int) ((y0 + y1) / 2);
    }
  if (title && title[0])
    {
      size_t len = geodata_utf8 (title, (char *) block + 60, 100);
      flags |= GD_OPEN_TITLE;
      block[0] = (int) ((60 + len + 1 + 3) & ~(size_t) 3);
    }
  else
    block[0] = 60;
  block[5] = flags;
}

void geodata_details (int *block, int dataset, int n, int x0, int y0, int x1, int y1,
                      const char *title, const char *description)
{
  unsigned char *b = (unsigned char *) block;
  size_t at = 48;
  memset (block, 0, GEODATA_WORDS * 4);
  block[4] = GeoData_DatasetDetails;
  block[5] = dataset;
  if (n > 0)
    {
      block[6] = x0;
      block[7] = y0;
      block[8] = x1;
      block[9] = y1;
      block[10] = GD_PHOTOS | GD_BOX | GD_RADIUS | GD_TEXT;
      block[11] = GD_FETCH_BOX | GD_FETCH_RADIUS | GD_SEARCH_ALL | GD_SEARCH_BOX | GD_SEARCH_RADIUS
                | GD_SUPPORTS_PAGES | GD_BY_ID;
      at += geodata_utf8 (title, (char *) b + at, 100) + 1;
      at += geodata_utf8 (description, (char *) b + at, GEODATA_WORDS * 4 - at) + 1;
    }
  else
    at += 2;                            /* no title, no description */
  block[0] = (int) ((at + 3) & ~(size_t) 3);
}

void geodata_status (int *block, int code, int quantity, const char *message)
{
  size_t at = 28;
  memset (block, 0, GEODATA_WORDS * 4);
  block[4] = GeoData_Status;
  block[5] = code;
  block[6] = quantity;
  at += geodata_utf8 (message, (char *) block + at, GEODATA_WORDS * 4 - at) + 1;
  block[0] = (int) ((at + 3) & ~(size_t) 3);
}

void geodata_read_fetch (const int *block, geodata_fetch *f)
{
  memset (f, 0, sizeof *f);
  f->map = block[5];
  f->dataset = block[6];
  f->object = block[7];
  f->flags = block[8];
  f->a = block[9];
  f->b = block[10];
  f->c = block[11];
  f->d = block[12];
  f->what = block[13];
  if (f->what & GD_TEXT)
    {
      f->text_at = block[14];
      f->text_len = block[15];
    }
  if (f->what & GD_PAGED)
    {
      f->page_size = block[16];
      f->page = block[17];
    }
}

int geodata_query (const geodata_fetch *f, const char *words, cat_geo_query *q)
{
  int types = f->what & GD_TYPES;
  memset (q, 0, sizeof *q);
  if (types && types != GD_TYPES && !(types & GD_PHOTOS))
    return -1;                          /* not photos */
  if (f->flags & GD_CANCEL_UPDATES)
    return -1;
  q->version = f->object > 0 ? f->object : 0;
  if (f->what & GD_RADIUS)
    {
      q->how = CAT_GEO_RADIUS;
      q->x0 = f->a;
      q->y0 = f->b;
      q->radius = f->c > 0 ? f->c : 0;
    }
  else if ((f->what & GD_BOX) || f->a || f->b || f->c || f->d)
    {
      q->how = CAT_GEO_BOX;
      q->x0 = f->a;
      q->y0 = f->b;
      q->x1 = f->c;
      q->y1 = f->d;
    }
  if (words)
    geodata_latin1 (words, q->words, sizeof q->words);
  if (f->what & GD_PAGED)
    {
      q->page_size = f->page_size > 0 ? f->page_size : 0;
      q->page = f->page > 0 ? f->page : 1;
    }
  return 0;
}
