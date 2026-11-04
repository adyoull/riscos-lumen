/* Copyright (c) 2026 Andrew Youll
   Released under the MIT licence: see the LICENSE file.
   SPDX-License-Identifier: MIT */
/* LumenRaw: Lumen's raw plug-in. A small program built on LibRaw that
   Lumen runs in the background (in a task window) for each raw file job:

     LumenRaw thumb  SRC IMAGE INFO          the camera's embedded preview
     LumenRaw decode SRC IMAGE INFO MAXSIDE  the raw developed to sRGB

   IMAGE is a JPEG (an embedded preview as the camera made it) or a binary
   PPM (P6, 8 bits, sRGB). INFO is text, one "key=value" a line: the
   picture's size before turning, its EXIF orientation, camera, exposure,
   lens and date; or "error=..." if the file couldn't be read. Both are
   written under a temporary name and renamed when complete, INFO first,
   so Lumen sees either nothing or a whole file.

   decode develops at half size (LibRaw's 2x2 binning, fast) when that is
   still at least MAXSIDE on the longer side, and in full otherwise. The
   camera's white balance and colour matrix are used; the picture is not
   turned (Lumen turns it, by the orientation in INFO).

   Copyright (C) 2026 Andrew Youll. LibRaw is used under the GNU LGPL 2.1
   (see Licence); this program's source is in Lumen's source, plugins.libraw. */
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include "libraw/libraw.h"
#ifdef __riscos
#include <unixlib/local.h>
int __riscosify_control = __RISCOSIFY_NO_PROCESS;   /* paths are RISC OS paths, unchanged */
#endif

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

static int fail (const char *info, const char *what, int code)
{
  char t[400];
  snprintf (t, sizeof t, "error=%s%s%s\n", what, code ? ": " : "", code ? libraw_strerror (code) : "");
  write_info (info, t);
  return 1;
}

/* LibRaw's flip (0, 3 180, 5 90 anticlockwise, 6 90 clockwise) as EXIF's. */
static int exif_orientation (int flip)
{
  switch (flip)
    {
    case 3: return 3;
    case 5: return 8;
    case 6: return 6;
    }
  return 1;
}

static void describe (LibRaw &r, char *t, size_t n)
{
  const libraw_iparams_t &ip = r.imgdata.idata;
  const libraw_imgother_t &o = r.imgdata.other;
  char when[32] = "";
  if (o.timestamp > 0)
    {
      time_t ts = o.timestamp;
      struct tm *tm = gmtime (&ts);      /* LibRaw reads the camera's local time as if UTC */
      if (tm)
        strftime (when, sizeof when, "%Y:%m:%d %H:%M:%S", tm);
    }
  snprintf (t, n,
            "width=%d\nheight=%d\norientation=%d\nmake=%s\nmodel=%s\niso=%.0f\nexposure=%g\n"
            "fnumber=%g\nfocal=%g\ntaken=%s\nlens=%s\n",
            r.imgdata.sizes.width, r.imgdata.sizes.height, exif_orientation (r.imgdata.sizes.flip),
            ip.make, ip.model, o.iso_speed, o.shutter, o.aperture, o.focal_len, when,
            r.imgdata.lens.Lens);
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

/* Develops the opened raw and writes it as a PPM. */
static int develop (LibRaw &r, const char *image, const char *info)
{
  int rc;
  libraw_processed_image_t *mem;
  if ((rc = r.unpack ()) != LIBRAW_SUCCESS)
    return fail (info, "can't unpack the raw data", rc);
  if ((rc = r.dcraw_process ()) != LIBRAW_SUCCESS)
    return fail (info, "can't develop it", rc);
  mem = r.dcraw_make_mem_image (&rc);
  if (!mem || mem->type != LIBRAW_IMAGE_BITMAP || mem->colors != 3 || mem->bits != 8)
    return fail (info, "can't make the picture", rc);
  rc = write_ppm (image, mem->data, mem->width, mem->height);
  LibRaw::dcraw_clear_mem (mem);
  return rc ? fail (info, "can't write the picture", 0) : 0;
}

/* LibRaw reports damaged data on stderr by default; any output would open
   a task window on the desktop, so it is kept quiet (a damaged file still
   develops as far as it can). */
static void quiet (void *data, const char *file, const INT64 offset)
{
  (void) data; (void) file; (void) offset;
}

static void settings (LibRaw &r)
{
  r.set_dataerror_handler (quiet, NULL);
  libraw_output_params_t &p = r.imgdata.params;
  p.output_bps = 8;
  p.output_color = 1;                  /* sRGB */
  p.use_camera_wb = 1;
  p.use_camera_matrix = 1;
  p.user_flip = 0;                     /* Lumen turns it */
  p.user_qual = 3;                     /* AHD demosaicing */
}

int main (int argc, char **argv)
{
  static LibRaw r;
  char t[1200];
  int rc;
  if (argc < 5 || (strcmp (argv[1], "thumb") != 0 && strcmp (argv[1], "decode") != 0)
      || (strcmp (argv[1], "decode") == 0 && argc < 6))
    {
      fprintf (stderr, "LumenRaw %s (LibRaw %s)\n"
               "  LumenRaw thumb SRC IMAGE INFO\n  LumenRaw decode SRC IMAGE INFO MAXSIDE\n",
               "1.1", libraw_version ());
      return 2;
    }
  const char *src = argv[2], *image = argv[3], *info = argv[4];
  settings (r);
  if ((rc = r.open_file (src)) != LIBRAW_SUCCESS)
    return fail (info, "not a raw file Lumen can read", rc);
  describe (r, t, sizeof t);

  if (strcmp (argv[1], "thumb") == 0)
    {
      /* The camera's own preview if it has one (fast); developed if not. */
      if (r.unpack_thumb () == LIBRAW_SUCCESS && r.imgdata.thumbnail.tformat == LIBRAW_THUMBNAIL_JPEG
          && r.imgdata.thumbnail.tlength > 0)
        {
          char tmp[1100];
          FILE *f;
          if (write_info (info, t))
            return 1;
          tmpname (tmp, sizeof tmp, image);
          f = fopen (tmp, "wb");
          if (!f || fwrite (r.imgdata.thumbnail.thumb, 1, r.imgdata.thumbnail.tlength, f) != r.imgdata.thumbnail.tlength
              || fclose (f) || finish (tmp, image))
            return fail (info, "can't write the preview", 0);
          return 0;
        }
      r.recycle ();
      settings (r);
      r.imgdata.params.half_size = 1;
      if ((rc = r.open_file (src)) != LIBRAW_SUCCESS)
        return fail (info, "not a raw file Lumen can read", rc);
      if (write_info (info, t))
        return 1;
      return develop (r, image, info);
    }

  /* decode */
  {
    int maxside = atoi (argv[5]);
    int longer = r.imgdata.sizes.width > r.imgdata.sizes.height ? r.imgdata.sizes.width : r.imgdata.sizes.height;
    if (maxside > 0 && longer / 2 >= maxside)
      r.imgdata.params.half_size = 1;
    if (write_info (info, t))
      return 1;
    return develop (r, image, info);
  }
}
