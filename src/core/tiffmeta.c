/* Copyright (c) 2026 Andrew Youll
   Released under the MIT licence: see the LICENSE file.
   SPDX-License-Identifier: MIT */
/* tiffmeta.c: see tiffmeta.h. A directory (IFD) is read into a small tree
   (its entries, each with its value's bytes, little-endian whatever the
   file was, and the EXIF, GPS and interoperability IFDs under the entries
   that point at them), and written out again at any offset. */
#include <stdlib.h>
#include <string.h>
#include "tiffmeta.h"
#include "simd.h"

#define MAX_ENTRIES 1000
#define MAX_VALUE (1024 * 1024)          /* a tag's value: larger ones are left out */
#define COMPACT_VALUE (64 * 1024)        /* ...and from the compact EXIF copy, larger than this */

enum { TAG_WIDTH = 256, TAG_HEIGHT = 257, TAG_BITS = 258, TAG_COMPRESSION = 259, TAG_PHOTOMETRIC = 262,
       TAG_STRIP_OFFSETS = 273, TAG_ORIENTATION = 274, TAG_SAMPLES = 277, TAG_ROWS = 278,
       TAG_STRIP_BYTES = 279, TAG_XRES = 282, TAG_YRES = 283, TAG_PLANAR = 284, TAG_RES_UNIT = 296,
       TAG_PREDICTOR = 317, TAG_TILE_WIDTH = 322, TAG_TILE_LENGTH = 323, TAG_TILE_OFFSETS = 324,
       TAG_TILE_BYTES = 325, TAG_SUBIFDS = 330, TAG_EXTRA = 338, TAG_SAMPLE_FORMAT = 339, TAG_XMP = 700,
       TAG_IPTC = 33723, TAG_PHOTOSHOP = 34377, TAG_EXIF_IFD = 34665, TAG_ICC = 34675, TAG_GPS_IFD = 34853,
       TAG_IMAGE_SOURCE = 37724, TAG_INTEROP_IFD = 40965 };

typedef struct node node;
typedef struct
{
  unsigned tag, type;
  unsigned long count;
  unsigned char *data;                 /* little-endian */
  size_t len;
  node *sub;                           /* an IFD this entry points at */
} entry;
struct node
{
  int n, cap;
  entry *e;
};

/* --- reading */

typedef struct
{
  FILE *f;                             /* the file, or */
  const unsigned char *p;              /* a block in memory */
  size_t len;
  int big;                             /* "MM" */
} source;

static int src_read (const source *s, unsigned long off, unsigned char *buf, size_t n)
{
  if (s->f)
    return fseek (s->f, (long) off, SEEK_SET) == 0 && fread (buf, 1, n, s->f) == n ? 0 : -1;
  if (off > s->len || n > s->len - off)
    return -1;
  memcpy (buf, s->p + off, n);
  return 0;
}

static unsigned get16 (const source *s, const unsigned char *b)
{
  return s->big ? (unsigned) (b[0] << 8 | b[1]) : (unsigned) (b[1] << 8 | b[0]);
}

static unsigned long get32 (const source *s, const unsigned char *b)
{
  return s->big ? (unsigned long) b[0] << 24 | (unsigned long) b[1] << 16 | (unsigned long) b[2] << 8 | b[3]
                : (unsigned long) b[3] << 24 | (unsigned long) b[2] << 16 | (unsigned long) b[1] << 8 | b[0];
}

/* Bytes in each part of a value that are swapped together, by type (0: not a type). */
static int unit_of (unsigned type)
{
  static const unsigned char u[14] = { 0, 1, 1, 2, 4, 4, 1, 1, 2, 4, 4, 4, 8, 4 };
  return type < 14 ? u[type] : 0;
}

/* RATIONAL and SRATIONAL are two LONGs: 8-byte values of 4-byte parts. */
static int size_of (unsigned type)
{
  return type == 5 || type == 10 ? 8 : unit_of (type);
}

static void free_node (node *n)
{
  int i;
  if (!n)
    return;
  for (i = 0; i < n->n; i++)
    {
      free (n->e[i].data);
      free_node (n->e[i].sub);
    }
  free (n->e);
  free (n);
}

static entry *add_entry (node *n)
{
  if (n->n == n->cap)
    {
      int cap = n->cap ? n->cap * 2 : 32;
      entry *e = realloc (n->e, sizeof *e * (size_t) cap);
      if (!e)
        return NULL;
      n->e = e;
      n->cap = cap;
    }
  memset (&n->e[n->n], 0, sizeof n->e[0]);
  return &n->e[n->n++];
}

static int is_ifd_pointer (unsigned tag)
{
  return tag == TAG_EXIF_IFD || tag == TAG_GPS_IFD || tag == TAG_INTEROP_IFD;
}

/* Reads the IFD at off (and the IFDs its EXIF, GPS and interoperability
   entries point at). NULL if it can't be read. */
static node *read_ifd (const source *s, unsigned long off, int depth)
{
  unsigned char b[12];
  unsigned count, i;
  node *n;
  if (depth > 3 || off < 8 || src_read (s, off, b, 2))
    return NULL;
  count = get16 (s, b);
  if (count == 0 || count > MAX_ENTRIES || !(n = calloc (1, sizeof *n)))
    return NULL;
  for (i = 0; i < count; i++)
    {
      unsigned tag, type;
      unsigned long cnt;
      size_t size, k;
      int unit;
      entry *e;
      if (src_read (s, off + 2 + 12UL * i, b, 12))
        break;
      tag = get16 (s, b);
      type = get16 (s, b + 2);
      cnt = get32 (s, b + 4);
      unit = unit_of (type);
      if (!unit || cnt == 0 || cnt > MAX_VALUE)
        continue;
      size = (size_t) size_of (type) * cnt;
      if (size > MAX_VALUE)
        continue;
      if (!(e = add_entry (n)))
        break;
      e->tag = tag;
      e->type = type;
      e->count = cnt;
      e->len = size;
      if (!(e->data = malloc (size < 4 ? 4 : size)))
        {
          n->n--;
          break;
        }
      if (size <= 4)
        memcpy (e->data, b + 8, size);
      else if (src_read (s, get32 (s, b + 8), e->data, size))
        {
          free (e->data);
          n->n--;
          continue;
        }
      if (s->big && unit > 1)            /* to little-endian */
        for (k = 0; k + (size_t) unit <= size; k += (size_t) unit)
          {
            int a = 0, z = unit - 1;
            while (a < z)
              {
                unsigned char t = e->data[k + (size_t) a];
                e->data[k + (size_t) a] = e->data[k + (size_t) z];
                e->data[k + (size_t) z] = t;
                a++, z--;
              }
          }
      if (is_ifd_pointer (tag) && cnt == 1 && (type == 4 || type == 13))
        {
          unsigned long child = (unsigned long) e->data[0] | (unsigned long) e->data[1] << 8
                                | (unsigned long) e->data[2] << 16 | (unsigned long) e->data[3] << 24;
          e->sub = read_ifd (s, child, depth + 1);
          if (!e->sub)
            {
              free (e->data);
              n->n--;
            }
        }
    }
  if (!n->n)
    {
      free_node (n);
      return NULL;
    }
  return n;
}

static const entry *find (const node *n, unsigned tag)
{
  int i;
  for (i = 0; n && i < n->n; i++)
    if (n->e[i].tag == tag)
      return &n->e[i];
  return NULL;
}

/* The first value of an integer entry, or def. */
static long first_int (const node *n, unsigned tag, long def)
{
  const entry *e = find (n, tag);
  if (!e)
    return def;
  switch (e->type)
    {
    case 1: case 7: return e->data[0];
    case 3: return e->data[0] | e->data[1] << 8;
    case 8: return (short) (e->data[0] | e->data[1] << 8);
    case 4: case 9: case 13:
      return (long) ((unsigned long) e->data[0] | (unsigned long) e->data[1] << 8
                     | (unsigned long) e->data[2] << 16 | (unsigned long) e->data[3] << 24);
    }
  return def;
}

static unsigned char *copy_value (const node *n, unsigned tag, size_t *len)
{
  const entry *e = find (n, tag);
  unsigned char *p;
  *len = 0;
  if (!e || !(p = malloc (e->len)))
    return NULL;
  memcpy (p, e->data, e->len);
  *len = e->len;
  return p;
}

/* --- writing a tree out */

typedef struct
{
  unsigned char *p;
  size_t len, cap;
  int failed;
} buffer;

static void grow (buffer *b, size_t n)
{
  if (b->failed || b->len + n <= b->cap)
    return;
  {
    size_t cap = b->cap ? b->cap : 4096;
    unsigned char *p;
    while (cap < b->len + n)
      cap *= 2;
    if (!(p = realloc (b->p, cap)))
      {
        b->failed = 1;
        return;
      }
    b->p = p;
    b->cap = cap;
  }
}

static void append (buffer *b, const void *data, size_t n)
{
  grow (b, n);
  if (b->failed)
    return;
  if (data)
    memcpy (b->p + b->len, data, n);
  else
    memset (b->p + b->len, 0, n);
  b->len += n;
}

static void put16 (unsigned char *p, unsigned v) { p[0] = (unsigned char) v; p[1] = (unsigned char) (v >> 8); }
static void put32 (unsigned char *p, unsigned long v)
{
  p[0] = (unsigned char) v; p[1] = (unsigned char) (v >> 8); p[2] = (unsigned char) (v >> 16); p[3] = (unsigned char) (v >> 24);
}

static int by_tag (const void *a, const void *b)
{
  const entry *x = a, *y = b;
  return x->tag < y->tag ? -1 : x->tag > y->tag;
}

/* Writes the IFD (entries in tag order, as TIFF wants), its values and its
   sub-IFDs into b, whose first byte will be at offset base in the file.
   Returns the IFD's offset. */
static unsigned long put_node (buffer *b, node *n, unsigned long base)
{
  size_t dir;
  unsigned long at;
  int i;
  if (b->len & 1)
    append (b, NULL, 1);               /* offsets are even */
  dir = b->len;
  at = base + (unsigned long) dir;
  qsort (n->e, (size_t) n->n, sizeof n->e[0], by_tag);
  append (b, NULL, 2 + 12 * (size_t) n->n + 4);
  if (b->failed)
    return 0;
  put16 (b->p + dir, (unsigned) n->n);
  for (i = 0; i < n->n; i++)
    {
      entry *e = &n->e[i];
      size_t ent = dir + 2 + 12 * (size_t) i;
      unsigned long value;
      if (e->sub)
        {
          value = put_node (b, e->sub, base);
          if (b->failed)
            return 0;
          put16 (b->p + ent, e->tag);
          put16 (b->p + ent + 2, 4);
          put32 (b->p + ent + 4, 1);
          put32 (b->p + ent + 8, value);
          continue;
        }
      put16 (b->p + ent, e->tag);
      put16 (b->p + ent + 2, e->type);
      put32 (b->p + ent + 4, e->count);
      if (e->len <= 4)
        memcpy (b->p + ent + 8, e->data, e->len);
      else
        {
          if (b->len & 1)
            append (b, NULL, 1);
          value = base + (unsigned long) b->len;
          append (b, e->data, e->len);
          if (b->failed)
            return 0;
          put32 (b->p + ent + 8, value);
        }
    }
  return at;
}

/* Entries left out of the compact copy: where the picture is, and big
   blocks that aren't EXIF (the profile and XMP are kept separately). */
static int left_out (unsigned tag)
{
  return tag == TAG_STRIP_OFFSETS || tag == TAG_STRIP_BYTES || tag == TAG_TILE_OFFSETS || tag == TAG_TILE_BYTES
         || tag == TAG_SUBIFDS || tag == TAG_ICC || tag == TAG_XMP || tag == TAG_IPTC || tag == TAG_PHOTOSHOP
         || tag == TAG_IMAGE_SOURCE;
}

static void prune (node *n)
{
  int i, k = 0;
  for (i = 0; i < n->n; i++)
    {
      entry *e = &n->e[i];
      if (left_out (e->tag) || (!e->sub && e->len > COMPACT_VALUE))
        {
          free (e->data);
          free_node (e->sub);
          continue;
        }
      if (e->sub)
        prune (e->sub);
      n->e[k++] = *e;
    }
  n->n = k;
}

int tiff_signature (const unsigned char *buf, size_t len)
{
  return len >= 4 && ((buf[0] == 'I' && buf[1] == 'I' && buf[2] == 42 && buf[3] == 0)
                      || (buf[0] == 'M' && buf[1] == 'M' && buf[2] == 0 && buf[3] == 42));
}

int tiff_meta_read (FILE *f, tiff_meta *m)
{
  unsigned char h[8];
  source s;
  node *ifd0;
  memset (m, 0, sizeof *m);
  memset (&s, 0, sizeof s);
  s.f = f;
  if (src_read (&s, 0, h, 8) || !tiff_signature (h, 8))
    return -1;
  s.big = h[0] == 'M';
  ifd0 = read_ifd (&s, get32 (&s, h + 4), 0);
  if (!ifd0)
    return -1;
  m->width = (int) first_int (ifd0, TAG_WIDTH, 0);
  m->height = (int) first_int (ifd0, TAG_HEIGHT, 0);
  m->bits = (int) first_int (ifd0, TAG_BITS, 1);
  m->samples = (int) first_int (ifd0, TAG_SAMPLES, 1);
  m->compression = (int) first_int (ifd0, TAG_COMPRESSION, 1);
  m->photometric = (int) first_int (ifd0, TAG_PHOTOMETRIC, m->samples >= 3 ? 2 : 1);
  m->planar = (int) first_int (ifd0, TAG_PLANAR, 1);
  m->sample_format = (int) first_int (ifd0, TAG_SAMPLE_FORMAT, 1);
  m->orientation = (int) first_int (ifd0, TAG_ORIENTATION, 1);
  if (m->orientation < 1 || m->orientation > 8)
    m->orientation = 1;
  m->tiled = find (ifd0, TAG_TILE_OFFSETS) != NULL;
  m->raw = find (ifd0, 50706) != NULL || m->photometric == 32803 || m->photometric == 34892
           || ((first_int (ifd0, 254, 0) & 1) && find (ifd0, TAG_SUBIFDS) != NULL);
  m->icc = copy_value (ifd0, TAG_ICC, &m->icc_len);
  m->xmp = copy_value (ifd0, TAG_XMP, &m->xmp_len);
  prune (ifd0);
  if (ifd0->n)
    {
      buffer b = { NULL, 0, 0, 0 };
      unsigned char hdr[8] = { 'I', 'I', 42, 0, 8, 0, 0, 0 };
      append (&b, hdr, 8);
      put_node (&b, ifd0, 0);
      if (!b.failed)
        {
          m->exif = b.p;
          m->exif_len = b.len;
        }
      else
        free (b.p);
    }
  free_node (ifd0);
  if (m->width <= 0 || m->height <= 0)
    {
      tiff_meta_free (m);
      return -1;
    }
  return 0;
}

int tiff_is_raw (FILE *f)
{
  tiff_meta m;
  int raw;
  if (tiff_meta_read (f, &m))
    return 0;
  raw = m.raw;
  tiff_meta_free (&m);
  return raw;
}

void tiff_meta_free (tiff_meta *m)
{
  free (m->icc);
  free (m->xmp);
  free (m->exif);
  m->icc = m->xmp = m->exif = NULL;
  m->icc_len = m->xmp_len = m->exif_len = 0;
}

void tiff_encoding (const tiff_meta *m, char *out, size_t n)
{
  const char *c, *ph;
  char cbuf[24];
  switch (m->compression)
    {
    case 1: c = "uncompressed"; break;
    case 5: c = "LZW"; break;
    case 6: case 7: c = "JPEG"; break;
    case 8: case 32946: c = "Deflate"; break;
    case 32773: c = "PackBits"; break;
    case 34925: c = "LZMA"; break;
    case 50000: c = "Zstandard"; break;
    case 50001: c = "WebP"; break;
    default: snprintf (cbuf, sizeof cbuf, "compression %d", m->compression); c = cbuf;
    }
  switch (m->photometric)
    {
    case 0: case 1: ph = "grey"; break;
    case 2: ph = "RGB"; break;
    case 3: ph = "palette"; break;
    case 5: ph = "CMYK"; break;
    case 6: ph = "YCbCr"; break;
    case 8: case 9: ph = "Lab"; break;
    default: ph = "";
    }
  {
    int colours = m->photometric == 2 || m->photometric == 6 || m->photometric == 8 || m->photometric == 9 ? 3
                : m->photometric == 5 ? 4 : 1;
    snprintf (out, n, "%s, %d bit%s%s%s%s%s%s", c, m->bits, m->bits == 1 ? "" : "s",
              m->sample_format == 3 ? " floating point" : "", ph[0] ? ", " : "", ph,
              m->samples > colours ? " with alpha" : "", m->tiled ? ", in tiles" : "");
  }
}

/* --- writing a TIFF */

#define LZW_HASH 9001                   /* prime, > 2 x 4096 */
#define CODE_CLEAR 256
#define CODE_EOI 257
#define CODE_FIRST 258
#define CODE_MAX 4096

struct tiff_writer
{
  FILE *f;
  int w, h, rows_per_strip, nstrips, y;
  unsigned long *offsets, *counts;
  unsigned char *strip;                /* this strip's rows, predicted */
  size_t strip_used;
  buffer out;                          /* this strip compressed */
  unsigned long pos;                   /* where the file has got to */
  /* the LZW encoder */
  int hash_code[LZW_HASH];
  long hash_key[LZW_HASH];
  unsigned long bits;
  int nbits_held, nbits, free_ent;
  unsigned char *icc, *xmp, *exif;
  size_t icc_len, xmp_len, exif_len;
  char err[160];
  int failed;
};

static void lzw_put (tiff_writer *t, int code)
{
  t->bits = (t->bits << t->nbits) | (unsigned long) code;
  t->nbits_held += t->nbits;
  while (t->nbits_held >= 8)
    {
      unsigned char c = (unsigned char) (t->bits >> (t->nbits_held - 8));
      append (&t->out, &c, 1);
      t->nbits_held -= 8;
    }
  t->bits &= (1UL << t->nbits_held) - 1;
}

static void lzw_reset (tiff_writer *t)
{
  int i;
  for (i = 0; i < LZW_HASH; i++)
    t->hash_key[i] = -1;
  t->free_ent = CODE_FIRST;
  t->nbits = 9;
}

/* A new table entry has been made: wider codes, or a full table cleared
   (as libtiff does, so its "early change" reader agrees). */
static void lzw_added (tiff_writer *t)
{
  if (t->free_ent == CODE_MAX - 1)
    {
      lzw_put (t, CODE_CLEAR);
      lzw_reset (t);
    }
  else if (t->free_ent > (1 << t->nbits) - 1)
    t->nbits++;
}

/* Compresses n bytes into t->out (one strip: clear first, EOI last). */
static void lzw_strip (tiff_writer *t, const unsigned char *p, size_t n)
{
  int ent;
  size_t i;
  t->out.len = 0;
  t->bits = 0;
  t->nbits_held = 0;
  lzw_reset (t);
  lzw_put (t, CODE_CLEAR);
  if (!n)
    {
      lzw_put (t, CODE_EOI);
      return;
    }
  ent = p[0];
  for (i = 1; i < n; i++)
    {
      int c = p[i];
      long key = (long) ent << 8 | c;
      unsigned h = (unsigned) (key % LZW_HASH);
      while (t->hash_key[h] != -1 && t->hash_key[h] != key)
        h = h + 1 == LZW_HASH ? 0 : h + 1;
      if (t->hash_key[h] == key)
        {
          ent = t->hash_code[h];
          continue;
        }
      lzw_put (t, ent);
      ent = c;
      t->hash_key[h] = key;
      t->hash_code[h] = t->free_ent++;
      lzw_added (t);
    }
  lzw_put (t, ent);
  t->free_ent++;
  if (t->free_ent == CODE_MAX - 1)
    {
      lzw_put (t, CODE_CLEAR);
      t->nbits = 9;
    }
  else if (t->free_ent > (1 << t->nbits) - 1)
    t->nbits++;
  lzw_put (t, CODE_EOI);
  if (t->nbits_held > 0)
    {
      unsigned char c = (unsigned char) (t->bits << (8 - t->nbits_held));
      append (&t->out, &c, 1);
    }
}

static int fail (tiff_writer *t, const char *m)
{
  if (!t->failed)
    snprintf (t->err, sizeof t->err, "%s", m);
  t->failed = 1;
  return -1;
}

static int write_at_end (tiff_writer *t, const unsigned char *p, size_t n)
{
  if (fwrite (p, 1, n, t->f) != n)
    return fail (t, "Can't write the file (is the disc full?)");
  t->pos += (unsigned long) n;
  return 0;
}

tiff_writer *tiff_write_start (FILE *f, int w, int h, char *err, size_t errlen)
{
  tiff_writer *t = calloc (1, sizeof *t);
  static const unsigned char hdr[8] = { 'I', 'I', 42, 0, 0, 0, 0, 0 };
  if (!t || w <= 0 || h <= 0)
    {
      free (t);
      snprintf (err, errlen, "Not enough memory");
      return NULL;
    }
  t->f = f;
  t->w = w;
  t->h = h;
  t->rows_per_strip = (int) (65536 / ((long) w * 3));
  if (t->rows_per_strip < 1)
    t->rows_per_strip = 1;
  if (t->rows_per_strip > h)
    t->rows_per_strip = h;
  t->nstrips = (h + t->rows_per_strip - 1) / t->rows_per_strip;
  t->offsets = calloc ((size_t) t->nstrips, sizeof *t->offsets);
  t->counts = calloc ((size_t) t->nstrips, sizeof *t->counts);
  t->strip = malloc ((size_t) w * 3 * (size_t) t->rows_per_strip);
  if (!t->offsets || !t->counts || !t->strip)
    {
      tiff_write_free (t);
      snprintf (err, errlen, "Not enough memory");
      return NULL;
    }
  if (fwrite (hdr, 1, 8, f) != 8)
    {
      tiff_write_free (t);
      snprintf (err, errlen, "Can't write the file");
      return NULL;
    }
  t->pos = 8;
  return t;
}

static unsigned char *dup (const unsigned char *p, size_t n)
{
  unsigned char *q = p && n ? malloc (n) : NULL;
  if (q)
    memcpy (q, p, n);
  return q;
}

void tiff_write_extras (tiff_writer *t, const unsigned char *icc, size_t icc_len, const unsigned char *xmp,
                        size_t xmp_len, const unsigned char *exif, size_t exif_len)
{
  if ((t->icc = dup (icc, icc_len)))
    t->icc_len = icc_len;
  if ((t->xmp = dup (xmp, xmp_len)))
    t->xmp_len = xmp_len;
  if ((t->exif = dup (exif, exif_len)))
    t->exif_len = exif_len;
}

static int flush_strip (tiff_writer *t)
{
  int s = (t->y - 1) / t->rows_per_strip;
  if (t->strip_used == 0)
    return 0;
  lzw_strip (t, t->strip, t->strip_used);
  if (t->out.failed)
    return fail (t, "Not enough memory");
  if (t->pos & 1)
    {
      static const unsigned char z = 0;
      if (write_at_end (t, &z, 1))
        return -1;
    }
  t->offsets[s] = t->pos;
  t->counts[s] = (unsigned long) t->out.len;
  t->strip_used = 0;
  return write_at_end (t, t->out.p, t->out.len);
}

/* The horizontal predictor, 16 samples at a time with NEON. Returns how
   far it got (from 3); the plain loop does the rest. */
#if defined(__arm__)
#pragma GCC push_options
#pragma GCC target ("fpu=neon")
#include <arm_neon.h>
static int predict_neon (const unsigned char *rgb, unsigned char *row, int n)
{
  int x = 3;
  for (; x + 16 <= n; x += 16)
    vst1q_u8 (row + x, vsubq_u8 (vld1q_u8 (rgb + x), vld1q_u8 (rgb + x - 3)));
  return x;
}
#pragma GCC pop_options
#endif

int tiff_write_row (tiff_writer *t, const unsigned char *rgb)
{
  unsigned char *row;
  int x;
  if (t->failed || t->y >= t->h)
    return -1;
  row = t->strip + t->strip_used;
  /* The horizontal predictor: each sample less the one to its left. */
  row[0] = rgb[0];
  row[1] = rgb[1];
  row[2] = rgb[2];
  x = 3;
#if defined(__arm__)
  if (simd_neon ())
    x = predict_neon (rgb, row, t->w * 3);
#endif
  for (; x < t->w * 3; x++)
    row[x] = (unsigned char) (rgb[x] - rgb[x - 3]);
  t->strip_used += (size_t) t->w * 3;
  t->y++;
  if (t->y % t->rows_per_strip == 0 || t->y == t->h)
    return flush_strip (t);
  return 0;
}

static entry *set (node *n, unsigned tag, unsigned type, unsigned long count, const void *data, size_t len)
{
  entry *e;
  int i;
  for (i = 0; i < n->n; i++)
    if (n->e[i].tag == tag)
      {
        free (n->e[i].data);
        free_node (n->e[i].sub);
        n->e[i] = n->e[--n->n];
        break;
      }
  if (!(e = add_entry (n)) || !(e->data = malloc (len < 4 ? 4 : len)))
    {
      if (e)
        n->n--;
      return NULL;
    }
  memset (e->data, 0, len < 4 ? 4 : len);
  memcpy (e->data, data, len);
  e->tag = tag;
  e->type = type;
  e->count = count;
  e->len = len;
  return e;
}

static void set_short (node *n, unsigned tag, unsigned v)
{
  unsigned char b[2];
  put16 (b, v);
  set (n, tag, 3, 1, b, 2);
}

static void set_long (node *n, unsigned tag, unsigned long v)
{
  unsigned char b[4];
  put32 (b, v);
  set (n, tag, 4, 1, b, 4);
}

/* The tags of the EXIF block's IFD0 the file takes as its own (the camera,
   dates, software, artist and copyright), and its EXIF and GPS IFDs. */
static int taken_from_exif (unsigned tag)
{
  return tag == 270 || tag == 271 || tag == 272 || tag == 305 || tag == 306 || tag == 315 || tag == 33432
         || tag == TAG_EXIF_IFD || tag == TAG_GPS_IFD;
}

int tiff_write_finish (tiff_writer *t)
{
  node *n;
  buffer b = { NULL, 0, 0, 0 };
  unsigned char *arr;
  unsigned long ifd;
  int i;
  static const unsigned char res[8] = { 72, 0, 0, 0, 1, 0, 0, 0 };
  unsigned char bits[6];
  if (t->failed)
    return -1;
  if (t->y != t->h)
    return fail (t, "Not all of the picture was written");
  if (!(n = calloc (1, sizeof *n)))
    return fail (t, "Not enough memory");
  if (t->exif && t->exif_len >= 8 && tiff_signature (t->exif, t->exif_len))
    {
      source s = { NULL, t->exif, t->exif_len, t->exif[0] == 'M' };
      node *e = read_ifd (&s, get32 (&s, t->exif + 4), 0);
      if (e)
        {
          for (i = 0; i < e->n; i++)
            if (taken_from_exif (e->e[i].tag))
              {
                entry *x = add_entry (n);
                if (x)
                  {
                    *x = e->e[i];
                    e->e[i].data = NULL;
                    e->e[i].sub = NULL;
                  }
              }
          free_node (e);
        }
    }
  set_long (n, TAG_WIDTH, (unsigned long) t->w);
  set_long (n, TAG_HEIGHT, (unsigned long) t->h);
  put16 (bits, 8); put16 (bits + 2, 8); put16 (bits + 4, 8);
  set (n, TAG_BITS, 3, 3, bits, 6);
  set_short (n, TAG_COMPRESSION, 5);
  set_short (n, TAG_PHOTOMETRIC, 2);
  set_short (n, TAG_ORIENTATION, 1);
  set_short (n, TAG_SAMPLES, 3);
  set_long (n, TAG_ROWS, (unsigned long) t->rows_per_strip);
  set (n, TAG_XRES, 5, 1, res, 8);
  set (n, TAG_YRES, 5, 1, res, 8);
  set_short (n, TAG_PLANAR, 1);
  set_short (n, TAG_RES_UNIT, 2);
  set_short (n, TAG_PREDICTOR, 2);
  arr = malloc ((size_t) t->nstrips * 4);
  if (!arr)
    {
      free_node (n);
      return fail (t, "Not enough memory");
    }
  for (i = 0; i < t->nstrips; i++)
    put32 (arr + 4 * i, t->offsets[i]);
  set (n, TAG_STRIP_OFFSETS, 4, (unsigned long) t->nstrips, arr, (size_t) t->nstrips * 4);
  for (i = 0; i < t->nstrips; i++)
    put32 (arr + 4 * i, t->counts[i]);
  set (n, TAG_STRIP_BYTES, 4, (unsigned long) t->nstrips, arr, (size_t) t->nstrips * 4);
  free (arr);
  if (t->icc)
    set (n, TAG_ICC, 7, (unsigned long) t->icc_len, t->icc, t->icc_len);
  if (t->xmp)
    set (n, TAG_XMP, 1, (unsigned long) t->xmp_len, t->xmp, t->xmp_len);
  if (t->pos & 1)
    {
      static const unsigned char z = 0;
      if (write_at_end (t, &z, 1))
        {
          free_node (n);
          return -1;
        }
    }
  ifd = put_node (&b, n, t->pos);
  free_node (n);
  if (b.failed)
    {
      free (b.p);
      return fail (t, "Not enough memory");
    }
  if (write_at_end (t, b.p, b.len))
    {
      free (b.p);
      return -1;
    }
  free (b.p);
  {
    unsigned char o[4];
    put32 (o, ifd);
    if (fseek (t->f, 4, SEEK_SET) != 0 || fwrite (o, 1, 4, t->f) != 4)
      return fail (t, "Can't write the file");
  }
  return 0;
}

const char *tiff_write_error (const tiff_writer *t)
{
  return t->err;
}

void tiff_write_free (tiff_writer *t)
{
  if (!t)
    return;
  free (t->offsets);
  free (t->counts);
  free (t->strip);
  free (t->out.p);
  free (t->icc);
  free (t->xmp);
  free (t->exif);
  free (t);
}
