/* colour.c: see colour.h. ICC layout from ICC.1:2010 (header 128 bytes,
   then the tag table: count, then signature/offset/size for each; 'desc'
   is a textDescriptionType in v2 profiles, multiLocalizedUnicodeType 'mluc'
   in v4). Matrices: each space's RGB to XYZ (D65), times XYZ to linear
   sRGB, from the published primaries. */
#include <math.h>
#include <stdint.h>
#include <string.h>
#include "colour.h"
#include "develop.h"
#include "exif.h"

static unsigned long be32 (const unsigned char *p)
{
  return (unsigned long) p[0] << 24 | (unsigned long) p[1] << 16 | (unsigned long) p[2] << 8 | p[3];
}

static int contains (const char *s, const char *word)
{
  size_t n = strlen (word), i, k;
  for (i = 0; s[i]; i++)
    {
      for (k = 0; k < n && s[i + k]; k++)
        {
          char a = s[i + k], b = word[k];
          if (a >= 'A' && a <= 'Z') a = (char) (a + 32);
          if (b >= 'A' && b <= 'Z') b = (char) (b + 32);
          if (a != b)
            break;
        }
      if (k == n)
        return 1;
    }
  return 0;
}

static int classify (const char *name)
{
  if (contains (name, "adobe rgb") || contains (name, "adobergb"))
    return CS_ADOBE;
  if (contains (name, "display p3"))
    return CS_P3;
  if (contains (name, "srgb") || contains (name, "iec61966-2.1") || contains (name, "iec 61966-2-1"))
    return CS_SRGB;
  return CS_OTHER;
}

int colour_from_icc (const unsigned char *icc, size_t len, char *name, size_t n)
{
  unsigned long count, i;
  char buf[128];
  buf[0] = '\0';
  if (name && n)
    name[0] = '\0';
  if (!icc || len < 132 || memcmp (icc + 36, "acsp", 4) != 0)
    return CS_OTHER;
  count = be32 (icc + 128);
  for (i = 0; i < count && i < 200 && 132 + 12 * (i + 1) <= len; i++)
    {
      const unsigned char *e = icc + 132 + 12 * i;
      unsigned long off = be32 (e + 4), size = be32 (e + 8);
      if (memcmp (e, "desc", 4) != 0 || off >= len || size < 12 || off + size > len)
        continue;
      if (memcmp (icc + off, "desc", 4) == 0)
        {
          /* v2: an ASCII count (with its terminator), then the text. */
          unsigned long k, c = be32 (icc + off + 8);
          for (k = 0; k < c && k + 1 < sizeof buf && off + 12 + k < len && icc[off + 12 + k]; k++)
            buf[k] = (char) icc[off + 12 + k];
          buf[k] = '\0';
        }
      else if (memcmp (icc + off, "mluc", 4) == 0 && size >= 28)
        {
          /* v4: the first record's UTF-16 big-endian text, kept as Latin-1. */
          unsigned long rl = be32 (icc + off + 20), ro = be32 (icc + off + 24), k, o = 0;
          for (k = 0; k + 1 < rl && off + ro + k + 1 < len && o + 1 < sizeof buf; k += 2)
            {
              unsigned ch = (unsigned) icc[off + ro + k] << 8 | icc[off + ro + k + 1];
              if (!ch)
                break;
              buf[o++] = (char) (ch < 256 ? ch : '?');
            }
          buf[o] = '\0';
        }
      break;
    }
  if (name && n)
    {
      strncpy (name, buf, n - 1);
      name[n - 1] = '\0';
    }
  return classify (buf);
}

typedef struct { int uncalibrated, r03, two; } exif_cs;

static void exif_tag (void *ctx, const char *group, const char *tag, const char *value)
{
  exif_cs *c = ctx;
  (void) group;
  if (strcmp (tag, "Colour space") == 0)
    {
      c->uncalibrated = strcmp (value, "Uncalibrated") == 0;
      c->two = strcmp (value, "2") == 0;   /* some cameras' own mark for Adobe RGB */
    }
  else if (strcmp (tag, "Interoperability index") == 0)
    c->r03 = strcmp (value, "R03") == 0;
}

int colour_from_exif (const unsigned char *tiff, size_t len)
{
  exif_cs c = { 0, 0, 0 };
  if (exif_list_tiff (tiff, len, exif_tag, &c) < 0)
    return CS_SRGB;
  return (c.uncalibrated && c.r03) || c.two ? CS_ADOBE : CS_SRGB;
}

int colour_from_jpeg (const unsigned char *buf, size_t len, char *name, size_t n)
{
  size_t pos = 2;
  int from_exif = -1;
  if (name && n)
    name[0] = '\0';
  if (len < 4 || buf[0] != 0xFF || buf[1] != 0xD8)
    return CS_SRGB;
  while (pos + 4 <= len && buf[pos] == 0xFF)
    {
      unsigned marker = buf[pos + 1], seg;
      size_t start, end;
      if (marker == 0xFF) { pos++; continue; }
      if (marker == 0xD8 || (marker >= 0xD0 && marker <= 0xD7) || marker == 0x01) { pos += 2; continue; }
      if (marker == 0xDA || marker == 0xD9)
        break;
      seg = (unsigned) (buf[pos + 2] << 8 | buf[pos + 3]);
      if (seg < 2)
        break;
      start = pos + 4;
      end = pos + 2 + seg > len ? len : pos + 2 + seg;
      if (marker == 0xE2 && end - start > 14 && memcmp (buf + start, "ICC_PROFILE\0", 12) == 0)
        return colour_from_icc (buf + start + 14, end - start - 14, name, n);
      if (marker == 0xE1 && end - start > 6 && memcmp (buf + start, "Exif\0\0", 6) == 0)
        from_exif = colour_from_exif (buf + start + 6, end - start - 6);
      pos += 2 + seg;
    }
  return from_exif >= 0 ? from_exif : CS_SRGB;
}

const char *colour_name (int cs)
{
  switch (cs)
    {
    case CS_ADOBE: return "Adobe RGB (1998)";
    case CS_P3: return "Display P3";
    case CS_SRGB: return "sRGB";
    }
  return "Other";
}

/* --- conversion */
static const double mats[3][9] = {
  { 1, 0, 0, 0, 1, 0, 0, 0, 1 },
  /* Adobe RGB (1998) to sRGB, linear */
  { 1.3983557, -0.3983557, 0.0, 0.0, 1.0, 0.0, 0.0, -0.0429289, 1.0429289 },
  /* Display P3 to sRGB, linear */
  { 1.2249401, -0.2249404, 0.0, -0.0420569, 1.0420571, 0.0, -0.0196376, -0.0786361, 1.0982735 },
};

typedef struct { int ready; uint16_t lin[256]; int m[9]; } conv;
static conv convs[3];

static const conv *get_conv (int cs)
{
  conv *c = &convs[cs];
  int i;
  if (c->ready)
    return c;
  for (i = 0; i < 256; i++)
    {
      double v = i / 255.0, l;
      if (cs == CS_ADOBE)
        l = pow (v, 563.0 / 256.0);    /* Adobe RGB's gamma, 2.19921875 */
      else
        l = v <= 0.04045 ? v / 12.92 : pow ((v + 0.055) / 1.055, 2.4);   /* P3 uses sRGB's curve */
      c->lin[i] = (uint16_t) lround (l * 65535);
    }
  for (i = 0; i < 9; i++)
    c->m[i] = (int) lround (mats[cs][i] * 4096);
  c->ready = 1;
  return c;
}

void colour_row_to_srgb (int cs, unsigned char *rgb, int w)
{
  const conv *c;
  const uint8_t *l2s;
  int x;
  if (cs != CS_ADOBE && cs != CS_P3)
    return;
  c = get_conv (cs);
  l2s = dev_linear_to_srgb ();
  for (x = 0; x < w; x++, rgb += 3)
    {
      int R = c->lin[rgb[0]], G = c->lin[rgb[1]], B = c->lin[rgb[2]], k;
      int o[3];
      for (k = 0; k < 3; k++)
        {
          int v = (c->m[3 * k] * R + c->m[3 * k + 1] * G + c->m[3 * k + 2] * B) >> 12;
          o[k] = v < 0 ? 0 : v > 65535 ? 65535 : v;
        }
      rgb[0] = l2s[o[0]]; rgb[1] = l2s[o[1]]; rgb[2] = l2s[o[2]];
    }
}
