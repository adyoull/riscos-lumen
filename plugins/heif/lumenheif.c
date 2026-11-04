/* Copyright (c) 2026 Andrew Youll
   Released under the MIT licence: see the LICENSE file.
   SPDX-License-Identifier: MIT */
/* LumenHEIF: Lumen's HEIF plug-in. A small program built on libheif (with
   FFmpeg's HEVC decoder) that Lumen runs in the background (in a task
   window) for each HEIF photo (.heic, .heif: what phones save), just as it
   runs LumenRaw for camera raw files:

     LumenHEIF thumb  SRC IMAGE INFO          the photo's own thumbnail
     LumenHEIF decode SRC IMAGE INFO MAXSIDE  the photo, MAXSIDE at most

   IMAGE is a binary PPM (P6, 8 bits, sRGB). INFO is text, one "key=value"
   a line: the photo's size, orientation (always 1: libheif turns and
   mirrors the picture as the file says), camera, exposure and date from
   its EXIF; or "error=..." if the file couldn't be read. Both are written
   under a temporary name and renamed when complete, INFO first, so Lumen
   sees either nothing or a whole file.

   Colour: a Display P3 photo (an iPhone's) is converted to sRGB here, in
   linear light, so Lumen gets sRGB as it does from LumenRaw.

   NEON: libavcodec's HEVC code uses NEON only when the processor has it
   (VFPSupport_Features on RISC OS); Lumen$NEON (LUMEN_NEON elsewhere) 0
   forces the plain code, 1 NEON.

   libheif and FFmpeg are used under the GNU LGPL (see Licence); their
   sources, with the changes made for RISC OS, are published with each
   release. */
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <libheif/heif.h>
#include <libavutil/cpu.h>
#include "exif.h"
#ifdef __riscos
#include <kernel.h>
#include <unixlib/local.h>
int __riscosify_control = __RISCOSIFY_NO_PROCESS;   /* paths are RISC OS paths, unchanged */
#else
#include <sys/auxv.h>
#endif

#define VERSION "1.0"
#define THUMB_MAX 512                  /* a thumbnail made from the photo itself: this at most */

/* --- writing results --- */

static void tmpname (char *out, size_t n, const char *path)
{
  snprintf (out, n, "%s_t", path);
}

static int finish (const char *tmp, const char *path)
{
  remove (path);
  return rename (tmp, path);
}

static int write_info (const char *path, const char *text)
{
  char tmp[1100];
  FILE *f;
  tmpname (tmp, sizeof tmp, path);
  f = fopen (tmp, "w");
  if (!f)
    return -1;
  fputs (text, f);
  if (fclose (f))
    return -1;
  return finish (tmp, path);
}

static int fail (const char *info, const char *what, const char *why)
{
  char t[400];
  snprintf (t, sizeof t, "error=%s%s%s\n", what, why && *why ? ": " : "", why ? why : "");
  write_info (info, t);
  return 1;
}

static int write_ppm (const char *path, const unsigned char *rgb, int w, int h)
{
  char tmp[1100];
  FILE *f;
  tmpname (tmp, sizeof tmp, path);
  f = fopen (tmp, "wb");
  if (!f)
    return -1;
  fprintf (f, "P6\n%d %d\n255\n", w, h);
  if (fwrite (rgb, (size_t) w * 3, (size_t) h, f) != (size_t) h)
    {
      fclose (f);
      return -1;
    }
  if (fclose (f))
    return -1;
  return finish (tmp, path);
}

/* --- NEON --- */

static int has_neon (void)
{
  const char *v;
#ifdef __riscos
  _kernel_swi_regs r;
  v = getenv ("Lumen$NEON");
  if (v && (*v == '0' || *v == '1'))
    return *v == '1';
  r.r[0] = 0;
  /* VFPSupport_Features 0: R1 = MVFR0, R2 = MVFR1 (PRM p706) */
  return _kernel_swi (0x58EC8, &r, &r) == NULL && (r.r[1] & 0xF) == 2 && ((r.r[2] >> 8) & 0xF) != 0;
#else
  v = getenv ("LUMEN_NEON");
  if (v && (*v == '0' || *v == '1'))
    return *v == '1';
#if defined(__arm__)
  return (getauxval (AT_HWCAP) & (1 << 12)) != 0;   /* HWCAP_NEON */
#else
  return 0;
#endif
#endif
}

/* --- colour: Display P3 to sRGB --- */

static int is_p3 (const struct heif_image_handle *h)
{
  enum heif_color_profile_type t = heif_image_handle_get_color_profile_type (h);
  if (t == heif_color_profile_type_nclx)
    {
      struct heif_color_profile_nclx *n = NULL;
      int p3 = 0;
      if (heif_image_handle_get_nclx_color_profile (h, &n).code == heif_error_Ok && n)
        p3 = n->color_primaries == heif_color_primaries_SMPTE_EG_432_1
             || n->color_primaries == heif_color_primaries_SMPTE_RP_431_2;
      heif_nclx_color_profile_free (n);
      return p3;
    }
  if (t == heif_color_profile_type_prof || t == heif_color_profile_type_rICC)
    {
      size_t len = heif_image_handle_get_raw_color_profile_size (h), i;
      unsigned char *icc = len ? malloc (len) : NULL;
      int p3 = 0;
      if (icc && heif_image_handle_get_raw_color_profile (h, icc).code == heif_error_Ok)
        /* Its description: "Display P3", in ASCII ('desc', ICC v2) or UTF-16 ('mluc', v4). */
        for (i = 0; i + 20 <= len && !p3; i++)
          p3 = memcmp (icc + i, "Display P3", 10) == 0
               || memcmp (icc + i, "\0D\0i\0s\0p\0l\0a\0y\0 \0P\0" "3", 20) == 0;
      free (icc);
      return p3;
    }
  return 0;
}

static void p3_to_srgb (unsigned char *rgb, int w, int h, int stride)
{
  /* Display P3 and sRGB share the white point and the transfer curve. */
  static const float m[9] = { 1.2249f, -0.2247f, 0.0f,
                              -0.0420f, 1.0419f, 0.0f,
                              -0.0197f, -0.0786f, 1.0979f };
  float lin[256];
  unsigned char enc[4097];
  int k, x, y;
  for (k = 0; k < 256; k++)
    {
      double c = k / 255.0;
      lin[k] = (float) (c <= 0.04045 ? c / 12.92 : pow ((c + 0.055) / 1.055, 2.4));
    }
  for (k = 0; k <= 4096; k++)
    {
      double c = k / 4096.0;
      c = c <= 0.0031308 ? c * 12.92 : 1.055 * pow (c, 1 / 2.4) - 0.055;
      enc[k] = (unsigned char) (c * 255 + 0.5);
    }
  for (y = 0; y < h; y++)
    {
      unsigned char *p = rgb + (size_t) y * stride;
      for (x = 0; x < w; x++, p += 3)
        {
          float r = lin[p[0]], g = lin[p[1]], b = lin[p[2]];
          float o[3];
          int c;
          o[0] = m[0] * r + m[1] * g + m[2] * b;
          o[1] = m[3] * r + m[4] * g + m[5] * b;
          o[2] = m[6] * r + m[7] * g + m[8] * b;
          for (c = 0; c < 3; c++)
            {
              int i = (int) (o[c] * 4096 + 0.5f);
              p[c] = enc[i < 0 ? 0 : i > 4096 ? 4096 : i];
            }
        }
    }
}

/* --- where the EXIF is in the file (ISO/IEC 14496-12 boxes; 23008-12),
   so Lumen's Info tab can list it: the Exif item in iinf, found by iloc --- */

static unsigned long be (const unsigned char *p, int n)
{
  unsigned long v = 0;
  while (n-- > 0)
    v = v << 8 | *p++;
  return v;
}

/* The box at buf[pos]: its 4cc and where its body and end are. 0 if none. */
static int box_at (const unsigned char *buf, size_t len, size_t pos, char type[5], size_t *body, size_t *end)
{
  unsigned long size;
  if (pos + 8 > len)
    return 0;
  size = be (buf + pos, 4);
  memcpy (type, buf + pos + 4, 4);
  type[4] = 0;
  *body = pos + 8;
  if (size == 1)
    return 0;                          /* 64-bit sizes: not at the start of a photo */
  if (size == 0)
    size = len - pos;
  if (size < 8 || pos + size > len)
    {
      /* A box running past what was read (mdat): only its start is here. */
      *end = len;
      return 1;
    }
  *end = pos + size;
  return 1;
}

static int exif_item_at (const unsigned char *buf, size_t len, long *offset, long *length)
{
  size_t pos = 0, body, end, meta = 0, meta_end = 0;
  char type[5];
  unsigned long exif_id = 0;
  size_t p, e;
  /* The top-level meta box (a full box: version and flags first). */
  while (box_at (buf, len, pos, type, &body, &end))
    {
      if (!strcmp (type, "meta"))
        {
          meta = body + 4;
          meta_end = end;
          break;
        }
      if (end <= pos)
        break;
      pos = end;
    }
  if (!meta)
    return 0;
  /* iinf: the item whose type is 'Exif'. */
  for (p = meta; box_at (buf, meta_end, p, type, &body, &e) && e > p; p = e)
    if (!strcmp (type, "iinf") && body + 6 <= e)
      {
        int v = buf[body];
        size_t q = body + 4 + (v == 0 ? 2 : 4), qb, qe;
        char t2[5];
        for (; box_at (buf, e, q, t2, &qb, &qe) && qe > q; q = qe)
          if (!strcmp (t2, "infe") && qb + 12 <= qe)
            {
              int iv = buf[qb];
              unsigned long id;
              size_t tp;
              if (iv < 2)
                continue;
              id = iv == 2 ? be (buf + qb + 4, 2) : be (buf + qb + 4, 4);
              tp = qb + 4 + (iv == 2 ? 2 : 4) + 2;
              if (tp + 4 <= qe && memcmp (buf + tp, "Exif", 4) == 0)
                exif_id = id;
            }
      }
  if (!exif_id)
    return 0;
  /* iloc: where that item's data is (its first extent, from the file's start). */
  for (p = meta; box_at (buf, meta_end, p, type, &body, &e) && e > p; p = e)
    if (!strcmp (type, "iloc") && body + 8 <= e)
      {
        int v = buf[body], osz = buf[body + 4] >> 4, lsz = buf[body + 4] & 15, bsz = buf[body + 5] >> 4;
        int isz = v >= 1 ? (buf[body + 5] & 15) : 0;
        size_t q = body + 6;
        unsigned long count, k;
        if (v > 2)
          return 0;
        count = v < 2 ? be (buf + q, 2) : be (buf + q, 4);
        q += v < 2 ? 2 : 4;
        for (k = 0; k < count && q < e; k++)
          {
            unsigned long id, base, ext, method = 0, j;
            if (q + 16 > e)
              return 0;
            id = v < 2 ? be (buf + q, 2) : be (buf + q, 4);
            q += v < 2 ? 2 : 4;
            if (v >= 1)
              {
                method = be (buf + q, 2) & 15;
                q += 2;
              }
            q += 2;                    /* data_reference_index */
            base = be (buf + q, bsz);
            q += (size_t) bsz;
            ext = be (buf + q, 2);
            q += 2;
            for (j = 0; j < ext; j++)
              {
                unsigned long off, n;
                if (q + (size_t) (isz + osz + lsz) > e)
                  return 0;
                q += (size_t) isz;
                off = be (buf + q, osz);
                q += (size_t) osz;
                n = be (buf + q, lsz);
                q += (size_t) lsz;
                if (id == exif_id && j == 0 && method == 0)
                  {
                    *offset = (long) (base + off);
                    *length = (long) n;
                    return n > 8;
                  }
              }
          }
      }
  return 0;
}

static int exif_tiff (const unsigned char *item, size_t len, size_t *tiff)
{
  /* A 4-byte offset from the end of itself to the TIFF header (often 6:
     past "Exif\0\0"). */
  size_t off;
  if (len < 12)
    return 0;
  off = 4 + (size_t) be (item, 4);
  if (off + 8 > len)
    return 0;
  if (!((item[off] == 'I' && item[off + 1] == 'I') || (item[off] == 'M' && item[off + 1] == 'M')))
    return 0;
  *tiff = off;
  return 1;
}

/* "exif=OFFSET LENGTH": the TIFF data's place in the file, or "". The
   meta box can be anywhere at the top level (phones often write it after
   the picture data), so the top-level boxes are walked through the file. */
static void exif_place (const char *path, char *out, size_t n)
{
  FILE *f = fopen (path, "rb");
  unsigned char h[16], hdr[64], *buf = NULL;
  long pos = 0, at, len;
  size_t tiff;
  out[0] = '\0';
  if (!f)
    return;
  for (;;)
    {
      unsigned long long size;
      if (fseek (f, pos, SEEK_SET) != 0 || fread (h, 1, 8, f) != 8)
        break;
      size = be (h, 4);
      if (size == 1)
        {
          if (fread (h + 8, 1, 8, f) != 8)
            break;
          size = (unsigned long long) be (h + 8, 4) << 32 | be (h + 12, 4);
        }
      if (memcmp (h + 4, "meta", 4) == 0)
        {
          if (size < 12 || size > 8u * 1024 * 1024 || !(buf = malloc ((size_t) size))
              || fseek (f, pos, SEEK_SET) != 0 || fread (buf, 1, (size_t) size, f) != (size_t) size)
            break;
          /* iloc's offsets are from the start of the file, wherever meta is. */
          if (exif_item_at (buf, (size_t) size, &at, &len) && len > (long) sizeof hdr
              && fseek (f, at, SEEK_SET) == 0 && fread (hdr, 1, sizeof hdr, f) == sizeof hdr
              && exif_tiff (hdr, sizeof hdr, &tiff))
            snprintf (out, n, "exif=%ld %ld\n", at + (long) tiff, len - (long) tiff);
          break;
        }
      if (size == 0 || size < 8 || size > 0x7FFFFFFFull - (unsigned long long) pos)
        break;
      pos += (long) size;
    }
  free (buf);
  fclose (f);
}

/* --- the photo's details --- */

static void describe (const char *src, const struct heif_image_handle *h, int w, int hh, char *t, size_t n)
{
  img_info in;
  char place[64];
  heif_item_id id;
  memset (&in, 0, sizeof in);
  if (heif_image_handle_get_list_of_metadata_block_IDs (h, "Exif", &id, 1) == 1)
    {
      size_t len = heif_image_handle_get_metadata_size (h, id);
      unsigned char *d = len > 4 ? malloc (len) : NULL;
      if (d && heif_image_handle_get_metadata (h, id, d).code == heif_error_Ok)
        {
          /* A 4-byte offset to the TIFF header, then (usually "Exif\0\0" and) the TIFF data. */
          size_t off = 4 + (((size_t) d[0] << 24) | ((size_t) d[1] << 16) | ((size_t) d[2] << 8) | d[3]);
          if (off < len)
            img_read_raw_info (d + off, len - off, &in);
        }
      free (d);
    }
  exif_place (src, place, sizeof place);
  snprintf (t, n,
            "width=%d\nheight=%d\norientation=1\nmake=%s\nmodel=%s\niso=%d\nexposure=%g\n"
            "fnumber=%g\nfocal=%g\ntaken=%s\nlens=\n%s",
            w, hh, in.make, in.model, in.iso, in.exposure, in.fnumber, in.focal, in.taken, place);
}

/* --- decoding --- */

/* Decodes h to 8-bit sRGB, box-filtered down to max_side at most (0: as it
   is). NULL with *why on error. */
static unsigned char *picture (struct heif_image_handle *h, int max_side, int *ow, int *oh, const char **why)
{
  struct heif_image *img = NULL;
  struct heif_error e;
  const uint8_t *src;
  int stride, w, hh, tw, th, x, y;
  unsigned char *out;
  e = heif_decode_image (h, &img, heif_colorspace_RGB, heif_chroma_interleaved_RGB, NULL);
  if (e.code != heif_error_Ok)
    {
      *why = e.message;
      return NULL;
    }
  src = heif_image_get_plane_readonly (img, heif_channel_interleaved, &stride);
  w = heif_image_get_width (img, heif_channel_interleaved);
  hh = heif_image_get_height (img, heif_channel_interleaved);
  if (!src || w <= 0 || hh <= 0)
    {
      heif_image_release (img);
      *why = "no picture in it";
      return NULL;
    }
  if (is_p3 (h))
    p3_to_srgb ((unsigned char *) src, w, hh, stride);
  tw = w; th = hh;
  if (max_side > 0 && (w > max_side || hh > max_side))
    {
      if (w >= hh)
        {
          tw = max_side;
          th = (int) (((long long) hh * tw + w / 2) / w);
        }
      else
        {
          th = max_side;
          tw = (int) (((long long) w * th + hh / 2) / hh);
        }
      if (tw < 1) tw = 1;
      if (th < 1) th = 1;
    }
  out = malloc ((size_t) tw * th * 3);
  if (!out)
    {
      heif_image_release (img);
      *why = "not enough memory";
      return NULL;
    }
  if (tw == w && th == hh)
    for (y = 0; y < hh; y++)
      memcpy (out + (size_t) y * w * 3, src + (size_t) y * stride, (size_t) w * 3);
  else
    {
      /* Box filter: each output pixel the average of the source pixels in it. */
      int *x0 = malloc (sizeof *x0 * (size_t) (tw + 1));
      if (!x0)
        {
          free (out);
          heif_image_release (img);
          *why = "not enough memory";
          return NULL;
        }
      for (x = 0; x <= tw; x++)
        x0[x] = (int) ((long long) x * w / tw);
      for (y = 0; y < th; y++)
        {
          int y0 = (int) ((long long) y * hh / th), y1 = (int) ((long long) (y + 1) * hh / th);
          if (y1 <= y0)
            y1 = y0 + 1;
          for (x = 0; x < tw; x++)
            {
              unsigned s[3] = { 0, 0, 0 }, cnt;
              int xa = x0[x], xb = x0[x + 1] > xa ? x0[x + 1] : xa + 1, yy, xx, c;
              for (yy = y0; yy < y1; yy++)
                {
                  const uint8_t *p = src + (size_t) yy * stride + (size_t) xa * 3;
                  for (xx = xa; xx < xb; xx++, p += 3)
                    {
                      s[0] += p[0];
                      s[1] += p[1];
                      s[2] += p[2];
                    }
                }
              cnt = (unsigned) ((y1 - y0) * (xb - xa));
              for (c = 0; c < 3; c++)
                out[((size_t) y * tw + x) * 3 + c] = (unsigned char) ((s[c] + cnt / 2) / cnt);
            }
        }
      free (x0);
    }
  heif_image_release (img);
  *ow = tw;
  *oh = th;
  return out;
}

int main (int argc, char **argv)
{
  struct heif_context *ctx;
  struct heif_image_handle *h = NULL, *th = NULL;
  struct heif_error e;
  const char *src, *image, *info, *why = NULL;
  unsigned char *rgb;
  char t[1200];
  int w, hh, maxside, pw, ph, rc;
  if (argc < 5 || (strcmp (argv[1], "thumb") != 0 && strcmp (argv[1], "decode") != 0)
      || (strcmp (argv[1], "decode") == 0 && argc < 6))
    {
      fprintf (stderr, "LumenHEIF %s (libheif %s, FFmpeg's HEVC decoder)\n"
               "  LumenHEIF thumb SRC IMAGE INFO\n  LumenHEIF decode SRC IMAGE INFO MAXSIDE\n",
               VERSION, heif_get_version ());
      return 2;
    }
  src = argv[2];
  image = argv[3];
  info = argv[4];
  if (!has_neon ())
    av_force_cpu_flags (av_get_cpu_flags () & ~AV_CPU_FLAG_NEON);
  heif_init (NULL);
  ctx = heif_context_alloc ();
  if (!ctx)
    return fail (info, "not enough memory", NULL);
  e = heif_context_read_from_file (ctx, src, NULL);
  if (e.code != heif_error_Ok)
    return fail (info, "not a HEIF photo Lumen can read", e.message);
  e = heif_context_get_primary_image_handle (ctx, &h);
  if (e.code != heif_error_Ok)
    return fail (info, "no photo in the file", e.message);
  w = heif_image_handle_get_width (h);
  hh = heif_image_handle_get_height (h);
  describe (src, h, w, hh, t, sizeof t);
  if (write_info (info, t))
    return 1;

  maxside = 0;
  if (strcmp (argv[1], "thumb") == 0)
    {
      /* The photo's own thumbnail if it has one (small: fast); else the photo, made small. */
      heif_item_id tid;
      if (heif_image_handle_get_list_of_thumbnail_IDs (h, &tid, 1) == 1
          && heif_image_handle_get_thumbnail (h, tid, &th).code == heif_error_Ok)
        {
          rgb = picture (th, THUMB_MAX, &pw, &ph, &why);
          heif_image_handle_release (th);
          if (rgb)
            goto write;
        }
      maxside = THUMB_MAX;
    }
  else
    maxside = atoi (argv[5]);
  rgb = picture (h, maxside, &pw, &ph, &why);
  if (!rgb)
    return fail (info, "can't decode the photo", why);
write:
  rc = write_ppm (image, rgb, pw, ph);
  free (rgb);
  heif_image_handle_release (h);
  heif_context_free (ctx);
  heif_deinit ();
  return rc ? fail (info, "can't write the picture", NULL) : 0;
}
