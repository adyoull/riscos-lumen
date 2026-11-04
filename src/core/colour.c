/* Copyright (c) 2026 Andrew Youll
   Released under the MIT licence: see the LICENSE file.
   SPDX-License-Identifier: MIT */
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

/* --- out of sRGB, for exports */
static const double mats_out[3][9] = {
  { 1, 0, 0, 0, 1, 0, 0, 0, 1 },
  /* linear sRGB to Adobe RGB (1998) (the inverse of the matrix above) */
  { 0.7151627, 0.2848373, 0.0, 0.0, 1.0, 0.0, 0.0, 0.0411705, 0.9588295 },
  /* linear sRGB to Display P3 */
  { 0.8224621, 0.1775380, 0.0, 0.0331941, 0.9668058, 0.0, 0.0170827, 0.0723974, 0.9105199 },
};

typedef struct { int ready; uint16_t lin[256]; int m[9]; uint8_t enc[4096]; } conv_out;
static conv_out outs[3];

static const conv_out *get_out (int cs)
{
  conv_out *c = &outs[cs];
  int i;
  if (c->ready)
    return c;
  for (i = 0; i < 256; i++)
    {
      double v = i / 255.0;
      c->lin[i] = (uint16_t) lround ((v <= 0.04045 ? v / 12.92 : pow ((v + 0.055) / 1.055, 2.4)) * 65535);
    }
  for (i = 0; i < 9; i++)
    c->m[i] = (int) lround (mats_out[cs][i] * 4096);
  for (i = 0; i < 4096; i++)
    {
      double l = (i + 0.5) / 4096.0, v;
      if (cs == CS_ADOBE)
        v = pow (l, 256.0 / 563.0);
      else
        v = l <= 0.0031308 ? l * 12.92 : 1.055 * pow (l, 1 / 2.4) - 0.055;
      c->enc[i] = (uint8_t) lround (v * 255);
    }
  c->enc[0] = 0;
  c->ready = 1;
  return c;
}

void colour_row_from_srgb (int cs, unsigned char *rgb, int w)
{
  const conv_out *c;
  int x;
  if (cs != CS_ADOBE && cs != CS_P3)
    return;
  c = get_out (cs);
  for (x = 0; x < w; x++, rgb += 3)
    {
      int R = c->lin[rgb[0]], G = c->lin[rgb[1]], B = c->lin[rgb[2]], k, o[3];
      for (k = 0; k < 3; k++)
        {
          int v = (c->m[3 * k] * R + c->m[3 * k + 1] * G + c->m[3 * k + 2] * B) >> 16;   /* 0-4095 */
          o[k] = v < 0 ? 0 : v > 4095 ? 4095 : v;
        }
      rgb[0] = c->enc[o[0]]; rgb[1] = c->enc[o[1]]; rgb[2] = c->enc[o[2]];
    }
}

/* --- ICC profiles for exports (ICC.1:2001-04, version 2.1, so every
   reader takes them): a display-class RGB profile with the D50-adapted
   primaries, the white point, and the transfer curve (Adobe RGB's as a
   gamma, P3's as sRGB's curve in 1024 steps), described as Lumen's own
   detection expects. */
static void be32w (unsigned char *p, unsigned long v)
{
  p[0] = (unsigned char) (v >> 24); p[1] = (unsigned char) (v >> 16);
  p[2] = (unsigned char) (v >> 8); p[3] = (unsigned char) v;
}

static void s15 (unsigned char *p, double v)
{
  be32w (p, (unsigned long) (long) lround (v * 65536.0));
}

size_t colour_icc_profile (int cs, unsigned char *out, size_t cap)
{
  static const double prim[3][9] = {
    { 0.4360747, 0.2225045, 0.0139322, 0.3850649, 0.7168786, 0.0971045, 0.1430804, 0.0606169, 0.7141733 },
    { 0.6097559, 0.3111242, 0.0194811, 0.2052401, 0.6256560, 0.0608902, 0.1492240, 0.0632197, 0.7448387 },
    { 0.5151022, 0.2411819, -0.0010491, 0.2919651, 0.6922360, 0.0418818, 0.1571533, 0.0665822, 0.7843782 },
  };
  const char *desc = cs == CS_ADOBE ? "Adobe RGB (1998)" : cs == CS_P3 ? "Display P3" : "sRGB IEC61966-2.1";
  const char *cprt = "No copyright, use freely";
  int ntags = 9, i, k, parametric = cs == CS_ADOBE;
  size_t p, desc_at, desc_len, cprt_at, cprt_len, xyz_at[4], trc_at, trc_len, total;
  if (cs != CS_ADOBE && cs != CS_P3 && cs != CS_SRGB)
    return 0;
  /* Layout: header, tag table, then the data, each piece 4-byte aligned. */
  p = 128 + 4 + 12 * (size_t) ntags;
  desc_at = p; desc_len = 12 + strlen (desc) + 1 + 4 + 4 + 2 + 1 + 67; p += (desc_len + 3) & ~(size_t) 3;
  cprt_at = p; cprt_len = 8 + strlen (cprt) + 1; p += (cprt_len + 3) & ~(size_t) 3;
  for (k = 0; k < 4; k++)
    {
      xyz_at[k] = p;
      p += 20;
    }
  trc_at = p; trc_len = parametric ? 14 : 12 + 2 * 1024; p += (trc_len + 3) & ~(size_t) 3;
  total = p;
  if (total > cap)
    return 0;
  memset (out, 0, total);
  /* Header */
  be32w (out, (unsigned long) total);
  memcpy (out + 4, "lcms", 4);                 /* preferred CMM: none in particular */
  be32w (out + 8, 0x02100000);                 /* version 2.1 */
  memcpy (out + 12, "mntr", 4);
  memcpy (out + 16, "RGB ", 4);
  memcpy (out + 20, "XYZ ", 4);
  out[24] = 0x07; out[25] = 0xEA; out[27] = 1; out[29] = 1;   /* 2026-01-01 */
  memcpy (out + 36, "acsp", 4);
  s15 (out + 68, 0.9642); s15 (out + 72, 1.0); s15 (out + 76, 0.8249);   /* PCS illuminant, D50 */
  /* Tag table */
  be32w (out + 128, (unsigned long) ntags);
  {
    static const char sig[9][5] = { "desc", "cprt", "wtpt", "rXYZ", "gXYZ", "bXYZ", "rTRC", "gTRC", "bTRC" };
    size_t at[9], len[9];
    at[0] = desc_at; len[0] = desc_len;
    at[1] = cprt_at; len[1] = cprt_len;
    for (k = 0; k < 4; k++)
      {
        at[2 + k] = xyz_at[k];
        len[2 + k] = 20;
      }
    for (k = 6; k < 9; k++)
      {
        at[k] = trc_at;                        /* the three curves share one */
        len[k] = trc_len;
      }
    for (i = 0; i < ntags; i++)
      {
        memcpy (out + 132 + 12 * i, sig[i], 4);
        be32w (out + 136 + 12 * i, (unsigned long) at[i]);
        be32w (out + 140 + 12 * i, (unsigned long) len[i]);
      }
  }
  /* desc: textDescriptionType (ASCII, then empty Unicode and ScriptCode parts) */
  memcpy (out + desc_at, "desc", 4);
  be32w (out + desc_at + 8, (unsigned long) strlen (desc) + 1);
  memcpy (out + desc_at + 12, desc, strlen (desc));
  /* cprt: textType */
  memcpy (out + cprt_at, "text", 4);
  memcpy (out + cprt_at + 8, cprt, strlen (cprt));
  /* wtpt (D50, as the PCS) and the colorants */
  for (k = 0; k < 4; k++)
    {
      unsigned char *q = out + xyz_at[k];
      memcpy (q, "XYZ ", 4);
      if (k == 0)
        {
          s15 (q + 8, 0.9642); s15 (q + 12, 1.0); s15 (q + 16, 0.8249);
        }
      else
        {
          s15 (q + 8, prim[cs][3 * (k - 1)]);
          s15 (q + 12, prim[cs][3 * (k - 1) + 1]);
          s15 (q + 16, prim[cs][3 * (k - 1) + 2]);
        }
    }
  /* TRC: curveType */
  memcpy (out + trc_at, "curv", 4);
  if (parametric)
    {
      be32w (out + trc_at + 8, 1);
      out[trc_at + 12] = 2; out[trc_at + 13] = 0x33;   /* u8Fixed8 2.19921875 = 563/256 */
    }
  else
    {
      be32w (out + trc_at + 8, 1024);
      for (i = 0; i < 1024; i++)
        {
          double v = i / 1023.0, l = v <= 0.04045 ? v / 12.92 : pow ((v + 0.055) / 1.055, 2.4);
          unsigned long q = (unsigned long) lround (l * 65535);
          out[trc_at + 12 + 2 * i] = (unsigned char) (q >> 8);
          out[trc_at + 13 + 2 * i] = (unsigned char) q;
        }
    }
  return total;
}
