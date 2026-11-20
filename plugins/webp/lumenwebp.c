/* Copyright (c) 2026 Andrew Youll
   Released under the MIT licence: see the LICENSE file.
   SPDX-License-Identifier: MIT */
/* LumenWebP: Lumen's WebP plug-in. A small program built on libwebp that
   Lumen runs in the background (in a task window) for each WebP picture
   (.webp: what web pages and many phones and editors save), just as it
   runs LumenHEIF for HEIF photos:

     LumenWebP thumb  SRC IMAGE INFO          a thumbnail, 512 pixels at most
     LumenWebP decode SRC IMAGE INFO MAXSIDE  the picture, MAXSIDE at most

   IMAGE is a binary PPM (P6, 8 bits, sRGB). INFO is text, one "key=value"
   a line: the picture's size, its EXIF orientation, camera, exposure and
   date, where its EXIF is in the file (so Lumen can read its position for
   maps), and how it is encoded; or "error=..." if the file couldn't be
   read. Both are written under a temporary name and renamed when
   complete, INFO first, so Lumen sees either nothing or a whole file.

   Lossy (VP8) and lossless (VP8L) WebP, with or without transparency
   (laid on white), and animated WebP (its first frame). A picture made
   smaller is scaled by libwebp as it decodes, which is fast and smooth.

   NEON: libwebp's NEON code is used only when the processor has it
   (VFPSupport_Features on RISC OS); Lumen$NEON (LUMEN_NEON elsewhere) 0
   forces the plain code, 1 NEON. Full-size pictures are the same either
   way; scaled ones can differ by a level or two in a few edge pixels.

   libwebp is used under its BSD licence (see Licence). */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <webp/decode.h>
#include <webp/demux.h>
#include "exif.h"
#include "../handover.h"
#ifdef __riscos
#include <kernel.h>
#include <unixlib/local.h>
int __riscosify_control = __RISCOSIFY_NO_PROCESS;   /* paths are RISC OS paths, unchanged */
#elif defined(__arm__)
#include <sys/auxv.h>
#endif

#define VERSION "1.0"
#define THUMB_MAX 512
#define MAX_FILE (256L * 1024 * 1024)

/* libwebp's switch for its processor-specific code (src/dsp/cpu.h): a
   function it asks "has the processor got this?". kNEON is 6. */
typedef int (*webp_cpu_info) (int feature);
extern webp_cpu_info VP8GetCPUInfo;
#define WEBP_KNEON 6

/* --- writing results --- */

static int write_info (const char *path, const char *text)
{
  char tmp[1100];
  FILE *f;
  snprintf (tmp, sizeof tmp, "%s_t", path);
  f = fopen (tmp, "w");
  if (!f)
    return -1;
  fputs (text, f);
  if (fclose (f))
    return -1;
  remove (path);
  return rename (tmp, path);
}

static int fail (const char *info, const char *what, const char *why)
{
  char t[400];
  snprintf (t, sizeof t, "error=%s%s%s\n", what, why && *why ? ": " : "", why ? why : "");
  write_info (info, t);
  return 1;
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

static int use_neon;

static int cpu_info (int feature)
{
  return feature == WEBP_KNEON && use_neon;
}

/* --- the file --- */

static unsigned long le32 (const unsigned char *p)
{
  return (unsigned long) p[0] | (unsigned long) p[1] << 8 | (unsigned long) p[2] << 16 | (unsigned long) p[3] << 24;
}

/* The whole file in memory (WebP files are small: the decoder wants it
   all), NULL with *why if it can't be read. */
static unsigned char *load (const char *path, size_t *len, const char **why)
{
  FILE *f = fopen (path, "rb");
  unsigned char *buf;
  long n;
  if (!f)
    {
      *why = "can't open the file";
      return NULL;
    }
  if (fseek (f, 0, SEEK_END) != 0 || (n = ftell (f)) < 12 || n > MAX_FILE || fseek (f, 0, SEEK_SET) != 0)
    {
      fclose (f);
      *why = "not a WebP file";
      return NULL;
    }
  buf = malloc ((size_t) n);
  if (!buf || fread (buf, 1, (size_t) n, f) != (size_t) n)
    {
      free (buf);
      fclose (f);
      *why = buf ? "can't read the file" : "not enough memory";
      return NULL;
    }
  fclose (f);
  *len = (size_t) n;
  return buf;
}

/* Where the EXIF chunk's TIFF data is in the file: 1 if there is one.
   The chunks follow "RIFF" size "WEBP", each a four-letter name, a
   little-endian length and the data, padded to an even length. Some
   programs start the data with "Exif\0\0" as in a JPEG. */
static int exif_chunk (const unsigned char *buf, size_t len, size_t *at, size_t *n)
{
  size_t pos = 12, end = len;
  if (len < 12 || memcmp (buf, "RIFF", 4) != 0 || memcmp (buf + 8, "WEBP", 4) != 0)
    return 0;
  if (le32 (buf + 4) + 8 < end)
    end = le32 (buf + 4) + 8;
  while (pos + 8 <= end)
    {
      unsigned long size = le32 (buf + pos + 4);
      if (size > end - pos - 8)
        break;
      if (memcmp (buf + pos, "EXIF", 4) == 0)
        {
          size_t a = pos + 8, l = size;
          if (l > 6 && memcmp (buf + a, "Exif\0\0", 6) == 0)
            a += 6, l -= 6;
          if (l < 8 || !((buf[a] == 'I' && buf[a + 1] == 'I') || (buf[a] == 'M' && buf[a + 1] == 'M')))
            return 0;
          *at = a;
          *n = l;
          return 1;
        }
      pos += 8 + size + (size & 1);
    }
  return 0;
}

/* --- the picture's details --- */

static void describe (const unsigned char *buf, size_t len, const WebPBitstreamFeatures *ft, int w, int h, int frames,
                      int lossless, char *t, size_t n)
{
  img_info in;
  size_t at, el;
  char place[64] = "";
  memset (&in, 0, sizeof in);
  in.orientation = 1;
  if (exif_chunk (buf, len, &at, &el))
    {
      img_read_tiff_tags (buf + at, el, &in);
      snprintf (place, sizeof place, "exif=%lu %lu\n", (unsigned long) at, (unsigned long) el);
    }
  if (in.orientation < 1 || in.orientation > 8)
    in.orientation = 1;
  snprintf (t, n,
            "width=%d\nheight=%d\norientation=%d\nmake=%s\nmodel=%s\niso=%d\nexposure=%g\n"
            "fnumber=%g\nfocal=%g\ntaken=%s\nlens=%s\n%sencoding=%s%s%s",
            w, h, in.orientation, in.make, in.model, in.iso, in.exposure, in.fnumber, in.focal, in.taken, in.lens,
            place, lossless ? "WebP lossless" : "WebP lossy", ft->has_alpha ? ", with transparency" : "",
            frames > 1 ? ", animated" : "");
  if (frames > 1 && strlen (t) + 24 < n)
    snprintf (t + strlen (t), n - strlen (t), " (%d frames)", frames);
  if (strlen (t) + 2 < n)
    strcat (t, "\n");
}

/* --- decoding --- */

/* The size for a picture w x h with its longer side max_side at most (0:
   as it is). */
static void fit (int w, int h, int max_side, int *ow, int *oh)
{
  *ow = w;
  *oh = h;
  if (max_side > 0 && (w > max_side || h > max_side))
    {
      if (w >= h)
        {
          *ow = max_side;
          *oh = (int) (((long long) h * max_side + w / 2) / w);
        }
      else
        {
          *oh = max_side;
          *ow = (int) (((long long) w * max_side + h / 2) / h);
        }
      if (*ow < 1)
        *ow = 1;
      if (*oh < 1)
        *oh = 1;
    }
}

/* RGBA to RGB in place, laid on white. */
static void flatten (unsigned char *p, size_t pixels)
{
  const unsigned char *s = p;
  unsigned char *o = p;
  size_t i;
  for (i = 0; i < pixels; i++, s += 4, o += 3)
    {
      unsigned a = s[3], k;
      for (k = 0; k < 3; k++)
        o[k] = (unsigned char) ((s[k] * a + 255 * (255 - a) + 127) / 255);
    }
}

/* Decodes one bitstream (a still picture, or one frame's) to RGB, scaled
   to ow x oh. NULL with *why on error. */
static unsigned char *decode (const uint8_t *data, size_t size, int ow, int oh, int alpha, const char **why)
{
  WebPDecoderConfig cfg;
  unsigned char *out;
  size_t stride = (size_t) ow * (alpha ? 4 : 3);
  VP8StatusCode st;
  if (!WebPInitDecoderConfig (&cfg))
    {
      *why = "libwebp version mismatch";
      return NULL;
    }
  if (WebPGetFeatures (data, size, &cfg.input) != VP8_STATUS_OK)
    {
      *why = "the picture data is damaged";
      return NULL;
    }
  out = malloc (stride * (size_t) oh);
  if (!out)
    {
      *why = "not enough memory";
      return NULL;
    }
  if (ow != cfg.input.width || oh != cfg.input.height)
    {
      cfg.options.use_scaling = 1;
      cfg.options.scaled_width = ow;
      cfg.options.scaled_height = oh;
    }
  cfg.output.colorspace = alpha ? MODE_RGBA : MODE_RGB;
  cfg.output.is_external_memory = 1;
  cfg.output.u.RGBA.rgba = out;
  cfg.output.u.RGBA.stride = (int) stride;
  cfg.output.u.RGBA.size = stride * (size_t) oh;
  st = WebPDecode (data, size, &cfg);
  WebPFreeDecBuffer (&cfg.output);
  if (st != VP8_STATUS_OK)
    {
      free (out);
      *why = st == VP8_STATUS_OUT_OF_MEMORY ? "not enough memory"
           : st == VP8_STATUS_UNSUPPORTED_FEATURE ? "a WebP feature Lumen can't read"
           : st == VP8_STATUS_NOT_ENOUGH_DATA ? "the file is cut short"
           : "the picture data is damaged";
      return NULL;
    }
  if (alpha)
    {
      flatten (out, (size_t) ow * (size_t) oh);
      out = realloc (out, (size_t) ow * 3 * (size_t) oh) ? : out;
    }
  return out;
}

/* The picture: a still one decoded, scaled to max_side; or an animation's
   first frame on its canvas (white where the frame doesn't cover it). */
static unsigned char *picture (const unsigned char *buf, size_t len, const WebPBitstreamFeatures *ft, int max_side,
                               int *pw, int *ph, int *frames, int *lossless, const char **why)
{
  WebPData wd;
  WebPDemuxer *dm;
  WebPIterator it;
  unsigned char *rgb, *canvas;
  int cw, ch, ow, oh, fw, fh, fx, fy, y;
  *frames = 1;
  *lossless = ft->format == 2;
  if (!ft->has_animation)
    {
      fit (ft->width, ft->height, max_side, pw, ph);
      return decode (buf, len, *pw, *ph, ft->has_alpha, why);
    }
  wd.bytes = buf;
  wd.size = len;
  dm = WebPDemux (&wd);
  if (!dm)
    {
      *why = "the animation is damaged";
      return NULL;
    }
  cw = (int) WebPDemuxGetI (dm, WEBP_FF_CANVAS_WIDTH);
  ch = (int) WebPDemuxGetI (dm, WEBP_FF_CANVAS_HEIGHT);
  *frames = (int) WebPDemuxGetI (dm, WEBP_FF_FRAME_COUNT);
  if (!WebPDemuxGetFrame (dm, 1, &it))
    {
      WebPDemuxDelete (dm);
      *why = "the animation has no frames";
      return NULL;
    }
  {
    WebPBitstreamFeatures f1;
    if (WebPGetFeatures (it.fragment.bytes, it.fragment.size, &f1) == VP8_STATUS_OK)
      *lossless = f1.format == 2;
  }
  /* The frame scaled as the canvas is, and placed where it goes on it. */
  fit (cw, ch, max_side, &ow, &oh);
  fw = (int) (((long long) it.width * ow + cw / 2) / cw);
  fh = (int) (((long long) it.height * oh + ch / 2) / ch);
  fx = (int) ((long long) it.x_offset * ow / cw);
  fy = (int) ((long long) it.y_offset * oh / ch);
  if (fw < 1) fw = 1;
  if (fh < 1) fh = 1;
  if (fx + fw > ow) fw = ow - fx;
  if (fy + fh > oh) fh = oh - fy;
  rgb = fw > 0 && fh > 0 ? decode (it.fragment.bytes, it.fragment.size, fw, fh, 1, why) : NULL;
  WebPDemuxReleaseIterator (&it);
  WebPDemuxDelete (dm);
  if (!rgb)
    return NULL;
  canvas = malloc ((size_t) ow * 3 * (size_t) oh);
  if (!canvas)
    {
      free (rgb);
      *why = "not enough memory";
      return NULL;
    }
  memset (canvas, 255, (size_t) ow * 3 * (size_t) oh);
  for (y = 0; y < fh; y++)
    memcpy (canvas + ((size_t) (fy + y) * ow + fx) * 3, rgb + (size_t) y * fw * 3, (size_t) fw * 3);
  free (rgb);
  *pw = ow;
  *ph = oh;
  return canvas;
}

int main (int argc, char **argv)
{
  const char *src, *image, *info, *why = NULL;
  unsigned char *buf, *rgb;
  size_t len;
  WebPBitstreamFeatures ft;
  char t[1200];
  int maxside, pw, ph, frames, lossless, rc, dv;
  if (argc < 5 || (strcmp (argv[1], "thumb") != 0 && strcmp (argv[1], "decode") != 0)
      || (strcmp (argv[1], "decode") == 0 && argc < 6))
    {
      dv = WebPGetDecoderVersion ();
      fprintf (stderr, "LumenWebP %s (libwebp %d.%d.%d)\n"
               "  LumenWebP thumb SRC IMAGE INFO\n  LumenWebP decode SRC IMAGE INFO MAXSIDE [NOISE [QUALITY [MEMORY]]]\n"
               "  (NOISE and QUALITY are for raw plug-ins and ignored; MEMORY: 1 the picture may be handed over in memory)\n",
               VERSION, dv >> 16, (dv >> 8) & 0xFF, dv & 0xFF);
      return 2;
    }
  src = argv[2];
  image = argv[3];
  info = argv[4];
  use_neon = has_neon ();
  VP8GetCPUInfo = cpu_info;

  buf = load (src, &len, &why);
  if (!buf)
    return fail (info, "can't read the WebP file", why);
  if (WebPGetFeatures (buf, len, &ft) != VP8_STATUS_OK)
    {
      free (buf);
      return fail (info, "not a WebP picture Lumen can read", NULL);
    }
  maxside = strcmp (argv[1], "thumb") == 0 ? THUMB_MAX : atoi (argv[5]);
  rgb = picture (buf, len, &ft, maxside, &pw, &ph, &frames, &lossless, &why);
  if (!rgb)
    {
      free (buf);
      return fail (info, "can't decode the picture", why);
    }
  /* (For an animation, WebPGetFeatures gives the canvas's size.) */
  describe (buf, len, &ft, ft.width, ft.height, frames, lossless, t, sizeof t);
  free (buf);
  rc = handover_write (image, rgb, pw, ph, strcmp (argv[1], "decode") == 0 && argc > 8 && atoi (argv[8]) == 1);
  free (rgb);
  if (rc)
    return fail (info, "can't write the picture", NULL);
  strncat (t, handover_info (), sizeof t - strlen (t) - 1);
  if (write_info (info, t))
    {
      handover_drop ();
      return 1;
    }
  return 0;
}
