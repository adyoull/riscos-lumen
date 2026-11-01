/* exif.c: see exif.h. TIFF/EXIF layout from the EXIF 2.32 specification:
   a TIFF header ("II" or "MM", 42, offset of IFD0), IFDs of 12-byte
   entries (tag, type, count, value or offset), IFD0 linking to the EXIF
   IFD (tag &8769) and to IFD1, which holds the thumbnail's offset and
   length (tags &201, &202). */
#include <string.h>
#include "exif.h"

typedef struct
{
  const unsigned char *p;    /* TIFF header */
  size_t len;                /* bytes from p to the end of the buffer */
  int big;                   /* "MM": big-endian */
} tiff;

static unsigned rd16 (const tiff *t, size_t off)
{
  if (off + 2 > t->len)
    return 0;
  return t->big ? (unsigned) (t->p[off] << 8 | t->p[off + 1])
                : (unsigned) (t->p[off] | t->p[off + 1] << 8);
}

static unsigned long rd32 (const tiff *t, size_t off)
{
  if (off + 4 > t->len)
    return 0;
  if (t->big)
    return (unsigned long) t->p[off] << 24 | (unsigned long) t->p[off + 1] << 16
         | (unsigned long) t->p[off + 2] << 8 | t->p[off + 3];
  return (unsigned long) t->p[off] | (unsigned long) t->p[off + 1] << 8
       | (unsigned long) t->p[off + 2] << 16 | (unsigned long) t->p[off + 3] << 24;
}

static const size_t type_size[] = { 0, 1, 1, 2, 4, 8, 1, 1, 2, 4, 8, 4, 8 };

/* Where an entry's value starts: inline if it fits in 4 bytes. */
static size_t value_at (const tiff *t, size_t entry, unsigned type, unsigned long count)
{
  size_t sz = (type < sizeof type_size / sizeof type_size[0]) ? type_size[type] : 0;
  if (sz == 0 || count > 0x10000)
    return (size_t) -1;
  if (sz * count <= 4)
    return entry + 8;
  return (size_t) rd32 (t, entry + 8);
}

static unsigned long int_value (const tiff *t, size_t entry, unsigned type, unsigned long count)
{
  size_t at = value_at (t, entry, type, count);
  if (at == (size_t) -1)
    return 0;
  return type == 3 ? rd16 (t, at) : (type == 4 || type == 9) ? rd32 (t, at) : 0;
}

static double rational (const tiff *t, size_t entry, unsigned type, unsigned long count)
{
  size_t at = value_at (t, entry, type, count);
  unsigned long n, d;
  if (at == (size_t) -1 || (type != 5 && type != 10))
    return type == 3 || type == 4 ? (double) int_value (t, entry, type, count) : 0;
  n = rd32 (t, at);
  d = rd32 (t, at + 4);
  if (d == 0)
    return 0;
  if (type == 10)
    return (double) (long) n / (double) (long) d;
  return (double) n / (double) d;
}

static void ascii (const tiff *t, size_t entry, unsigned type, unsigned long count, char *out, size_t n)
{
  size_t at = value_at (t, entry, type, count), i;
  out[0] = '\0';
  if (type != 2 || at == (size_t) -1 || at >= t->len || n == 0)
    return;
  for (i = 0; i < count && i + 1 < n && at + i < t->len && t->p[at + i]; i++)
    out[i] = (char) t->p[at + i];
  out[i] = '\0';
  while (i > 0 && out[i - 1] == ' ')           /* cameras pad with spaces */
    out[--i] = '\0';
}

/* Walks one IFD. Returns the offset of the next IFD (0 if none). */
static unsigned long walk (const tiff *t, size_t ifd, int which, img_info *info,
                           unsigned long *exif_ifd, long base)
{
  unsigned n, i;
  if (ifd < 8 || ifd + 2 > t->len)
    return 0;
  n = rd16 (t, ifd);
  if (n > 500 || ifd + 2 + 12 * (size_t) n + 4 > t->len)
    return 0;
  for (i = 0; i < n; i++)
    {
      size_t e = ifd + 2 + 12 * (size_t) i;
      unsigned tag = rd16 (t, e), type = rd16 (t, e + 2);
      unsigned long count = rd32 (t, e + 4);
      if (which == 0)                           /* IFD0 */
        switch (tag)
          {
          case 0x0112: info->orientation = (int) int_value (t, e, type, count); break;
          case 0x010F: ascii (t, e, type, count, info->make, sizeof info->make); break;
          case 0x0110: ascii (t, e, type, count, info->model, sizeof info->model); break;
          case 0x8769: *exif_ifd = int_value (t, e, type, count); break;
          case 0x0132: if (!info->taken[0]) ascii (t, e, type, count, info->taken, sizeof info->taken); break;
          }
      else if (which == 1)                      /* IFD1: the thumbnail */
        switch (tag)
          {
          case 0x0201: info->thumb_offset = (long) int_value (t, e, type, count) + base; break;
          case 0x0202: info->thumb_length = (long) int_value (t, e, type, count); break;
          }
      else                                      /* the EXIF IFD */
        switch (tag)
          {
          case 0x9003: ascii (t, e, type, count, info->taken, sizeof info->taken); break;
          case 0x829A: info->exposure = rational (t, e, type, count); break;
          case 0x829D: info->fnumber = rational (t, e, type, count); break;
          case 0x8827: info->iso = (int) int_value (t, e, type, count); break;
          case 0x920A: info->focal = rational (t, e, type, count); break;
          }
    }
  return rd32 (t, ifd + 2 + 12 * (size_t) n);
}

static void parse_exif (const unsigned char *p, size_t len, long base, img_info *info)
{
  tiff t;
  unsigned long ifd0, ifd1, exif_ifd = 0;
  char taken0[20];
  if (len < 8)
    return;
  t.p = p;
  t.len = len;
  if (p[0] == 'I' && p[1] == 'I')
    t.big = 0;
  else if (p[0] == 'M' && p[1] == 'M')
    t.big = 1;
  else
    return;
  if (rd16 (&t, 2) != 42)
    return;
  ifd0 = rd32 (&t, 4);
  ifd1 = walk (&t, ifd0, 0, info, &exif_ifd, base);
  /* DateTimeOriginal (EXIF IFD) wins over DateTime (IFD0). */
  memcpy (taken0, info->taken, sizeof taken0);
  info->taken[0] = '\0';
  if (exif_ifd)
    walk (&t, exif_ifd, 2, info, &exif_ifd, base);
  if (!info->taken[0])
    memcpy (info->taken, taken0, sizeof taken0);
  if (ifd1)
    walk (&t, ifd1, 1, info, &exif_ifd, base);
  if (info->orientation < 1 || info->orientation > 8)
    info->orientation = 1;
  if (info->thumb_offset <= 0 || info->thumb_length <= 0)
    info->thumb_offset = info->thumb_length = 0;
}

int img_probe (const unsigned char *buf, size_t len)
{
  static const unsigned char png_sig[8] = { 0x89, 'P', 'N', 'G', 0x0D, 0x0A, 0x1A, 0x0A };
  if (len >= 3 && buf[0] == 0xFF && buf[1] == 0xD8 && buf[2] == 0xFF)
    return IMG_JPEG;
  if (len >= 8 && memcmp (buf, png_sig, 8) == 0)
    return IMG_PNG;
  return IMG_UNKNOWN;
}

static int read_jpeg (const unsigned char *buf, size_t len, img_info *info)
{
  size_t pos = 2;
  while (pos + 4 <= len)
    {
      unsigned marker, seglen;
      if (buf[pos] != 0xFF)
        return -1;
      marker = buf[pos + 1];
      if (marker == 0xFF)                       /* fill byte */
        {
          pos++;
          continue;
        }
      if (marker == 0xD8 || (marker >= 0xD0 && marker <= 0xD7) || marker == 0x01)
        {
          pos += 2;
          continue;
        }
      seglen = (unsigned) (buf[pos + 2] << 8 | buf[pos + 3]);
      if (seglen < 2)
        return -1;
      if (marker == 0xE1 && pos + 10 <= len && memcmp (buf + pos + 4, "Exif\0\0", 6) == 0)
        {
          size_t start = pos + 10, end = pos + 2 + seglen;
          if (end > len)
            end = len;
          parse_exif (buf + start, end - start, (long) start, info);
        }
      /* Start of frame: SOF0-3, 5-7, 9-11, 13-15 (not C4 DHT, C8 JPG, CC DAC). */
      if (marker >= 0xC0 && marker <= 0xCF && marker != 0xC4 && marker != 0xC8 && marker != 0xCC)
        {
          if (pos + 9 > len)
            return -1;
          info->height = buf[pos + 5] << 8 | buf[pos + 6];
          info->width = buf[pos + 7] << 8 | buf[pos + 8];
          return (info->width > 0 && info->height > 0) ? 0 : -1;
        }
      if (marker == 0xDA || marker == 0xD9)     /* scan data before a frame header: give up */
        return -1;
      pos += 2 + seglen;
    }
  return -1;
}

int img_read_info (const unsigned char *buf, size_t len, img_info *info)
{
  memset (info, 0, sizeof *info);
  info->orientation = 1;
  info->format = img_probe (buf, len);
  if (info->format == IMG_JPEG)
    return read_jpeg (buf, len, info);
  if (info->format == IMG_PNG)
    {
      /* IHDR is the first chunk: length(4) "IHDR" width(4) height(4), big-endian. */
      if (len < 24 || memcmp (buf + 12, "IHDR", 4) != 0)
        return -1;
      info->width = (int) ((unsigned long) buf[16] << 24 | (unsigned long) buf[17] << 16 | buf[18] << 8 | buf[19]);
      info->height = (int) ((unsigned long) buf[20] << 24 | (unsigned long) buf[21] << 16 | buf[22] << 8 | buf[23]);
      return (info->width > 0 && info->height > 0) ? 0 : -1;
    }
  return -1;
}
