/* exifout.c: see exifout.h. TIFF and EXIF layout from the EXIF 2.32
   specification (as in exif.c). Every value is read a byte at a time in
   the original's byte order and written little-endian. */
#include <math.h>
#include <stdio.h>
#include <string.h>
#include "exifout.h"

#define MAX_TAGS 96
#define POOL 62000

typedef struct
{
  const unsigned char *p;
  size_t len;
  int big;
} src_tiff;

static unsigned rd16 (const src_tiff *t, size_t off)
{
  if (off + 2 > t->len || off + 2 < off)
    return 0;
  return t->big ? (unsigned) (t->p[off] << 8 | t->p[off + 1]) : (unsigned) (t->p[off] | t->p[off + 1] << 8);
}

static unsigned long rd32 (const src_tiff *t, size_t off)
{
  if (off + 4 > t->len || off + 4 < off)
    return 0;
  if (t->big)
    return (unsigned long) t->p[off] << 24 | (unsigned long) t->p[off + 1] << 16
         | (unsigned long) t->p[off + 2] << 8 | t->p[off + 3];
  return (unsigned long) t->p[off] | (unsigned long) t->p[off + 1] << 8
       | (unsigned long) t->p[off + 2] << 16 | (unsigned long) t->p[off + 3] << 24;
}

static const unsigned type_size[13] = { 0, 1, 1, 2, 4, 8, 1, 1, 2, 4, 8, 4, 8 };

typedef struct { unsigned tag, type; unsigned long count; size_t at, len; } entry;   /* at: into the pool */

typedef struct
{
  entry e[MAX_TAGS];
  int n;
} ifd;

typedef struct
{
  unsigned char pool[POOL];
  size_t used;
  ifd ifd0, exif, gps;
} builder;

static void put16 (unsigned char *p, unsigned v) { p[0] = (unsigned char) v; p[1] = (unsigned char) (v >> 8); }
static void put32 (unsigned char *p, unsigned long v) { put16 (p, (unsigned) (v & 0xFFFF)); put16 (p + 2, (unsigned) (v >> 16)); }

static entry *find (ifd *d, unsigned tag)
{
  int k;
  for (k = 0; k < d->n; k++)
    if (d->e[k].tag == tag)
      return &d->e[k];
  return NULL;
}

/* Adds (or replaces) a tag whose value, little-endian already, is data. */
static void add (builder *b, ifd *d, unsigned tag, unsigned type, unsigned long count, const void *data, size_t len)
{
  entry *e = find (d, tag);
  if (!e)
    {
      if (d->n >= MAX_TAGS)
        return;
      e = &d->e[d->n++];
    }
  if (b->used + len + 1 > POOL)
    {
      d->n--;                          /* no room: left out */
      return;
    }
  e->tag = tag; e->type = type; e->count = count;
  e->at = b->used;
  e->len = len;
  memmove (b->pool + b->used, data, len);   /* copy_entry builds it in place */
  b->used += len;
}

static void remove_tag (ifd *d, unsigned tag)
{
  entry *e = find (d, tag);
  if (e)
    *e = d->e[--d->n];
}

static void add_short (builder *b, ifd *d, unsigned tag, unsigned v)
{
  unsigned char x[2];
  put16 (x, v);
  add (b, d, tag, 3, 1, x, 2);
}

static void add_long (builder *b, ifd *d, unsigned tag, unsigned long v)
{
  unsigned char x[4];
  put32 (x, v);
  add (b, d, tag, 4, 1, x, 4);
}

static void add_ascii (builder *b, ifd *d, unsigned tag, const char *s)
{
  size_t l = strlen (s);
  if (l)
    add (b, d, tag, 2, (unsigned long) l + 1, s, l + 1);
}

/* A positive number as a rational: 1/250 for shutter speeds, tenths
   above 1, millionths otherwise. */
static void add_rational (builder *b, ifd *d, unsigned tag, double v)
{
  unsigned char x[8];
  unsigned long n, den;
  if (!(v > 0) || v > 1e6)
    return;
  if (v < 1 && fabs (1 / v - floor (1 / v + 0.5)) < 0.02)
    {
      n = 1;
      den = (unsigned long) floor (1 / v + 0.5);
    }
  else if (v >= 1)
    {
      n = (unsigned long) floor (v * 10 + 0.5);
      den = 10;
    }
  else
    {
      n = (unsigned long) floor (v * 1000000 + 0.5);
      den = 1000000;
    }
  put32 (x, n);
  put32 (x + 4, den);
  add (b, d, tag, 5, 1, x, 8);
}

/* Copies one entry of the original, in its byte order, into the pool. */
static void copy_entry (builder *b, ifd *d, const src_tiff *t, size_t ent)
{
  unsigned tag = rd16 (t, ent), type = rd16 (t, ent + 2);
  unsigned long count = rd32 (t, ent + 4), k, elems;
  size_t len, at, esz;
  unsigned char *tmp = b->pool + b->used;
  if (type == 0 || type > 12 || count == 0 || count > 60000)
    return;
  len = type_size[type] * (size_t) count;
  if (len > 60000 || b->used + len + 1 > POOL)
    return;
  at = len <= 4 ? ent + 8 : (size_t) rd32 (t, ent + 8);
  if (at + len > t->len || at + len < at)
    return;
  /* Elements to turn round: 2 or 4 bytes (rationals are two 4-byte halves; doubles 8). */
  esz = type_size[type] == 8 && type != 12 ? 4 : type_size[type];
  elems = (unsigned long) (len / esz);
  for (k = 0; k < elems; k++)
    {
      size_t o = (size_t) k * esz;
      if (esz == 2)
        put16 (tmp + o, rd16 (t, at + o));
      else if (esz == 4)
        put32 (tmp + o, rd32 (t, at + o));
      else if (esz == 8)
        {
          int j;
          for (j = 0; j < 8; j++)
            tmp[o + j] = t->p[at + o + (t->big ? 7 - j : j)];
        }
      else
        tmp[o] = t->p[at + o];
    }
  if (type == 2)
    {
      /* Cameras fill Artist and Copyright with spaces when nobody set them. */
      size_t j = 0;
      while (j < len && (tmp[j] == ' ' || tmp[j] == 0))
        j++;
      if (j == len)
        return;
    }
  add (b, d, tag, type, count, tmp, len);
}

/* An IFD of the original: the tags keep() wants. */
static void copy_ifd (builder *b, ifd *d, const src_tiff *t, size_t off, int (*keep) (unsigned),
                      unsigned long *exif_ptr, unsigned long *gps_ptr)
{
  unsigned n, i;
  if (off < 8 || off + 2 > t->len)
    return;
  n = rd16 (t, off);
  if (n > 1000 || off + 2 + 12 * (size_t) n > t->len)
    return;
  for (i = 0; i < n; i++)
    {
      size_t ent = off + 2 + 12 * (size_t) i;
      unsigned tag = rd16 (t, ent);
      if (tag == 0x8769 && exif_ptr)
        *exif_ptr = rd32 (t, ent + 8);
      else if (tag == 0x8825 && gps_ptr)
        *gps_ptr = rd32 (t, ent + 8);
      else if (keep (tag))
        copy_entry (b, d, t, ent);
    }
}

/* IFD0: the photo's description, not the file's structure. */
static int keep_ifd0 (unsigned tag)
{
  return tag == 0x010E || tag == 0x010F || tag == 0x0110 || tag == 0x011A || tag == 0x011B || tag == 0x0128
         || tag == 0x0132 || tag == 0x013B || tag == 0x8298;
}

/* The Exif IFD: all but the maker's notes, pointers, padding and what Lumen sets. */
static int keep_exif (unsigned tag)
{
  return tag != 0x927C && tag != 0xA005 && tag != 0xA001 && tag != 0xA002 && tag != 0xA003 && tag != 0xEA1C
         && tag != 0xA500 && tag != 0xEA1D;
}

static int keep_all (unsigned tag)
{
  (void) tag;
  return 1;
}

int exif_find_tiff (const unsigned char *buf, size_t len, size_t *off, size_t *n)
{
  size_t pos = 2;
  if (len >= 8 && ((buf[0] == 'I' && buf[1] == 'I' && buf[2] == 42 && buf[3] == 0)
                   || (buf[0] == 'M' && buf[1] == 'M' && buf[2] == 0 && buf[3] == 42)))
    {
      *off = 0;
      *n = len;
      return 0;
    }
  if (len < 4 || buf[0] != 0xFF || buf[1] != 0xD8)
    return -1;
  while (pos + 4 <= len && buf[pos] == 0xFF)
    {
      unsigned marker = buf[pos + 1], seglen;
      if (marker == 0xFF)
        {
          pos++;
          continue;
        }
      if (marker == 0xDA || marker == 0xD9)
        break;
      seglen = (unsigned) (buf[pos + 2] << 8 | buf[pos + 3]);
      if (seglen < 2)
        break;
      if (marker == 0xE1 && seglen > 8 && pos + 10 <= len && memcmp (buf + pos + 4, "Exif\0\0", 6) == 0)
        {
          size_t end = pos + 2 + seglen;
          if (end > len)
            end = len;
          *off = pos + 10;
          *n = end - *off;
          return 0;
        }
      pos += 2 + seglen;
    }
  return -1;
}

static int by_tag (const entry *a, const entry *b) { return (int) a->tag - (int) b->tag; }

static void sort (ifd *d)
{
  int i, j;
  for (i = 1; i < d->n; i++)
    for (j = i; j > 0 && by_tag (&d->e[j - 1], &d->e[j]) > 0; j--)
      {
        entry x = d->e[j];
        d->e[j] = d->e[j - 1];
        d->e[j - 1] = x;
      }
}

static size_t ifd_size (const ifd *d)
{
  size_t s = 2 + 12 * (size_t) d->n + 4;
  int k;
  for (k = 0; k < d->n; k++)
    if (d->e[k].len > 4)
      s += (d->e[k].len + 1) & ~(size_t) 1;
  return s;
}

/* Writes d at off in out (cap bytes); returns 0 if it fitted. */
static int write_ifd (const builder *b, const ifd *d, unsigned char *out, size_t cap, size_t off)
{
  size_t data = off + 2 + 12 * (size_t) d->n + 4;
  int k;
  if (off + ifd_size (d) > cap)
    return -1;
  put16 (out + off, (unsigned) d->n);
  for (k = 0; k < d->n; k++)
    {
      const entry *e = &d->e[k];
      unsigned char *p = out + off + 2 + 12 * (size_t) k;
      put16 (p, e->tag);
      put16 (p + 2, e->type);
      put32 (p + 4, e->count);
      memset (p + 8, 0, 4);
      if (e->len <= 4)
        memcpy (p + 8, b->pool + e->at, e->len);
      else
        {
          put32 (p + 8, (unsigned long) data);
          memcpy (out + data, b->pool + e->at, e->len);
          if (e->len & 1)
            out[data + e->len] = 0;
          data += (e->len + 1) & ~(size_t) 1;
        }
    }
  put32 (out + off + 2 + 12 * (size_t) d->n, 0);   /* no next IFD: no thumbnail */
  return 0;
}

static builder bld;                    /* 62 KB: not on the stack */

size_t exif_for_export (const unsigned char *src, size_t src_len, const img_info *known,
                        int w, int h, const char *software, unsigned char *out, size_t cap)
{
  builder *b = &bld;
  size_t off_exif, off_gps, total;
  int have = 0;
  memset (b, 0, sizeof *b);
  if (src && src_len >= 8)
    {
      src_tiff t;
      unsigned long ifd0, exif_ptr = 0, gps_ptr = 0;
      t.p = src;
      t.len = src_len;
      t.big = src[0] == 'M';
      if ((src[0] == 'I' || src[0] == 'M') && src[0] == src[1] && rd16 (&t, 2) == 42)
        {
          ifd0 = rd32 (&t, 4);
          copy_ifd (b, &b->ifd0, &t, ifd0, keep_ifd0, &exif_ptr, &gps_ptr);
          if (exif_ptr)
            copy_ifd (b, &b->exif, &t, exif_ptr, keep_exif, NULL, NULL);
          if (gps_ptr)
            copy_ifd (b, &b->gps, &t, gps_ptr, keep_all, NULL, NULL);
          have = b->ifd0.n + b->exif.n + b->gps.n > 0;
        }
    }
  if (!have && known)
    {
      /* What the library knows, for originals with no EXIF to copy. */
      add_ascii (b, &b->ifd0, 0x010F, known->make);
      add_ascii (b, &b->ifd0, 0x0110, known->model);
      add_ascii (b, &b->ifd0, 0x0132, known->taken);
      add_rational (b, &b->exif, 0x829A, known->exposure);
      add_rational (b, &b->exif, 0x829D, known->fnumber);
      if (known->iso > 0 && known->iso < 65536)
        add_short (b, &b->exif, 0x8827, (unsigned) known->iso);
      add_ascii (b, &b->exif, 0x9003, known->taken);
      add_ascii (b, &b->exif, 0x9004, known->taken);
      add_rational (b, &b->exif, 0x920A, known->focal);
      have = b->ifd0.n + b->exif.n > 0;
    }
  if (!have)
    return 0;
  /* What the new file is. */
  add_short (b, &b->ifd0, 0x0112, 1);
  if (software && *software)
    add_ascii (b, &b->ifd0, 0x0131, software);
  if (!find (&b->exif, 0x9000))
    add (b, &b->exif, 0x9000, 7, 4, "0232", 4);
  add_short (b, &b->exif, 0xA001, 1);
  add_long (b, &b->exif, 0xA002, (unsigned long) (w > 0 ? w : 0));
  add_long (b, &b->exif, 0xA003, (unsigned long) (h > 0 ? h : 0));
  /* The pointers, as placeholders: sizes are fixed once they are in. */
  remove_tag (&b->ifd0, 0x8769);
  remove_tag (&b->ifd0, 0x8825);
  add_long (b, &b->ifd0, 0x8769, 0);
  if (b->gps.n)
    add_long (b, &b->ifd0, 0x8825, 0);
  sort (&b->ifd0);
  sort (&b->exif);
  sort (&b->gps);
  off_exif = 8 + ifd_size (&b->ifd0);
  off_gps = off_exif + ifd_size (&b->exif);
  total = off_gps + (b->gps.n ? ifd_size (&b->gps) : 0);
  if (total > cap || total > EXIFOUT_MAX)
    return 0;
  put32 (b->pool + find (&b->ifd0, 0x8769)->at, (unsigned long) off_exif);
  if (b->gps.n)
    put32 (b->pool + find (&b->ifd0, 0x8825)->at, (unsigned long) off_gps);
  out[0] = 'I'; out[1] = 'I';
  put16 (out + 2, 42);
  put32 (out + 4, 8);
  if (write_ifd (b, &b->ifd0, out, cap, 8) || write_ifd (b, &b->exif, out, cap, off_exif)
      || (b->gps.n && write_ifd (b, &b->gps, out, cap, off_gps)))
    return 0;
  return total;
}
