/* Copyright (c) 2026 Andrew Youll
   Released under the MIT licence: see the LICENSE file.
   SPDX-License-Identifier: MIT */
/* Lumen's portable core on Linux: x86-64 for speed, and ARM under
   qemu-arm-aligntrap with RISC OS's alignment rules. Makes its own
   fixtures (JPEGs with EXIF, PNGs, a non-image, an application directory),
   imports them and checks the catalogue, the thumbnails and the time
   slicing. Usage: test_core WORKDIR */
#include <stdio.h>
#include "bus_trap.h"
#include <stdlib.h>
#include <string.h>
#include <jpeglib.h>
#include <png.h>
#include <tiffio.h>
#include <sys/stat.h>
#include <unistd.h>
#include <utime.h>
#include "../src/core/catalog.h"
#include "../src/core/xmp.h"
#include "../src/core/exif.h"
#include "../src/core/colour.h"
#include "../src/core/rawjob.h"
#include "../src/core/fsys.h"
#include "../src/core/library.h"
#include "../src/core/libxfer.h"
#include "../src/core/presets.h"
#include "../src/core/libcheck.h"
#include "sqlite3.h"
#include "../src/core/thumbgen.h"
#include "../src/core/tiffmeta.h"
#include "../src/core/fits.h"
#include "../src/core/astro.h"
#include "../src/core/stack.h"
#include "../src/core/plugins.h"
#include "../src/core/prefs.h"
#include <math.h>
#include "../src/core/lens.h"
#include "../src/core/geodata.h"

static int checks, fails;
#define CHECK(c, what) do { checks++; if (!(c)) { fails++; fprintf (stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, what); } } while (0)

static char work[512];

static void put16 (unsigned char *p, unsigned v) { p[0] = (unsigned char) v; p[1] = (unsigned char) (v >> 8); }
static void put32 (unsigned char *p, unsigned long v) { put16 (p, v & 0xFFFF); put16 (p + 2, v >> 16); }

/* A little-endian EXIF block: IFD0 (Orientation, Make, Model, ExifIFD),
   the EXIF IFD (DateTimeOriginal, ExposureTime, FNumber, ISO, FocalLength). */
static size_t make_exif (unsigned char *b, int orientation, const char *taken)
{
  unsigned char *t;
  size_t ifd0 = 8, exif_ifd, data;
  memset (b, 0, 512);
  memcpy (b, "Exif\0\0", 6);
  t = b + 6;
  memcpy (t, "II", 2); put16 (t + 2, 42); put32 (t + 4, ifd0);
  /* IFD0: 4 entries, then next-IFD 0, then data */
  put16 (t + ifd0, 4);
  exif_ifd = ifd0 + 2 + 4 * 12 + 4;
  data = exif_ifd + 2 + 5 * 12 + 4;
  /* Orientation SHORT 1 */
  put16 (t + ifd0 + 2, 0x0112); put16 (t + ifd0 + 4, 3); put32 (t + ifd0 + 6, 1); put16 (t + ifd0 + 10, (unsigned) orientation);
  /* Make ASCII "Pi\0" inline */
  put16 (t + ifd0 + 14, 0x010F); put16 (t + ifd0 + 16, 2); put32 (t + ifd0 + 18, 3); memcpy (t + ifd0 + 22, "Pi", 3);
  /* Model ASCII at data */
  put16 (t + ifd0 + 26, 0x0110); put16 (t + ifd0 + 28, 2); put32 (t + ifd0 + 30, 11); put32 (t + ifd0 + 34, data);
  memcpy (t + data, "HQ Camera  ", 11);   /* trailing spaces, as cameras pad */
  data += 12;
  /* ExifIFD pointer LONG */
  put16 (t + ifd0 + 38, 0x8769); put16 (t + ifd0 + 40, 4); put32 (t + ifd0 + 42, 1); put32 (t + ifd0 + 46, exif_ifd);
  put32 (t + ifd0 + 50, 0);
  /* EXIF IFD */
  put16 (t + exif_ifd, 5);
  { unsigned char *e = t + exif_ifd + 2;
    put16 (e, 0x9003); put16 (e + 2, 2); put32 (e + 4, 20); put32 (e + 8, data);
    memcpy (t + data, taken, 20); data += 20;
    e += 12; put16 (e, 0x829A); put16 (e + 2, 5); put32 (e + 4, 1); put32 (e + 8, data);
    put32 (t + data, 1); put32 (t + data + 4, 250); data += 8;
    e += 12; put16 (e, 0x829D); put16 (e + 2, 5); put32 (e + 4, 1); put32 (e + 8, data);
    put32 (t + data, 56); put32 (t + data + 4, 10); data += 8;
    e += 12; put16 (e, 0x8827); put16 (e + 2, 3); put32 (e + 4, 1); put16 (e + 8, 400);
    e += 12; put16 (e, 0x920A); put16 (e + 2, 5); put32 (e + 4, 1); put32 (e + 8, data);
    put32 (t + data, 31); put32 (t + data + 4, 1); data += 8;
    put32 (e + 12, 0); }
  return 6 + data;
}

/* exif_list callback: "group|name|value;" appended to ctx (NULL: ignore). */
static void list_tag (void *ctx, const char *group, const char *name, const char *value)
{
  char *all = ctx;
  if (all && strlen (all) + strlen (group) + strlen (name) + strlen (value) + 4 < 2048)
    sprintf (all + strlen (all), "%s|%s|%s;", group, name, value);
}

/* A w x h JPEG: left half red, right half blue (so orientation shows). */
static void make_jpeg (const char *path, int w, int h, int orientation, const char *taken)
{
  struct jpeg_compress_struct c;
  struct jpeg_error_mgr e;
  FILE *f = fopen (path, "wb");
  unsigned char *row = malloc ((size_t) w * 3), exif[600];
  int x;
  c.err = jpeg_std_error (&e);
  jpeg_create_compress (&c);
  jpeg_stdio_dest (&c, f);
  c.image_width = (JDIMENSION) w; c.image_height = (JDIMENSION) h;
  c.input_components = 3; c.in_color_space = JCS_RGB;
  jpeg_set_defaults (&c);
  jpeg_set_quality (&c, 90, TRUE);
  jpeg_start_compress (&c, TRUE);
  if (taken)
    {
      size_t n = make_exif (exif, orientation, taken);
      jpeg_write_marker (&c, JPEG_APP0 + 1, exif, (unsigned) n);
    }
  for (x = 0; x < w; x++)
    {
      row[3 * x] = x < w / 2 ? 230 : 20;
      row[3 * x + 1] = 30;
      row[3 * x + 2] = x < w / 2 ? 20 : 230;
    }
  while (c.next_scanline < c.image_height)
    {
      JSAMPROW r = row;
      jpeg_write_scanlines (&c, &r, 1);
    }
  jpeg_finish_compress (&c);
  jpeg_destroy_compress (&c);
  fclose (f);
  free (row);
}

static void p (char *out, const char *rel);

/* --- colour spaces */

static void put32be (unsigned char *p, unsigned long v)
{
  p[0] = (unsigned char) (v >> 24); p[1] = (unsigned char) (v >> 16);
  p[2] = (unsigned char) (v >> 8); p[3] = (unsigned char) v;
}

/* A minimal ICC profile: the header and one 'desc' tag (v2 text, or v4
   'mluc' UTF-16), which is all Lumen reads. Returns its length. */
static size_t make_icc (unsigned char *p, const char *desc, int v4)
{
  size_t n = strlen (desc), tag = 144, len, i;
  memset (p, 0, 600);
  memcpy (p + 36, "acsp", 4);
  put32be (p + 128, 1);
  memcpy (p + 132, "desc", 4);
  put32be (p + 136, (unsigned long) tag);
  if (!v4)
    {
      memcpy (p + tag, "desc", 4);
      put32be (p + tag + 8, (unsigned long) n + 1);
      memcpy (p + tag + 12, desc, n + 1);
      len = tag + 12 + n + 1 + 79;     /* the Unicode and Script parts, empty */
    }
  else
    {
      memcpy (p + tag, "mluc", 4);
      put32be (p + tag + 8, 1);
      put32be (p + tag + 12, 12);
      memcpy (p + tag + 16, "enUS", 4);
      put32be (p + tag + 20, (unsigned long) (2 * n));
      put32be (p + tag + 24, 28);
      for (i = 0; i < n; i++)
        {
          p[tag + 28 + 2 * i] = 0;
          p[tag + 28 + 2 * i + 1] = (unsigned char) desc[i];
        }
      len = tag + 28 + 2 * n;
    }
  put32be (p + 140, (unsigned long) (len - tag));
  put32be (p, (unsigned long) len);
  return len;
}

/* A flat w x h JPEG of one colour, with an ICC profile (icc_len > 0) or
   an EXIF block (exif_len > 0). */
static void make_flat_jpeg (const char *path, int w, int h, const unsigned char rgb[3],
                            const unsigned char *icc, size_t icc_len, const unsigned char *exif, size_t exif_len)
{
  struct jpeg_compress_struct c;
  struct jpeg_error_mgr e;
  FILE *f = fopen (path, "wb");
  unsigned char *row = malloc ((size_t) w * 3);
  int x;
  c.err = jpeg_std_error (&e);
  jpeg_create_compress (&c);
  jpeg_stdio_dest (&c, f);
  c.image_width = (JDIMENSION) w; c.image_height = (JDIMENSION) h;
  c.input_components = 3; c.in_color_space = JCS_RGB;
  jpeg_set_defaults (&c);
  jpeg_set_quality (&c, 100, TRUE);
  jpeg_start_compress (&c, TRUE);
  if (icc_len)
    jpeg_write_icc_profile (&c, icc, (unsigned) icc_len);
  if (exif_len)
    jpeg_write_marker (&c, JPEG_APP0 + 1, exif, (unsigned) exif_len);
  for (x = 0; x < w; x++)
    memcpy (row + 3 * x, rgb, 3);
  while (c.next_scanline < c.image_height)
    {
      JSAMPROW r = row;
      jpeg_write_scanlines (&c, &r, 1);
    }
  jpeg_finish_compress (&c);
  jpeg_destroy_compress (&c);
  fclose (f);
  free (row);
}

/* Decodes path whole; the middle pixel and the colour space found. */
static int decode_middle (const char *path, unsigned char out[3])
{
  char err[200];
  thumbgen *t = thumbgen_start (path, NULL, 1 << 30, 1, err, sizeof err);
  rgb_image img;
  int r, cs;
  if (!t)
    return -1;
  while ((r = thumbgen_step (t, 64)) == 0)
    ;
  cs = thumbgen_colour (t);
  if (r < 0 || thumbgen_take (t, &img))
    {
      thumbgen_free (t);
      return -1;
    }
  thumbgen_free (t);
  memcpy (out, img.rgb + ((size_t) (img.h / 2) * img.w + img.w / 2) * 3, 3);
  free (img.rgb);
  return cs;
}

static int near3 (const unsigned char a[3], int r, int g, int b, int tol)
{
  return abs (a[0] - r) <= tol && abs (a[1] - g) <= tol && abs (a[2] - b) <= tol;
}

static void test_colour (void)
{
  static unsigned char icc[600], buf[4096];
  char path[700], name[64];
  unsigned char px[3];
  static const unsigned char green[3] = { 60, 140, 60 }, orange[3] = { 200, 80, 60 }, grey[3] = { 128, 128, 128 };
  size_t n;
  FILE *f;

  /* Profiles by their descriptions, v2 and v4. */
  n = make_icc (icc, "Adobe RGB (1998)", 0);
  CHECK (colour_from_icc (icc, n, name, sizeof name) == CS_ADOBE && strcmp (name, "Adobe RGB (1998)") == 0,
         "ICC v2 desc: Adobe RGB (1998)");
  n = make_icc (icc, "Display P3", 1);
  CHECK (colour_from_icc (icc, n, name, sizeof name) == CS_P3 && strcmp (name, "Display P3") == 0, "ICC v4 mluc: Display P3");
  n = make_icc (icc, "sRGB IEC61966-2.1", 0);
  CHECK (colour_from_icc (icc, n, NULL, 0) == CS_SRGB, "sRGB IEC61966-2.1");
  n = make_icc (icc, "Some printer's profile", 0);
  CHECK (colour_from_icc (icc, n, NULL, 0) == CS_OTHER, "anything else: other (shown as sRGB)");
  CHECK (colour_from_icc (icc, 100, NULL, 0) == CS_OTHER && colour_from_icc (buf, 300, NULL, 0) == CS_OTHER,
         "short or not a profile: other");
  for (n = 0; n < 300; n++)
    colour_from_icc (icc, n, name, sizeof name);
  CHECK (1, "cut-off profiles read safely");

  /* Adobe RGB: decoded, converted; grey stays grey. */
  n = make_icc (icc, "Adobe RGB (1998)", 0);
  p (path, "adobe-green.jpg");
  make_flat_jpeg (path, 48, 32, green, icc, n, NULL, 0);
  CHECK (decode_middle (path, px) == CS_ADOBE, "a JPEG with an Adobe RGB profile is found to be one");
  CHECK (near3 (px, 0, 141, 50, 3), "Adobe RGB (60,140,60) shows as sRGB (0,141,50)");
  p (path, "adobe-orange.jpg");
  make_flat_jpeg (path, 48, 32, orange, icc, n, NULL, 0);
  decode_middle (path, px);
  CHECK (near3 (px, 230, 79, 56, 3), "(200,80,60) as (230,79,56): richer, as it should be");
  p (path, "adobe-grey.jpg");
  make_flat_jpeg (path, 48, 32, grey, icc, n, NULL, 0);
  decode_middle (path, px);
  CHECK (near3 (px, 129, 129, 129, 2), "grey stays grey");
  /* The Info tab's reader finds it from the file's first bytes. */
  f = fopen (path, "rb");
  n = fread (buf, 1, sizeof buf, f);
  fclose (f);
  CHECK (colour_from_jpeg (buf, n, name, sizeof name) == CS_ADOBE && strcmp (name, "Adobe RGB (1998)") == 0,
         "colour_from_jpeg: Adobe RGB, by name");

  /* Display P3 (an iPhone's), as a v4 profile. */
  n = make_icc (icc, "Display P3", 1);
  p (path, "p3-orange.jpg");
  make_flat_jpeg (path, 48, 32, orange, icc, n, NULL, 0);
  CHECK (decode_middle (path, px) == CS_P3 && near3 (px, 216, 69, 50, 3), "Display P3 (200,80,60) as sRGB (216,69,50)");

  /* No profile, but the camera's EXIF says Adobe RGB (uncalibrated + R03). */
  {
    unsigned char ex[200], *t = ex + 6;
    memset (ex, 0, sizeof ex);
    memcpy (ex, "Exif\0\0", 6);
    memcpy (t, "II", 2); put16 (t + 2, 42); put32 (t + 4, 8);
    /* IFD0: the EXIF IFD at 26 */
    put16 (t + 8, 1);
    put16 (t + 10, 0x8769); put16 (t + 12, 4); put32 (t + 14, 1); put32 (t + 18, 26);
    put32 (t + 22, 0);
    /* EXIF IFD: ColorSpace 0xFFFF, the interoperability IFD at 56 */
    put16 (t + 26, 2);
    put16 (t + 28, 0xA001); put16 (t + 30, 3); put32 (t + 32, 1); put16 (t + 36, 0xFFFF);
    put16 (t + 40, 0xA005); put16 (t + 42, 4); put32 (t + 44, 1); put32 (t + 48, 56);
    put32 (t + 52, 0);
    /* Interoperability: index "R03" */
    put16 (t + 56, 1);
    put16 (t + 58, 0x0001); put16 (t + 60, 2); put32 (t + 62, 4); memcpy (t + 66, "R03", 4);
    put32 (t + 70, 0);
    p (path, "exif-adobe.jpg");
    make_flat_jpeg (path, 48, 32, green, NULL, 0, ex, 6 + 74);
    CHECK (decode_middle (path, px) == CS_ADOBE && near3 (px, 0, 141, 50, 3), "EXIF uncalibrated + R03: Adobe RGB");
    /* ColorSpace sRGB (1): left alone. */
    put16 (t + 36, 1);
    p (path, "exif-srgb.jpg");
    make_flat_jpeg (path, 48, 32, green, NULL, 0, ex, 6 + 74);
    CHECK (decode_middle (path, px) == CS_SRGB && near3 (px, 60, 140, 60, 3), "EXIF sRGB: unchanged");
  }
  /* Nothing at all: sRGB, unchanged. */
  p (path, "plain.jpg");
  make_flat_jpeg (path, 48, 32, green, NULL, 0, NULL, 0);
  CHECK (decode_middle (path, px) == CS_SRGB && near3 (px, 60, 140, 60, 3), "no profile, no EXIF: sRGB, unchanged");
}

static void make_png (const char *path, int w, int h, int interlace, int alpha)
{
  FILE *f = fopen (path, "wb");
  png_structp p = png_create_write_struct (PNG_LIBPNG_VER_STRING, NULL, NULL, NULL);
  png_infop i = png_create_info_struct (p);
  unsigned char *row = malloc ((size_t) w * 4);
  int y, x, passes;
  png_init_io (p, f);
  png_set_IHDR (p, i, (png_uint_32) w, (png_uint_32) h, 8, alpha ? PNG_COLOR_TYPE_RGBA : PNG_COLOR_TYPE_RGB,
                interlace ? PNG_INTERLACE_ADAM7 : PNG_INTERLACE_NONE, PNG_COMPRESSION_TYPE_DEFAULT, PNG_FILTER_TYPE_DEFAULT);
  png_write_info (p, i);
  passes = png_set_interlace_handling (p);
  for (x = 0; x < w; x++)
    {
      unsigned char *px = row + x * (alpha ? 4 : 3);
      px[0] = 20; px[1] = (unsigned char) (x * 255 / w); px[2] = 40;
      if (alpha) px[3] = 128;
    }
  while (passes--)
    for (y = 0; y < h; y++)
      png_write_row (p, row);
  png_write_end (p, i);
  png_destroy_write_struct (&p, &i);
  fclose (f);
  free (row);
}

static void write_text (const char *path, const char *text)
{
  FILE *f = fopen (path, "w");
  fputs (text, f);
  fclose (f);
}

static void p (char *out, const char *rel)
{
  snprintf (out, 1024, "%s/%s", work, rel);
}

static unsigned clock_cs;
static unsigned fake_now (void) { return clock_cs += 1; }  /* every call: 10 ms later */
static unsigned frozen_now (void) { return 0; }            /* never moves */

static int thumbs_done;
static void on_thumb (void *ctx, cat_id v) { (void) ctx; (void) v; thumbs_done++; }

static void test_exif (void)
{
  unsigned char buf[2048];
  char path[1024];
  FILE *f;
  size_t n;
  img_info in;
  p (path, "exif.jpg");
  make_jpeg (path, 64, 48, 6, "2026:06:16 04:52:10");
  f = fopen (path, "rb");
  n = fread (buf, 1, sizeof buf, f);
  fclose (f);
  CHECK (img_probe (buf, n) == IMG_JPEG, "probe: JPEG");
  CHECK (img_read_info (buf, n, &in) == 0, "JPEG info read");
  CHECK (in.width == 64 && in.height == 48, "JPEG size from SOF");
  CHECK (in.orientation == 6, "EXIF orientation");
  CHECK (strcmp (in.taken, "2026:06:16 04:52:10") == 0, "DateTimeOriginal");
  CHECK (strcmp (in.make, "Pi") == 0, "Make (inline value)");
  CHECK (strcmp (in.model, "HQ Camera") == 0, "Model (trailing spaces trimmed)");
  CHECK (in.exposure > 0.0039 && in.exposure < 0.0041, "ExposureTime 1/250");
  CHECK (in.fnumber > 5.59 && in.fnumber < 5.61, "FNumber 5.6");
  CHECK (in.iso == 400, "ISO 400");
  CHECK (in.focal > 30.9 && in.focal < 31.1, "FocalLength 31");
  /* Every tag, for the Info tab. */
  {
    char all[2048];
    int k;
    all[0] = '\0';
    k = exif_list (buf, n, list_tag, all);
    CHECK (k == 8, "exif_list: eight tags (the EXIF pointer isn't one)");
    CHECK (strstr (all, "Camera|Orientation|Turned right;") != NULL, "Orientation 6 in words");
    CHECK (strstr (all, "Camera|Model|HQ Camera;") != NULL, "Model");
    CHECK (strstr (all, "Exif|Exposure time|1/250 s;") != NULL, "exposure as a fraction");
    CHECK (strstr (all, "Exif|F number|f/5.6;") != NULL, "f number");
    CHECK (strstr (all, "Exif|ISO|400;") != NULL && strstr (all, "Exif|Focal length|31 mm;") != NULL, "ISO, focal length");
    CHECK (strstr (all, "Exif|Date taken|2026:06:16 04:52:10;") != NULL, "date taken");
    for (k = 0; k < (int) n; k += 3)
      exif_list (buf, (size_t) k, list_tag, NULL);
    CHECK (1, "cut-off buffers listed safely");
  }
  /* Truncated buffers never crash, and say no. */
  for (n = 0; n < 200; n++)
    img_read_info (buf, n, &in);
  CHECK (img_read_info (buf, 20, &in) == -1, "a cut-off header is rejected");
  /* Garbage EXIF offsets stay in bounds. */
  {
    unsigned char bad[400];
    size_t i;
    memcpy (bad, buf, sizeof bad);
    for (i = 30; i < 200; i += 7)
      bad[i] = 0xFF;
    img_read_info (bad, sizeof bad, &in);
    CHECK (1, "garbage EXIF survived");
  }
  p (path, "info.png");
  make_png (path, 300, 120, 0, 0);
  f = fopen (path, "rb");
  n = fread (buf, 1, sizeof buf, f);
  fclose (f);
  CHECK (img_read_info (buf, n, &in) == 0 && in.format == IMG_PNG && in.width == 300 && in.height == 120,
         "PNG size from IHDR");
}

static void make_fixtures (void)
{
  char path[1024];
  p (path, "Lakes"); mkdir (path, 0777);
  p (path, "Lakes/Day1"); mkdir (path, 0777);
  p (path, "Lakes/!App"); mkdir (path, 0777);
  p (path, "Lakes/Day1/catbells.jpg"); make_jpeg (path, 600, 400, 1, "2026:06:16 04:52:10");
  p (path, "Lakes/Day1/portrait.jpg"); make_jpeg (path, 400, 200, 6, "2026:06:16 05:00:00");
  p (path, "Lakes/Day1/noexif.jpg"); make_jpeg (path, 300, 300, 1, NULL);
  p (path, "Lakes/wide.png"); make_png (path, 1000, 100, 0, 0);
  p (path, "Lakes/ilace.png"); make_png (path, 200, 400, 1, 1);
  p (path, "Lakes/notes.txt"); write_text (path, "not a photo");
  p (path, "Lakes/fake.jpg"); write_text (path, "not really a JPEG");
  p (path, "Lakes/!App/inside.jpg"); make_jpeg (path, 50, 50, 1, NULL);
  p (path, "single.jpg"); make_jpeg (path, 80, 60, 3, "2025:01:01 10:00:00");
}

static int find_item (const cat_item *items, int n, const char *name)
{
  int i;
  for (i = 0; i < n; i++)
    if (strcmp (items[i].name, name) == 0)
      return i;
  return -1;
}

static void test_library (void)
{
  library *l;
  char err[256], path[1024], libdir[1024];
  cat_item *items;
  int n, i, steps;
  cat_source all = { SRC_ALL, 0, 0, "" };
  lib_progress pr;

  p (libdir, "Library");
  CHECK (lib_open (&l, libdir, err, sizeof err) == 0, "library opens (and is created)");
  p (path, "Lakes");
  CHECK (lib_import (l, path) == 0, "a directory is queued");
  CHECK (lib_busy (l), "busy with an import");

  /* A frozen clock still makes progress: one step a call. */
  steps = 0;
  while (lib_busy (l) && steps < 10000)
    {
      lib_work (l, frozen_now, 0, on_thumb, NULL);
      steps++;
    }
  CHECK (steps > 10, "the import and thumbnails took many small steps");
  CHECK (!lib_busy (l), "and finished");
  lib_get_progress (l, &pr);
  CHECK (pr.added == 5, "5 images added (3 JPEGs, 2 PNGs)");
  CHECK (pr.skipped == 1, "the fake JPEG was skipped");
  CHECK (thumbs_done == 5, "5 thumbnails made");

  cat_list (lib_cat (l), &all, &items, &n);
  CHECK (n == 5, "the browser lists 5");
  CHECK (find_item (items, n, "inside.jpg") < 0, "nothing from inside an application directory");
  CHECK (n >= 2 && strcmp (items[0].name, "catbells.jpg") == 0 && strcmp (items[1].name, "portrait.jpg") == 0,
         "sorted by capture time");
  for (i = 0; i < n; i++)
    CHECK (items[i].thumb_state == THUMB_READY, "every thumbnail ready");

  /* Thumbnail sizes: fit 256, orientation applied. */
  {
    rgb_image img;
    int k = find_item (items, n, "portrait.jpg");
    lib_thumb_path (l, items[k].id, path, sizeof path);
    CHECK (img_load_jpeg (path, &img, err, sizeof err) == 0, "portrait thumbnail loads");
    CHECK (img.w == 128 && img.h == 256, "400x200 rotated 90: thumbnail 128x256");
    /* Orientation 6 turns the red left half to the top. */
    CHECK (img.rgb[(10 * img.w + 50) * 3] > 150 && img.rgb[(190 * img.w + 50) * 3] < 100,
           "rotated the right way (red at the top)");
    free (img.rgb);
    k = find_item (items, n, "wide.png");
    lib_thumb_path (l, items[k].id, path, sizeof path);
    CHECK (img_load_jpeg (path, &img, err, sizeof err) == 0 && img.w == 256 && img.h == 26, "1000x100 PNG: 256x26");
    free (img.rgb);
    k = find_item (items, n, "ilace.png");
    lib_thumb_path (l, items[k].id, path, sizeof path);
    CHECK (img_load_jpeg (path, &img, err, sizeof err) == 0 && img.w == 128 && img.h == 256, "interlaced RGBA PNG: 128x256");
    free (img.rgb);
    k = find_item (items, n, "catbells.jpg");
    lib_thumb_path (l, items[k].id, path, sizeof path);
    CHECK (img_load_jpeg (path, &img, err, sizeof err) == 0 && img.w == 256 && img.h == 171, "600x400 JPEG: 256x171");
    free (img.rgb);
  }

  /* Details. */
  {
    cat_details d;
    int k = find_item (items, n, "portrait.jpg");
    CHECK (cat_get_details (lib_cat (l), items[k].id, &d) == 0, "details read");
    CHECK (d.width == 400 && d.height == 200 && d.orientation == 6 && d.iso == 400, "details: size, orientation, ISO");
    CHECK (strcmp (d.model, "HQ Camera") == 0, "details: camera");
  }

  /* Search: words that all match the name, the camera, the date or the project. */
  {
    cat_source q = { SRC_ALL, 0, 0, "" };
    cat_item *found;
    int nf;
    static const struct { const char *text; int count; const char *first; } cases[] = {
      { "catbells", 1, "catbells.jpg" },     /* the name */
      { "CATBELLS", 1, "catbells.jpg" },     /* in any case */
      { "hq camera", 2, "catbells.jpg" },    /* the camera: both words */
      { "pi 05:00", 1, "portrait.jpg" },     /* camera and time together */
      { "2026-06-16", 2, "catbells.jpg" },   /* the date written with dashes */
      { "2026:06", 2, "catbells.jpg" },      /* or as EXIF writes it */
      { "lakes", 5, NULL },                  /* the project */
      { "png", 2, NULL },
      { "zebra", 0, NULL },
      { "50%", 0, NULL },                    /* LIKE's own characters, literally */
      { "c_t", 0, NULL },
      { "   ", 5, NULL },                    /* only spaces: everything */
    };
    size_t k;
    for (k = 0; k < sizeof cases / sizeof cases[0]; k++)
      {
        char what[120];
        snprintf (q.search, sizeof q.search, "%s", cases[k].text);
        nf = -1;
        cat_list (lib_cat (l), &q, &found, &nf);
        snprintf (what, sizeof what, "search \"%s\": %d found", cases[k].text, cases[k].count);
        CHECK (nf == cases[k].count && (!cases[k].first || (nf > 0 && strcmp (found[0].name, cases[k].first) == 0)), what);
        free (found);
      }
  }

  /* Captions and keywords: set on several at once, found by search. */
  {
    cat *c = lib_cat (l);
    cat_id two[2], one;
    char cap[CAT_CAPTION_MAX], kw[CAT_KEYWORDS_MAX];
    int same = -1, nf, k;
    cat_item *found;
    cat_keyword *kws;
    cat_source q = { SRC_ALL, 0, 0, "" };
    two[0] = items[find_item (items, n, "catbells.jpg")].id;
    two[1] = items[find_item (items, n, "portrait.jpg")].id;
    one = items[find_item (items, n, "wide.png")].id;
    CHECK (cat_get_tags (c, one, cap, sizeof cap, kw, sizeof kw) == 0 && !cap[0] && !kw[0], "no caption or keywords at first");
    CHECK (cat_add_keywords (c, two, 2, " Fells, walking ;fells,, Derwent Water ") == 0, "keywords added to two");
    CHECK (cat_add_keywords (c, &one, 1, "Walking, sunset") == 0, "and others to one");
    cat_get_tags (c, two[0], cap, sizeof cap, kw, sizeof kw);
    CHECK (!strcmp (kw, "Derwent Water, Fells, walking"), "trimmed, sorted, one of each (case aside)");
    cat_get_tags (c, one, cap, sizeof cap, kw, sizeof kw);
    CHECK (!strcmp (kw, "sunset, walking"), "a keyword keeps the case it was first given");
    CHECK (cat_set_caption (c, two, 2, "Morning above Keswick") == 0, "caption set on two");
    cat_common_tags (c, two, 2, cap, sizeof cap, &same, kw, sizeof kw);
    CHECK (same == 1 && !strcmp (cap, "Morning above Keswick") && !strcmp (kw, "Derwent Water, Fells, walking"),
           "in common: the caption and all three keywords");
    {
      cat_id three[3] = { two[0], two[1], one };
      cat_common_tags (c, three, 3, cap, sizeof cap, &same, kw, sizeof kw);
      CHECK (same == 0 && !cap[0] && !strcmp (kw, "walking"), "with the third: captions differ, one keyword in common");
    }
    CHECK (cat_keywords (c, &kws, &k) == 0 && k == 4 && !strcmp (kws[0].name, "Derwent Water") && kws[3].count == 3,
           "the keywords in use, with their counts");
    free (kws);
    strcpy (q.search, "derwent");
    cat_list (c, &q, &found, &nf);
    CHECK (nf == 2, "search finds keywords");
    free (found);
    strcpy (q.search, "keswick morning");
    cat_list (c, &q, &found, &nf);
    CHECK (nf == 2, "and captions");
    free (found);
    CHECK (cat_remove_keywords (c, &one, 1, "SUNSET") == 0, "a keyword taken away (any case)");
    cat_keywords (c, &kws, &k);
    CHECK (k == 3, "and gone from the list when nobody has it");
    free (kws);
  }

  /* Keywords: a long one cut at a whole UTF-8 character; long captions compared whole. */
  {
    char kw[200], out[1024], cap[16];
    int same = 0;
    cat_id v0 = items[0].id, v1 = items[1].id, both[2] = { items[0].id, items[1].id };
    memset (kw, 'a', 62);
    kw[62] = '\0';
    strcat (kw, "\xC3\xA9xyz");           /* an e-acute straddling the 63-byte limit */
    cat_add_keywords (lib_cat (l), &v0, 1, kw);
    cat_get_tags (lib_cat (l), v0, NULL, 0, out, sizeof out);
    CHECK (strncmp (out, kw, 62) == 0 && (out[62] == ',' || out[62] == '\0'),
           "a keyword too long is cut before a character it can't hold whole");
    kw[62] = '\0';
    cat_remove_keywords (lib_cat (l), &v0, 1, kw);
    memset (kw, 'b', 100);
    kw[100] = '\0';
    cat_set_caption (lib_cat (l), both, 2, kw);
    cat_common_tags (lib_cat (l), both, 2, cap, sizeof cap, &same, NULL, 0);
    CHECK (same == 1, "the same long caption on two: the same, though the copy asked for is short");
    cat_set_caption (lib_cat (l), both, 2, "");
    (void) v1;
  }

  /* Colour labels: set on two, found by search (by the label's name), shown in the list. */
  {
    cat_id both[2] = { items[0].id, items[1].id };
    cat_source q = { SRC_ALL, 0, 0, "orange" };
    cat_item *found;
    int nf, k2, labelled = 0;
    CHECK (cat_set_label (lib_cat (l), both, 2, CAT_LABEL_ORANGE) == 0, "two given the orange label");
    cat_list (lib_cat (l), &q, &found, &nf);
    CHECK (nf == 2 && found[0].label == CAT_LABEL_ORANGE, "search \"orange\" finds them, with their label");
    free (found);
    strcpy (q.search, "");
    cat_list (lib_cat (l), &q, &found, &nf);
    for (k2 = 0; k2 < nf; k2++)
      labelled += found[k2].label != 0;
    CHECK (labelled == 2, "the others have none");
    free (found);
    CHECK (cat_set_label (lib_cat (l), both, 2, 99) == 0, "a label out of range: none");
    cat_list (lib_cat (l), &q, &found, &nf);
    CHECK (found[0].label == 0, "none");
    free (found);
    CHECK (!strcmp (cat_label_names[CAT_LABEL_GREY], "Grey"), "the names");
  }

  /* Dates as typed, for smart albums. */
  {
    static const struct { const char *in; int end; const char *out; } d[] = {
      { "2026-06-16", 0, "2026-06-16" }, { "2026:6:3", 0, "2026-06-03" }, { "3/5/2026", 0, "2026-05-03" },
      { "2026", 0, "2026-01-01" }, { "2026", 1, "2026-12-31" }, { "2026-02", 1, "2026-02-31" },
      { "5/2026", 0, "2026-05-01" }, { "", 0, "" }, { " 2026-6-1 ", 0, "2026-06-01" },
    };
    static const char *const bad[] = { "16/06/26", "2026-13-01", "June", "2026-06-16x", "1/2/3" };
    size_t k;
    char out[11], what[80];
    for (k = 0; k < sizeof d / sizeof d[0]; k++)
      {
        snprintf (what, sizeof what, "date \"%s\" is %s", d[k].in, d[k].out);
        CHECK (cat_date_normal (d[k].in, d[k].end, out) == 0 && !strcmp (out, d[k].out), what);
      }
    for (k = 0; k < sizeof bad / sizeof bad[0]; k++)
      {
        snprintf (what, sizeof what, "\"%s\" is not a date", bad[k]);
        CHECK (cat_date_normal (bad[k], 0, out) == -1, what);
      }
  }

  /* Smart albums: whatever matches the rule now. */
  {
    cat *c = lib_cat (l);
    cat_rule r, back;
    char text[256];
    cat_id smart, one = items[find_item (items, n, "wide.png")].id, cb = items[find_item (items, n, "catbells.jpg")].id;
    cat_source src = { SRC_ALBUM, 0, 0, "" };
    cat_item *found;
    cat_group *g;
    int nf, ng, k, seen = 0;
    memset (&r, 0, sizeof r);
    strcpy (r.words, "walking");
    smart = cat_smart_create (c, "Walks", &r);
    CHECK (smart > 0 && cat_smart_get (c, smart, &back) == 1 && !strcmp (back.words, "walking"), "a smart album keeps its rule");
    src.id = smart;
    cat_list (c, &src, &found, &nf);
    CHECK (nf == 3, "Walks: the three with the keyword");
    free (found);
    CHECK (cat_album_add (c, smart, &cb, 1) == -1 && strstr (cat_error (c), "smart") != NULL,
           "photos can't be put in one by hand");
    strcpy (r.from, "2026-06-01");
    strcpy (r.to, "2026-06-30");
    cat_smart_set (c, smart, &r);
    cat_list (c, &src, &found, &nf);
    CHECK (nf == 2, "taken in June 2026: two (the PNG has no date)");
    free (found);
    r.from[0] = r.to[0] = '\0';
    r.flagged = 1;
    cat_smart_set (c, smart, &r);
    cat_set_flag (c, &one, 1, 1);
    cat_list (c, &src, &found, &nf);
    CHECK (nf == 1 && found[0].id == one, "flagged only: the one flagged");
    free (found);
    cat_set_flag (c, &one, 1, 0);
    r.flagged = 0;
    r.min_rating = 2;
    cat_smart_set (c, smart, &r);
    cat_set_rating (c, &cb, 1, 3);
    cat_list (c, &src, &found, &nf);
    CHECK (nf == 1 && found[0].id == cb, "2 stars and up: the one rated 3");
    free (found);
    cat_groups (c, SRC_ALBUM, &g, &ng);
    for (k = 0; k < ng; k++)
      if (g[k].id == smart)
        seen = g[k].smart && g[k].count == 1;
    CHECK (seen, "listed with the albums as smart, with its count now");
    free (g);
    {
      cat_id *vs;
      int nv;
      CHECK (cat_group_versions (c, SRC_ALBUM, smart, &vs, &nv) == 0 && nv == 1 && vs[0] == cb, "its photos, for exporting");
      free (vs);
    }
    /* Rule words aren't dropped when there's a search too. */
    {
      cat_rule r2 = r;
      cat_source q2 = { SRC_ALBUM, smart, 0, "fells" };
      cat_id cb2 = cb;
      cat_add_keywords (c, &cb2, 1, "k1, k2, k3, k4, k5");
      cat_add_keywords (c, &one, 1, "k1, k2, k3, k4");
      memset (&r2, 0, sizeof r2);
      strcpy (r2.words, "k1 k2 k3 k4 k5");
      cat_smart_set (c, smart, &r2);
      strcpy (q2.search, "k1 k2 k3 k4");
      cat_list (c, &q2, &found, &nf);
      CHECK (nf == 1 && found[0].id == cb, "5 rule words and 4 searched: all count");
      free (found);
      cat_remove_keywords (c, &cb2, 1, "k1, k2, k3, k4, k5");
      cat_remove_keywords (c, &one, 1, "k1, k2, k3, k4");
      cat_smart_set (c, smart, &r);
    }
    r.edited = 1;
    strcpy (r.from, "2026");
    cat_rule_format (&r, text, sizeof text);
    cat_rule_parse (text, &back);
    CHECK (!memcmp (&r, &back, sizeof r) || (back.min_rating == 2 && back.edited && !strcmp (back.from, "2026-01-01")),
           "a rule survives being written as text");
    cat_set_rating (c, &cb, 1, 0);
    CHECK (cat_album_delete (c, smart) == 0, "a smart album deleted");
    /* Captions and keywords go back to none for what follows. */
    {
      cat_id all3[3] = { items[find_item (items, n, "catbells.jpg")].id, items[find_item (items, n, "portrait.jpg")].id, one };
      cat_set_caption (c, all3, 3, "");
      cat_remove_keywords (c, all3, 3, "Derwent Water, Fells, walking");
      cat_keyword *kws;
      cat_keywords (c, &kws, &k);
      CHECK (k == 0, "no keywords left");
      free (kws);
    }
  }

  /* Versions and stacks. */
  {
    cat *c = lib_cat (l);
    cat_id cb = items[find_item (items, n, "catbells.jpg")].id, pt = items[find_item (items, n, "portrait.jpg")].id;
    cat_id v2, v3, alb;
    cat_item *it;
    int ni, k, before;
    char cap[512], kw[1024], rec[256];
    cat_details d;
    cat_source all2 = { SRC_ALL, 0, 0, "" }, srch = { SRC_ALL, 0, 0, "catbells" };
    cat_list (c, &all2, &it, &before);
    free (it);
    cat_set_recipe (c, cb, "exposure ev=0.50\n");
    cat_set_caption (c, &cb, 1, "Catbells");
    cat_add_keywords (c, &cb, 1, "fells");
    alb = cat_album_create (c, "Versions");
    cat_album_add (c, alb, &cb, 1);
    v2 = cat_new_version (c, cb, 1);
    CHECK (v2 > 0, "a duplicate version");
    cat_get_recipe (c, v2, rec, sizeof rec);
    cat_get_tags (c, v2, cap, sizeof cap, kw, sizeof kw);
    CHECK (strstr (rec, "exposure ev=0.50") != NULL && !strcmp (cap, "Catbells") && !strcmp (kw, "fells"),
           "with the adjustments, caption and keywords");
    {
      cat_source a = { SRC_ALBUM, alb, 0, "" };
      cat_list (c, &a, &it, &ni);
      CHECK (ni == 2, "and in the same album");
      free (it);
    }
    v3 = cat_new_version (c, cb, 0);
    cat_get_recipe (c, v3, rec, sizeof rec);
    CHECK (v3 > 0 && !rec[0], "a new version from the original: no adjustments");
    cat_list (c, &all2, &it, &ni);
    k = find_item (it, ni, "catbells.jpg");
    CHECK (ni == before + 2 && k >= 0 && it[k].stack && it[k].stack_count == 3 && it[k].stack_pick && it[k].stack_open,
           "the three stacked, open, catbells the pick");
    CHECK (k + 2 < ni && !strcmp (it[k + 1].name, "catbells_v2.jpg") && !strcmp (it[k + 2].name, "catbells_v3.jpg")
           && !it[k + 1].stack_pick, "listed together, the pick first, named _v2 and _v3");
    free (it);
    CHECK (cat_get_details (c, v3, &d) == 0 && d.versions == 3 && d.version_no == 3 && d.original
           && cat_count_versions (c, d.original) == 3, "version 3 of 3 of its original");
    /* Closed: the pick alone. A search still finds each. */
    cat_stack_open (c, cb, 0);
    cat_list (c, &all2, &it, &ni);
    CHECK (ni == before && find_item (it, ni, "catbells_v2.jpg") < 0 && it[find_item (it, ni, "catbells.jpg")].stack_count == 3,
           "closed: only the pick shows, with its count");
    free (it);
    cat_list (c, &srch, &it, &ni);
    CHECK (ni == 3, "a search finds all three");
    free (it);
    /* Another pick. */
    cat_stack_pick (c, v2);
    cat_list (c, &all2, &it, &ni);
    CHECK (find_item (it, ni, "catbells_v2.jpg") >= 0 && find_item (it, ni, "catbells.jpg") < 0, "v2 the pick: it stands for them");
    free (it);
    /* Stacking two different photos merges whole stacks. */
    {
      cat_id two[2] = { pt, v3 };
      cat_id st = cat_stack (c, two, 2, v3);
      cat_list (c, &all2, &it, &ni);
      k = find_item (it, ni, "catbells_v3.jpg");
      CHECK (st > 0 && k >= 0 && it[k].stack_count == 4 && it[k].stack_pick && find_item (it, ni, "portrait.jpg") < 0,
             "a stack with portrait: v3's stack-mates come too");
      free (it);
    }
    /* Removing the pick: the stack keeps going with a new pick. */
    cat_remove_versions (c, &v3, 1);
    cat_list (c, &all2, &it, &ni);
    {
      int picks = 0, j;
      for (j = 0; j < ni; j++)
        if (it[j].stack_count == 3 && it[j].stack_pick)
          picks++;
      CHECK (picks == 1 && find_item (it, ni, "catbells_v3.jpg") < 0, "its pick removed: three left, one the pick (closed: it alone)");
    }
    free (it);
    /* Unstacking all but one: the stack goes. */
    {
      cat_id two[2] = { v2, pt };
      cat_unstack (c, two, 2);
      cat_list (c, &srch, &it, &ni);
      CHECK (ni == 2 && !it[0].stack && !it[1].stack, "unstacked: none left in a stack");
      free (it);
    }
    /* Auto-stack: catbells and its version share a time; the others don't. */
    {
      cat_id *vs;
      int nv, made;
      cat_list (c, &all2, &it, &ni);
      vs = malloc (sizeof *vs * (size_t) ni);
      for (k = 0; k < ni; k++)
        vs[k] = it[k].id;
      made = cat_auto_stack (c, vs, ni, 5);
      free (it);
      cat_list (c, &all2, &it, &nv);
      k = find_item (it, nv, "catbells.jpg");
      CHECK (made == 1 && k >= 0 && it[k].stack_count == 2 && it[find_item (it, nv, "portrait.jpg")].stack == 0,
             "auto-stack within 5 s: catbells and its version (portrait, 8 minutes on, alone)");
      CHECK (cat_auto_stack (c, vs, ni, 5) == 0, "again: they are one stack already, none made");
      free (it);
      free (vs);
    }
    /* Regressions from review. A closed stack whose pick the view leaves
       out (rejected) still shows one photo, with the badge. */
    {
      cat_id two[2] = { cb, v2 }, st;
      int k2, shown = 0;
      cat_unstack_all (c, &cb, 1);
      st = cat_stack (c, two, 2, cb);
      cat_stack_open (c, cb, 0);
      cat_set_rating (c, &cb, 1, RATING_REJECT);
      cat_list (c, &all2, &it, &ni);
      for (k2 = 0; k2 < ni; k2++)
        if (it[k2].stack == st)
          shown++;
      CHECK (st > 0 && shown == 1 && find_item (it, ni, "catbells_v2.jpg") >= 0,
             "closed stack, pick rejected: the other shows for it");
      free (it);
      cat_set_rating (c, &cb, 1, 0);
      CHECK (cat_unstack_all (c, &v2, 1) == 0, "unstack all");
      cat_list (c, &all2, &it, &ni);
      CHECK (it[find_item (it, ni, "catbells.jpg")].stack == 0, "the whole stack taken apart from one of it");
      free (it);
      CHECK (cat_stack (c, (cat_id[2]) { v2, v2 }, 2, v2) == -1, "the same photo twice: no stack");
    }
    /* Back as it was for what follows: the versions go. */
    {
      cat_remove_versions (c, &v2, 1);
      cat_list (c, &all2, &it, &ni);
      k = find_item (it, ni, "catbells.jpg");
      CHECK (k >= 0 && !it[k].stack, "the versions removed: no stack left");
      free (it);
      cat_set_recipe (c, cb, "");
      cat_set_caption (c, &cb, 1, "");
      cat_remove_keywords (c, &cb, 1, "fells");
      cat_album_delete (c, alb);
    }
  }

  /* Ratings, flags, rejects, albums. */
  {
    cat_id ids[2];
    cat_source flagged = { SRC_FLAGGED, 0, 0, "" }, rejected = { SRC_REJECTED, 0, 0, "" }, three = { SRC_ALL, 0, 3, "" };
    cat_item *it2;
    int n2;
    cat_id album;
    ids[0] = items[0].id;
    ids[1] = items[1].id;
    CHECK (cat_set_rating (lib_cat (l), ids, 2, 4) == 0, "rate two 4 stars");
    CHECK (cat_set_flag (lib_cat (l), ids, 1, -1) == 0, "toggle a flag");
    cat_list (lib_cat (l), &flagged, &it2, &n2);
    CHECK (n2 == 1 && it2[0].id == ids[0] && it2[0].rating == 4, "Flagged lists the flagged one");
    free (it2);
    cat_list (lib_cat (l), &three, &it2, &n2);
    CHECK (n2 == 2, "3 stars and up: the two");
    free (it2);
    cat_set_rating (lib_cat (l), &items[4].id, 1, RATING_REJECT);
    cat_list (lib_cat (l), &all, &it2, &n2);
    CHECK (n2 == 4, "a reject leaves All photos");
    free (it2);
    cat_list (lib_cat (l), &rejected, &it2, &n2);
    CHECK (n2 == 1 && it2[0].id == items[4].id, "and shows under Rejected");
    free (it2);
    album = cat_album_create (lib_cat (l), "Best of 2026");
    CHECK (album > 0, "album created");
    CHECK (cat_album_add (lib_cat (l), album, ids, 2) == 0 && cat_album_add (lib_cat (l), album, ids, 1) == 0,
           "add to album (twice is harmless)");
    {
      cat_source a = { SRC_ALBUM, album, 0, "" };
      cat_list (lib_cat (l), &a, &it2, &n2);
      CHECK (n2 == 2, "the album holds 2");
      free (it2);
      cat_album_remove (lib_cat (l), album, ids, 1);
      cat_list (lib_cat (l), &a, &it2, &n2);
      CHECK (n2 == 1, "and 1 after removing one");
      free (it2);
    }
  }
  free (items);

  /* Projects: one per dropped directory. */
  {
    cat_group *g;
    int ng;
    cat_groups (lib_cat (l), SRC_PROJECT, &g, &ng);
    CHECK (ng == 1 && strcmp (g[0].name, "Lakes") == 0 && g[0].count == 4, "project Lakes, 4 not rejected");
    free (g);
  }

  /* The same directory again: nothing new. A single file: its directory names the project. */
  p (path, "Lakes");
  lib_import (l, path);
  p (path, "single.jpg");
  lib_import (l, path);
  clock_cs = 0;
  while (lib_busy (l))
    lib_work (l, fake_now, clock_cs + 3, NULL, NULL);
  lib_get_progress (l, &pr);
  CHECK (pr.added == 1 && pr.skipped == 6, "re-import: 1 new, 5 already there + the fake JPEG");
  {
    cat_source last = { SRC_LAST_IMPORT, 0, 0, "" };
    cat_list (lib_cat (l), &last, &items, &n);
    CHECK (n == 1 && strcmp (items[0].name, "single.jpg") == 0, "Last import shows the new one");
    free (items);
  }
  /* A photo tagged elsewhere: its XMP caption and keywords come in with it. */
  {
    char src[1024], dst[1024], cap[512], kw[1024];
    unsigned char *buf = malloc (1 << 20), xmp[2048];
    size_t got = 0, xn;
    FILE *f;
    cat_source last = { SRC_LAST_IMPORT, 0, 0, "" };
    p (src, "Lakes/Day1/catbells.jpg");
    p (dst, "Tagged");
    mkdir (dst, 0777);
    p (dst, "Tagged/tagged.jpg");
    f = fopen (src, "rb");
    if (f && buf)
      {
        got = fread (buf, 1, 1 << 20, f);
        fclose (f);
      }
    memcpy (xmp, XMP_NAMESPACE, sizeof XMP_NAMESPACE);
    xn = sizeof XMP_NAMESPACE + xmp_packet ("Catbells at dawn", "Catbells, fells", (char *) xmp + sizeof XMP_NAMESPACE,
                                             sizeof xmp - sizeof XMP_NAMESPACE);
    f = fopen (dst, "wb");
    if (f && got > 2)
      {
        /* SOI, then the XMP's APP1, then the rest. */
        fwrite (buf, 1, 2, f);
        fputc (0xFF, f); fputc (0xE1, f); fputc ((int) ((xn + 2) >> 8), f); fputc ((int) ((xn + 2) & 255), f);
        fwrite (xmp, 1, xn, f);
        fwrite (buf + 2, 1, got - 2, f);
        fclose (f);
      }
    free (buf);
    p (path, "Tagged");
    lib_import (l, path);
    while (lib_busy (l))
      lib_work (l, fake_now, clock_cs + 3, NULL, NULL);
    cat_list (lib_cat (l), &last, &items, &n);
    CHECK (n == 1 && cat_get_tags (lib_cat (l), items[0].id, cap, sizeof cap, kw, sizeof kw) == 0
           && !strcmp (cap, "Catbells at dawn") && !strcmp (kw, "Catbells, fells"),
           "a photo with XMP: its caption and keywords imported");
    if (n == 1)
      cat_remove_versions (lib_cat (l), &items[0].id, 1);
    free (items);
    remove (dst);
  }
  lib_close (l);

  /* Everything survives closing and reopening. */
  CHECK (lib_open (&l, libdir, err, sizeof err) == 0, "library reopens");
  cat_list (lib_cat (l), &all, &items, &n);
  CHECK (n == 5, "5 in All photos after reopening (6 minus the reject)");
  CHECK (!lib_busy (l), "nothing to do after reopening");
  free (items);

  /* A photo moved elsewhere: marked missing, then found again by
     importing the folder it is in now (no duplicate). */
  {
    char old[1024], moved[1024];
    cat_source miss = { SRC_MISSING, 0, 0, "" };
    cat_details d;
    cat_group *g;
    int ng, k, guard = 0;
    cat_id wide = 0;
    p (old, "Lakes/wide.png");
    p (moved, "Moved");
    mkdir (moved, 0777);
    p (moved, "Moved/wide.png");
    CHECK (rename (old, moved) == 0, "wide.png moved to Moved/");
    CHECK (lib_verifying (l), "checking the originals started when the library opened");
    while (lib_verifying (l) && guard++ < 100)
      lib_verify_step (l, frozen_now, 0);
    lib_verify_start (l);
    guard = 0;
    while (lib_verifying (l) && guard++ < 100)
      k = lib_verify_step (l, frozen_now, 0);
    cat_list (lib_cat (l), &miss, &items, &n);
    CHECK (n == 1 && strcmp (items[0].name, "wide.png") == 0 && items[0].missing, "wide.png is missing");
    CHECK (cat_count_missing (lib_cat (l)) == 1, "one missing");
    CHECK (cat_get_details (lib_cat (l), items[0].id, &d) == 0 && d.missing, "its details say so");
    wide = items[0].id;
    free (items);
    p (path, "Moved");
    lib_import (l, path);
    while (lib_busy (l))
      lib_work (l, fake_now, clock_cs + 3, NULL, NULL);
    lib_get_progress (l, &pr);
    CHECK (pr.relinked == 1 && pr.added == 0, "importing Moved: found again, nothing added");
    CHECK (cat_count_missing (lib_cat (l)) == 0, "no longer missing");
    cat_list (lib_cat (l), &all, &items, &n);
    CHECK (n == 5 && cat_get_details (lib_cat (l), wide, &d) == 0 && strcmp (d.path, moved) == 0 && !d.missing,
           "nothing new in All photos; wide.png (rejected) is at its new place");
    free (items);
    cat_groups (lib_cat (l), SRC_PROJECT, &g, &ng);
    for (k = 0; k < ng; k++)
      CHECK (strcmp (g[k].name, "Moved") != 0, "no empty Moved project left behind");
    free (g);
    /* Moved back by hand: the check finds it there again. */
    CHECK (rename (moved, old) == 0, "moved back");
    lib_verify_start (l);
    guard = 0;
    while (lib_verifying (l) && guard++ < 100)
      lib_verify_step (l, frozen_now, 0);
    CHECK (cat_count_missing (lib_cat (l)) == 1, "at the old place it's missing from the new one");
    lib_import (l, old);
    while (lib_busy (l))
      lib_work (l, fake_now, clock_cs + 3, NULL, NULL);
    CHECK (cat_count_missing (lib_cat (l)) == 0, "and importing it puts it right");
  }

  /* Removing, deleting and renaming. */
  {
    cat_group *g;
    int ng, k, before_files;
    cat_id album = cat_album_create (lib_cat (l), "Keep"), *vs;
    cat_source albsrc = { SRC_ALBUM, 0, 0, "" };
    FILE *f;
    cat_list (lib_cat (l), &all, &items, &n);
    k = find_item (items, n, "catbells.jpg");
    cat_album_add (lib_cat (l), album, &items[k].id, 1);
    lib_thumb_path (l, items[k].id, path, sizeof path);
    f = fopen (path, "rb");
    before_files = f != NULL;
    if (f) fclose (f);
    CHECK (before_files, "catbells has a thumbnail file");
    k = lib_remove (l, &items[k].id, 1);
    CHECK (k == 0, "remove catbells from the library");
    f = fopen (path, "rb");
    CHECK (!f, "its thumbnail file went");
    if (f) fclose (f);
    p (path, "Lakes/Day1/catbells.jpg");
    f = fopen (path, "rb");
    CHECK (f != NULL, "the photo's own file is still there");
    if (f) fclose (f);
    free (items);
    cat_list (lib_cat (l), &all, &items, &n);
    CHECK (n == 4 && find_item (items, n, "catbells.jpg") < 0, "4 left in All photos");
    free (items);
    albsrc.id = album;
    cat_list (lib_cat (l), &albsrc, &items, &n);
    CHECK (n == 0, "and out of the album");
    free (items);
    /* It can be imported again (its record went with it). */
    lib_import (l, path);
    while (lib_busy (l))
      lib_work (l, fake_now, clock_cs + 3, NULL, NULL);
    cat_list (lib_cat (l), &all, &items, &n);
    CHECK (n == 5 && find_item (items, n, "catbells.jpg") >= 0, "imported again");
    free (items);

    /* Renaming: projects' names are their own. */
    cat_groups (lib_cat (l), SRC_PROJECT, &g, &ng);
    CHECK (ng == 3, "three projects: Day1 (catbells, back alone), Lakes, the single photo's");
    for (k = 0; k < ng && strcmp (g[k].name, "Lakes") != 0; k++)
      ;
    CHECK (k < ng && cat_group_rename (lib_cat (l), SRC_PROJECT, g[k].id, "Lake District") == 0, "rename a project");
    CHECK (cat_group_rename (lib_cat (l), SRC_PROJECT, g[k == 0 ? 1 : 0].id, "Lake District") == -1
           && strstr (cat_error (lib_cat (l)), "already") != NULL, "not to another's name");
    CHECK (cat_group_rename (lib_cat (l), SRC_ALBUM, album, "  ") == -1, "nor to nothing");
    CHECK (cat_group_rename (lib_cat (l), SRC_ALBUM, album, "Favourites") == 0, "rename an album");
    {
      char nm[64];
      cat_group_name (lib_cat (l), SRC_ALBUM, album, nm, sizeof nm);
      CHECK (strcmp (nm, "Favourites") == 0, "the album's new name");
    }
    free (g);

    /* Delete an album: its photos stay. */
    cat_list (lib_cat (l), &all, &items, &n);
    cat_album_add (lib_cat (l), album, &items[0].id, 2);
    free (items);
    CHECK (cat_album_delete (lib_cat (l), album) == 0, "delete the album");
    cat_groups (lib_cat (l), SRC_ALBUM, &g, &ng);
    for (k = 0; k < ng; k++)
      CHECK (g[k].id != album, "it is gone");
    free (g);
    cat_list (lib_cat (l), &all, &items, &n);
    CHECK (n == 5, "its photos are still in the library");
    free (items);

    /* Remove a whole project: its versions, then the project itself. */
    cat_groups (lib_cat (l), SRC_PROJECT, &g, &ng);
    for (k = 0; k < ng; k++)
      if (strcmp (g[k].name, "Lake District") == 0)
        break;
    if (k < ng) cat_group_versions (lib_cat (l), SRC_PROJECT, g[k].id, &vs, &n);
    CHECK (k < ng && n == 4,
           "the project's versions, its reject too (catbells came back alone, into Day1)");
    CHECK (lib_remove (l, vs, n) == 0, "removed");
    free (vs);
    free (g);
    cat_groups (lib_cat (l), SRC_PROJECT, &g, &ng);
    CHECK (ng == 2, "the emptied project went; Day1 and the single photo's stay");
    free (g);
  }
  lib_close (l);
}

static void test_time_slicing (void)
{
  library *l;
  char err[256], path[1024], libdir[1024];
  int calls = 0, rc;
  unsigned before;
  p (path, "big.jpg");
  make_jpeg (path, 3000, 2000, 1, "2026:01:01 00:00:00");
  p (libdir, "Library2");
  lib_open (&l, libdir, err, sizeof err);
  lib_import (l, path);
  clock_cs = 0;
  while (lib_busy (l) && calls < 100000)
    {
      before = clock_cs;
      rc = lib_work (l, fake_now, clock_cs + 2, NULL, NULL);
      CHECK (clock_cs - before <= 3, "each call stops at its deadline");
      (void) rc;
      calls++;
    }
  CHECK (calls > 5, "a 6 MP thumbnail is made over several calls");
  lib_close (l);
}

static unsigned stack_clock;
static unsigned stack_now (void) { return stack_clock += 1; }

/* --- camera raw files through the plug-in (when LUMENRAW and RAW_SAMPLES are set) */

/* Runs the helper there and then (on RISC OS it runs in a task window). */
static int run_now (const char *cmd, int tag)
{
  int r = system (cmd);
  rawjob_ended (tag);
  return r < 0 ? -1 : 0;
}

static char last_cmd[2400];
static int run_and_keep (const char *cmd, int tag)
{
  snprintf (last_cmd, sizeof last_cmd, "%s", cmd);
  return run_now (cmd, tag);
}

static int runs;
static int run_counting (const char *cmd, int tag)
{
  runs++;
  return run_now (cmd, tag);
}

static int ran_nothing (const char *cmd, int tag)
{
  (void) cmd;
  rawjob_ended (tag);                  /* as if it had died at once */
  return 0;
}

static int sample (char *out, size_t n, const char *name)
{
  const char *dir = getenv ("RAW_SAMPLES");
  FILE *f;
  if (!dir)
    return 0;
  snprintf (out, n, "%s/%s", dir, name);
  f = fopen (out, "rb");
  if (f)
    fclose (f);
  return f != NULL;
}

static void test_raw (void)
{
  char nef[700], cr2[700], c400[700], err[300], scratch[700], path[1024];
  unsigned char head[64];
  const char *helper = getenv ("LUMENRAW");
  thumbgen *t;
  rgb_image img;
  img_info ri;
  FILE *f;
  int r, w, h;
  if (!helper || !sample (nef, sizeof nef, "iss030e122639.NEF") || !sample (cr2, sizeof cr2, "RAW_CANON_40D_SRAW_V103.CR2")
      || !sample (c400, sizeof c400, "M0054341_01_00005.cr2"))
    {
      printf ("test_core: no LUMENRAW or RAW_SAMPLES: raw files not tested\n");
      return;
    }
  /* Recognised: Canon's by signature, Nikon's (TIFF inside) by name. */
  f = fopen (cr2, "rb");
  r = (int) fread (head, 1, sizeof head, f);
  fclose (f);
  CHECK (img_probe_named (head, (size_t) r, "photo") == IMG_RAW, "CR2: raw by its signature, whatever its name");
  f = fopen (nef, "rb");
  r = (int) fread (head, 1, sizeof head, f);
  fclose (f);
  CHECK (img_probe_named (head, (size_t) r, "ADFS::4.$.Photos.DSC_0001/NEF") == IMG_RAW, "NEF: raw by its /NEF name (RISC OS)");
  CHECK (img_probe_named (head, (size_t) r, "/home/x/DSC_0001.nef") == IMG_RAW, "and by .nef (POSIX)");
  CHECK (img_probe_named (head, (size_t) r, "picture/tif") == IMG_TIFF, "a NEF with another name: a TIFF by its first bytes...");
  f = fopen (nef, "rb");
  CHECK (f && tiff_is_raw (f), "...and raw once its directory is read (a thumbnail first, the photo in sub-IFDs)");
  if (f)
    fclose (f);

  /* No plug-in: a clear message. */
  rawjob_setup (NULL, NULL, NULL);
  t = thumbgen_start (nef, NULL, 256, 1, err, sizeof err);
  CHECK (!t && strstr (err, "LumenRaw") && strstr (err, "PackMan"), "without the plug-in: install LumenRaw from PackMan");

  p (scratch, "RawScratch");
  rawjob_setup (helper, scratch, run_now);
  CHECK (rawjob_available (), "with it: available");
  /* A thumbnail: the camera's own preview, made small. */
  t = thumbgen_start (nef, NULL, 256, 1, err, sizeof err);
  CHECK (t != NULL, "a raw thumbnail starts");
  while (t && (r = thumbgen_step (t, 64)) == 0)
    ;
  CHECK (t && r == 1, "and finishes");
  CHECK (t && thumbgen_raw_details (t, &ri) && ri.width == 4284 && ri.height == 2844 && strcmp (ri.make, "Nikon") == 0
         && strcmp (ri.model, "D3S") == 0 && ri.iso == 3200 && strcmp (ri.taken, "2012:03:04 17:20:59") == 0,
         "the plug-in's details: 4284 x 2844, Nikon D3S, ISO 3200, the date");
  thumbgen_size (t, &w, &h);
  CHECK (w == 256 && h > 160 && h < 175, "a 256-wide thumbnail");
  CHECK (t && !thumbgen_raw_developed (t), "a thumbnail is the camera's preview");
  thumbgen_free (t);
  /* A small export (1000 pixels) is developed too: the preview can look
     quite unlike the raw (this one is much darker). */
  t = thumbgen_start (nef, NULL, 1000, 1, err, sizeof err);
  CHECK (t && thumbgen_raw_developed (t), "a 1000-pixel export of a raw is developed, not the camera's preview");
  while (t && (r = thumbgen_step (t, 256)) == 0)
    ;
  CHECK (t && r == 1 && thumbgen_take (t, &img) == 0 && img.w == 1000, "and comes out 1000 wide");
  if (t && r == 1)
    free (img.rgb);
  thumbgen_free (t);
  /* Developed, at half size (2142 wide is more than 1600). */
  t = thumbgen_start (nef, NULL, 1600, 1, err, sizeof err);
  while (t && (r = thumbgen_step (t, 256)) == 0)
    ;
  CHECK (t && r == 1 && thumbgen_take (t, &img) == 0 && img.w == 1600, "developed for the viewer: 1600 wide");
  if (t && r == 1)
    {
      /* An aurora over the Earth at night: plenty of strongly coloured
         pixels (green and red), which a missing colour matrix would grey. */
      long coloured = 0, total = 0;
      int x, y;
      for (y = 0; y < img.h; y += 4)
        for (x = 0; x < img.w; x += 4)
          {
            const unsigned char *q = img.rgb + (y * img.w + x) * 3;
            int mx = q[0] > q[1] ? (q[0] > q[2] ? q[0] : q[2]) : (q[1] > q[2] ? q[1] : q[2]);
            int mn = q[0] < q[1] ? (q[0] < q[2] ? q[0] : q[2]) : (q[1] < q[2] ? q[1] : q[2]);
            total++;
            if (mx - mn > 40)
              coloured++;
          }
      CHECK (coloured * 20 > total, "in colour: over 5% of it strongly coloured (the camera's colour matrix)");
      /* Again with the plug-in's strong noise reduction: smoother pixel to pixel (ISO 3200). */
      {
        rgb_image nr;
        thumbgen *t2;
        int r2 = 0;
        long d0 = 0, d1 = 0;
        rawjob_setup (helper, scratch, run_and_keep);
        rawjob_set_raw_noise (2);
        t2 = thumbgen_start (nef, NULL, 1600, 1, err, sizeof err);
        rawjob_set_raw_noise (0);      /* the job keeps what it started with */
        while (t2 && (r2 = thumbgen_step (t2, 256)) == 0)
          ;
        CHECK (strstr (last_cmd, " decode ") && !strcmp (last_cmd + strlen (last_cmd) - 6, " 2 0 1"),
               "raw noise reduction: the plug-in's decode is given 2 (its sixth argument; then QUALITY 0, "
               "MEMORY 1: no scratch cache)");
        {
          thumbgen *t3;
          rawjob_set_raw_noise (2);
          t3 = thumbgen_start (nef, NULL, 256, 1, err, sizeof err);   /* a thumbnail: the camera's preview */
          rawjob_set_raw_noise (0);
          while (t3 && thumbgen_step (t3, 64) == 0)
            ;
          CHECK (strstr (last_cmd, " thumb ") && strcmp (last_cmd + strlen (last_cmd) - 2, " 2"),
                 "but not for the camera's preview");
          thumbgen_free (t3);
        }
        rawjob_setup (helper, scratch, run_now);
        if (t2 && r2 == 1 && thumbgen_take (t2, &nr) == 0)
          {
            int x, y;
            for (y = 10; y < img.h - 10 && y < nr.h - 10; y += 3)
              for (x = 10; x < img.w - 11; x += 3)
                {
                  const unsigned char *a0 = img.rgb + (y * img.w + x) * 3, *a1 = nr.rgb + (y * nr.w + x) * 3;
                  d0 += abs (a0[1] - a0[4]);
                  d1 += abs (a1[1] - a1[4]);
                }
            CHECK (nr.w == 1600 && d1 < d0, "raw noise reduction (strong): smoother from pixel to pixel");
            free (nr.rgb);
          }
        else
          CHECK (0, "raw noise reduction (strong): developed");
        thumbgen_free (t2);
      }
      free (img.rgb);
    }
  thumbgen_free (t);
  /* Upside down (orientation 3 from the plug-in): turned, whatever the caller said. */
  t = thumbgen_start (c400, NULL, 256, 1, err, sizeof err);
  while (t && (r = thumbgen_step (t, 64)) == 0)
    ;
  CHECK (t && r == 1 && thumbgen_raw_details (t, &ri) && ri.orientation == 3, "a camera held upside down: orientation 3");
  thumbgen_free (t);
  /* Two jobs at once wait their turn. */
  {
    thumbgen *a = thumbgen_start (nef, NULL, 256, 1, err, sizeof err);
    thumbgen *b = thumbgen_start (cr2, NULL, 256, 1, err, sizeof err);
    int ra = 0, rb = 0, guard = 0;
    while ((ra == 0 || rb == 0) && guard++ < 10000)
      {
        if (ra == 0) ra = thumbgen_step (a, 64);
        if (rb == 0) rb = thumbgen_step (b, 64);
      }
    CHECK (ra == 1 && rb == 1 && !rawjob_busy (), "two at once: both done, nothing left running");
    thumbgen_free (a);
    thumbgen_free (b);
  }
  /* Not a raw file after all (a raw name): the plug-in's error. */
  p (path, "fake.nef");
  {
    FILE *ff = fopen (path, "wb");
    fwrite ("II*\0 not really", 1, 15, ff);
    fclose (ff);
  }
  t = thumbgen_start (path, NULL, 256, 1, err, sizeof err);
  while (t && (r = thumbgen_step (t, 64)) == 0)
    ;
  CHECK (t && r == -1 && strstr (thumbgen_error (t), "couldn't read this raw file"), "a damaged one: the plug-in says so");
  thumbgen_free (t);
  /* The plug-in dying without a word. */
  rawjob_setup (helper, scratch, ran_nothing);
  t = thumbgen_start (nef, NULL, 256, 1, err, sizeof err);
  while (t && (r = thumbgen_step (t, 64)) == 0)
    ;
  CHECK (t && r == -1 && strstr (thumbgen_error (t), "stopped without finishing"), "ended with nothing: said so, not waited for for ever");
  thumbgen_free (t);
  rawjob_setup (helper, scratch, run_now);

  /* The scratch cache: the full size kept, so it isn't developed again. */
  {
    unsigned char first[64];
    int photos = 0, made = 0;
    long long used;
    rawjob_setup (helper, scratch, run_counting);
    rawjob_cache_setup (scratch, 100);
    runs = 0;
    t = thumbgen_start (nef, NULL, 1 << 30, 1, err, sizeof err);
    while (t && (r = thumbgen_step (t, 256)) == 0)
      ;
    if (t && r == 1 && thumbgen_take (t, &img) == 0)
      {
        made = img.w == 4284;
        memcpy (first, img.rgb + (size_t) img.w * 1400 * 3 + 2000 * 3, sizeof first);
        free (img.rgb);
      }
    thumbgen_free (t);
    used = rawjob_cache_used (&photos);
    CHECK (made && runs == 1 && photos == 1 && used > 36000000, "scratch: the full size developed once, and kept (36 MB)");
    t = thumbgen_start (nef, NULL, 1 << 30, 1, err, sizeof err);
    while (t && (r = thumbgen_step (t, 256)) == 0)
      ;
    made = 0;
    if (t && r == 1 && thumbgen_take (t, &img) == 0)
      {
        made = img.w == 4284 && !memcmp (first, img.rgb + (size_t) img.w * 1400 * 3 + 2000 * 3, sizeof first);
        free (img.rgb);
      }
    thumbgen_free (t);
    CHECK (made && runs == 1, "scratch: asked again, from the cache (the plug-in not run), the same picture");
    t = thumbgen_start (nef, NULL, 2000, 1, err, sizeof err);
    while (t && (r = thumbgen_step (t, 256)) == 0)
      ;
    CHECK (t && r == 1 && runs == 1 && thumbgen_take (t, &img) == 0 && img.w == 2000, "scratch: a 2000-pixel export uses it too");
    if (t && r == 1)
      free (img.rgb);
    thumbgen_free (t);
    t = thumbgen_start (nef, NULL, 1600, 1, err, sizeof err);
    while (t && (r = thumbgen_step (t, 256)) == 0)
      ;
    CHECK (t && r == 1 && runs == 2, "scratch: the Viewer's proxy isn't taken from it (previews keep that)");
    thumbgen_free (t);
    rawjob_set_raw_noise (1);
    t = thumbgen_start (nef, NULL, 1 << 30, 1, err, sizeof err);
    rawjob_set_raw_noise (0);
    while (t && (r = thumbgen_step (t, 256)) == 0)
      ;
    thumbgen_free (t);
    rawjob_cache_used (&photos);
    CHECK (runs == 3 && photos == 2, "scratch: with raw noise reduction, another picture (developed, kept)");
    rawjob_cache_setup (scratch, 40);
    t = thumbgen_start (cr2, NULL, 1 << 30, 1, err, sizeof err);
    while (t && (r = thumbgen_step (t, 256)) == 0)
      ;
    thumbgen_free (t);
    used = rawjob_cache_used (&photos);
    CHECK (runs == 4 && used <= 40ll * 1024 * 1024 && photos >= 1, "scratch: kept within its size, the least recently used going first");
    rawjob_cache_discard ();
    CHECK (rawjob_cache_used (&photos) == 0 && photos == 0, "scratch: discarded");
    rawjob_cache_setup (NULL, 0);
    t = thumbgen_start (nef, NULL, 1 << 30, 1, err, sizeof err);
    while (t && (r = thumbgen_step (t, 256)) == 0)
      ;
    thumbgen_free (t);
    CHECK (runs == 5 && !rawjob_cache_on (), "scratch: none chosen, nothing kept");
    {
      fsys_dir *d = fsys_opendir (scratch);
      fsys_entry e;
      int left = 0;
      while (d && fsys_readdir (d, &e) == 1)
        left += !e.is_dir;
      if (d)
        fsys_closedir (d);
      CHECK (left == 0, "scratch: no files left behind");
    }
    rawjob_setup (helper, scratch, run_now);
  }

  /* Importing a folder of raw files: added, thumbnails made, details filled in. */
  {
    library *l;
    char libdir[700], dir[700], dst[760];
    cat_source all = { SRC_ALL, 0, 0, "" };
    cat_item *items;
    cat_details d;
    int n, k, guard = 0;
    lib_progress pr;
    p (dir, "Raws");
    mkdir (dir, 0777);
    snprintf (dst, sizeof dst, "%s/aurora.nef", dir);
    {
      FILE *in = fopen (nef, "rb"), *out = fopen (dst, "wb");
      char buf[65536];
      size_t got;
      while ((got = fread (buf, 1, sizeof buf, in)) > 0)
        fwrite (buf, 1, got, out);
      fclose (in);
      fclose (out);
    }
    p (libdir, "RawLibrary");
    CHECK (lib_open (&l, libdir, err, sizeof err) == 0, "a library for raws");
    lib_import (l, dir);
    while (lib_busy (l) && guard++ < 100000)
      lib_work (l, fake_now, clock_cs + 3, NULL, NULL);
    lib_get_progress (l, &pr);
    CHECK (pr.added == 1, "the raw file added");
    cat_list (lib_cat (l), &all, &items, &n);
    k = n == 1 ? 0 : -1;
    CHECK (k == 0 && items[0].thumb_state == THUMB_READY, "its thumbnail made by the plug-in");
    CHECK (k == 0 && cat_get_details (lib_cat (l), items[0].id, &d) == 0 && d.width == 4284 && d.height == 2844
           && strcmp (d.model, "D3S") == 0 && d.iso == 3200, "its details from the plug-in: 4284 x 2844, D3S, ISO 3200");
    free (items);
    lib_close (l);
  }
  /* For stacking: the sensor's data as FITS (LumenRaw cfa), then stacked */
  rawjob_setup (helper, scratch, run_now);
  {
    char cr2b[700], f1[1024], f2[1024], st[1024], e2[300];
    rawjob *j;
    fits_hdr fh;
    FILE *ff;
    if (sample (cr2b, sizeof cr2b, "RAW_CANON_5DMARK2_PREPROD.CR2"))
      {
        p (f1, "RawStack"); mkdir (f1, 0777);
        p (f1, "RawStack/Frame1"); p (f2, "RawStack/Frame2"); p (st, "RawStack/Stacked.fits");
        j = rawjob_start_cfa (cr2b, f1, e2, sizeof e2);
        while (j && (r = rawjob_poll (j)) == 0)
          ;
        CHECK (j && r == 1, "raw for stacking: the sensor's data written as FITS");
        rawjob_free (j);
        ff = fopen (f1, "rb");
        CHECK (ff && fits_read_header (ff, &fh, e2, sizeof e2) == 0 && fh.w == 5634 && fh.h == 3752 && fh.bitpix == 16
               && !strcmp (fh.bayer, "GBRG") && !fh.bottom_up && fh.iso == 3200 && !strcmp (fh.instrume, "Canon EOS 5D Mark II"),
               "raw for stacking: 5634 x 3752, 16 bits, Bayer GBRG, top down, ISO and camera (and kept after the job)");
        if (ff)
          fclose (ff);
        j = rawjob_start_cfa (nef, f2, e2, sizeof e2);
        while (j && (r = rawjob_poll (j)) == 0)
          ;
        CHECK (j && r == 1, "raw for stacking: a NEF too");
        rawjob_free (j);
        ff = fopen (f2, "rb");
        CHECK (ff && fits_read_header (ff, &fh, e2, sizeof e2) == 0 && fh.w == 4284 && !strcmp (fh.bayer, "RGGB"),
               "raw for stacking: the NEF's Bayer pattern (RGGB)");
        if (ff)
          fclose (ff);
        j = rawjob_start_cfa (cr2, f2, e2, sizeof e2);
        while (j && (r = rawjob_poll (j)) == 0)
          ;
        CHECK (j && r == -1 && strstr (rawjob_error (j), "Bayer"), "raw for stacking: an sRAW isn't a Bayer mosaic: says so");
        rawjob_free (j);
        /* The same frame twice: stacked (lined up exactly) */
        {
          char cp[2048];
          snprintf (cp, sizeof cp, "cp '%s' '%s'", f1, f2);
          if (system (cp) == 0)
            {
              const char *lp[2] = { f1, f2 };
              stack_job *sj = stack_start (lp, 2, NULL, 0, st, "test", e2, sizeof e2);
              int guard = 0;
              while (sj && (r = stack_step (sj, stack_now, stack_clock + 1000)) == 0 && guard++ < 100000)
                ;
              CHECK (sj && r == 1, "raw for stacking: a real camera's frames stacked");
              if (sj && r != 1)
                fprintf (stderr, "raw stack: %s\n", stack_error (sj));
              stack_free (sj);
            }
        }
      }
  }
}


/* --- plug-ins: described, listed, switched off and on, added, removed */
static void put_file (const char *path, const char *text)
{
  FILE *f = fopen (path, "w");
  if (f)
    {
      fputs (text, f);
      fclose (f);
    }
}

static void test_plugins (void)
{
  char dir[700], state[700], src[700], p[900], err[300];
  plugin_info list[PLUGINS_MAX], one;
  const plugin_info *raw;
  int n;
  fsys_entry e;
  fsys_join (dir, sizeof dir, work, "Plugins");
  fsys_join (state, sizeof state, work, "Choices/Lumen/Plugins");
  fsys_join (src, sizeof src, work, "Downloads");
  fsys_remove_tree (dir);
  fsys_remove_tree (src);
  CHECK (plugins_scan (dir, state, list, PLUGINS_MAX) == 0, "plug-ins: no Plugins directory, none");
  /* LibRaw as PackMan installs it: typed files (",fff", ",ff8" off RISC OS). */
  fsys_join (p, sizeof p, dir, "LibRaw");
  fsys_mkdirs (p);
  fsys_join (p, sizeof p, dir, "LibRaw/Plugin,fff");
  put_file (p, "# about\nName: Camera raw (LibRaw)\nVersion: 1.0\nKind:  raw \nProgram: LumenRaw\n"
               "Package: LumenRaw\nDescription: Reads camera raw files\n");
  fsys_join (p, sizeof p, dir, "LibRaw/LumenRaw,ff8");
  put_file (p, "program");
  /* An old one with no Plugin file, one whose program is missing, and a stray directory. */
  fsys_join (p, sizeof p, dir, "Old");
  fsys_mkdirs (p);
  fsys_join (p, sizeof p, dir, "Old/LumenRaw");
  put_file (p, "program");
  fsys_join (p, sizeof p, dir, "Broken");
  fsys_mkdirs (p);
  fsys_join (p, sizeof p, dir, "Broken/Plugin");
  put_file (p, "Name: Another raw reader\nKind: raw\nProgram: Gone\n");
  fsys_join (p, sizeof p, dir, "Stray");
  fsys_mkdirs (p);
  n = plugins_scan (dir, state, list, PLUGINS_MAX);
  CHECK (n == 3, "plug-ins: three found (the stray directory isn't one)");
  CHECK (n == 3 && !strcmp (list[0].name, "Another raw reader") && !strcmp (list[1].name, "Camera raw (LibRaw)")
         && !strcmp (list[1].dir, "LibRaw") && !strcmp (list[2].dir, "Old"), "plug-ins: sorted by name");
  CHECK (n == 3 && !strcmp (list[1].version, "1.0") && !strcmp (list[1].kind, "raw")
         && !strcmp (list[1].package, "LumenRaw") && strstr (list[1].program, "LibRaw/LumenRaw,ff8"),
         "plug-ins: the Plugin file read (trimmed), the program found with its type suffix");
  CHECK (n == 3 && list[0].program[0] == '\0' && list[0].enabled, "plug-ins: a missing program is left out");
  CHECK (n == 3 && !strcmp (list[2].name, "Camera raw (LibRaw)") && strstr (list[2].program, "Old/LumenRaw"),
         "plug-ins: one with no Plugin file but LumenRaw is LibRaw");
  raw = plugins_find (list, n, "raw");
  CHECK (raw && !strcmp (raw->dir, "LibRaw"), "plug-ins: the raw reader is the first that works");
  /* Switching off and on, remembered in Choices (made when needed). */
  CHECK (plugins_set_enabled (state, "LibRaw", 0) == 0 && plugins_set_enabled (state, "Old", 0) == 0,
         "plug-ins: switched off");
  n = plugins_scan (dir, state, list, PLUGINS_MAX);
  CHECK (n == 3 && !list[1].enabled && !list[2].enabled && list[0].enabled, "plug-ins: off after a rescan");
  CHECK (plugins_find (list, n, "raw") == NULL, "plug-ins: no raw reader when they are off");
  CHECK (plugins_set_enabled (state, "LibRaw", 1) == 0 && plugins_enabled (state, "LibRaw")
         && !plugins_enabled (state, "Old"), "plug-ins: one back on, the other still off");
  CHECK (plugins_set_enabled (state, "LibRaw", 1) == 0 && plugins_set_enabled (state, "Old", 0) == 0
         && !plugins_enabled (state, "Old"), "plug-ins: switching twice is harmless");
  /* Adding: a downloaded plug-in, with a subdirectory. */
  fsys_join (p, sizeof p, src, "Extra/Docs");
  fsys_mkdirs (p);
  fsys_join (p, sizeof p, src, "Extra/Plugin");
  put_file (p, "Name: Extra\nVersion: 2\nKind: export\nProgram: Run\n");
  fsys_join (p, sizeof p, src, "Extra/Run");
  put_file (p, "program");
  fsys_join (p, sizeof p, src, "Extra/Docs/Notes");
  put_file (p, "notes");
  fsys_join (p, sizeof p, src, "Extra");
  CHECK (plugins_add (dir, p, 0, err, sizeof err) == 0, "plug-ins: added");
  fsys_join (p, sizeof p, dir, "Extra/Docs/Notes");
  CHECK (fsys_stat (p, &e) == 0, "plug-ins: added with everything in it");
  fsys_join (p, sizeof p, src, "Extra");
  CHECK (plugins_add (dir, p, 0, err, sizeof err) == 1, "plug-ins: one of that name there: asks");
  fsys_join (p, sizeof p, src, "Extra/Plugin");
  put_file (p, "Name: Extra\nVersion: 3\nKind: export\nProgram: Run\n");
  fsys_join (p, sizeof p, src, "Extra");
  CHECK (plugins_add (dir, p, 1, err, sizeof err) == 0, "plug-ins: replaced");
  fsys_join (p, sizeof p, dir, "Extra");
  CHECK (plugins_read (p, &one) == 0 && !strcmp (one.version, "3"), "plug-ins: the new one is there");
  fsys_join (p, sizeof p, src, "Docs");
  fsys_mkdirs (p);
  CHECK (plugins_add (dir, p, 0, err, sizeof err) == -1 && strstr (err, "isn't a Lumen plug-in"),
         "plug-ins: a directory with no Plugin file isn't added");
  fsys_join (p, sizeof p, dir, "Extra");
  CHECK (plugins_add (dir, p, 1, err, sizeof err) == -1 && strstr (err, "already"),
         "plug-ins: one already in Plugins isn't added to itself");
  CHECK (fsys_stat (p, &e) == 0, "plug-ins: ...and is still there");
  /* Removing. */
  CHECK (plugins_remove (dir, "Extra", err, sizeof err) == 0 && fsys_stat (p, &e) != 0, "plug-ins: removed");
  CHECK (plugins_remove (dir, "../Downloads", err, sizeof err) == -1, "plug-ins: only a plug-in's own name");
  CHECK (plugins_remove (dir, "Extra", err, sizeof err) == 0, "plug-ins: removing one that's gone is fine");
  n = plugins_scan (dir, state, list, PLUGINS_MAX);
  CHECK (n == 3, "plug-ins: three again");
  fsys_remove_tree (dir);
  fsys_remove_tree (src);
}

/* Raw files arrive typed anyhow (DOS from a card, Text, Data): their
   extension is what lets them in to be probed. */
static void test_raw_names (void)
{
  char mp[24];
  img_megapixels (4000, 3000, mp, sizeof mp);
  CHECK (!strcmp (mp, "12 MP"), "megapixels: 4000 x 3000 is 12 MP");
  img_megapixels (4032, 3024, mp, sizeof mp);
  CHECK (!strcmp (mp, "12.2 MP"), "megapixels: 4032 x 3024 is 12.2 MP");
  img_megapixels (4824, 3232, mp, sizeof mp);
  CHECK (!strcmp (mp, "15.6 MP"), "megapixels: 4824 x 3232 is 15.6 MP");
  img_megapixels (1000, 1000, mp, sizeof mp);
  CHECK (!strcmp (mp, "1 MP"), "megapixels: 1000 x 1000 is 1 MP");
  img_megapixels (760, 480, mp, sizeof mp);
  CHECK (!strcmp (mp, "0.4 MP"), "megapixels: 760 x 480 is 0.4 MP");
  img_megapixels (200, 150, mp, sizeof mp);
  CHECK (!strcmp (mp, "0.03 MP"), "megapixels: a small one, 0.03 MP");
  img_megapixels (0, 3000, mp, sizeof mp);
  CHECK (!mp[0], "megapixels: none when the size isn't known");
  CHECK (img_has_raw_extension ("SDFS::Card.$.DCIM.IMG_0001/CR2"), "raw names: RISC OS /CR2");
  CHECK (img_has_raw_extension ("/media/card/dsc_0001.nef"), "raw names: POSIX .nef");
  CHECK (img_has_raw_extension ("x.y.Photo/Dng"), "raw names: any case");
  CHECK (!img_has_raw_extension ("SDFS::Card.$.DCIM.IMG_0001/JPG"), "raw names: not a JPEG");
  CHECK (!img_has_raw_extension ("SDFS::Card.$.Raw.Notes"), "raw names: a directory called Raw isn't one");
  CHECK (!img_has_raw_extension ("Photo/nefx"), "raw names: the whole extension");
}

/* --- Choices: defaults, a round trip, rubbish and limits */
static void test_prefs (void)
{
  char path[700];
  prefs p, q;
  FILE *f;
  fsys_join (path, sizeof path, work, "Prefs/Lumen/Choices");
  remove (path);
  CHECK (prefs_load (path, &p) == -1 && p.export_quality == 90 && p.thumb_size == 1 && p.verify == 1
         && p.library[0] == '\0' && p.export_format == 0 && p.export_size == 0, "prefs: defaults with no file");
  snprintf (p.library, sizeof p.library, "SDFS::Pi.$.My Photos.Library");
  p.export_format = 1; p.export_quality = 75; p.export_size = 2048; p.thumb_size = 0; p.verify = 0;
  CHECK (prefs_save (path, &p) == 0, "prefs: saved (its directory made)");
  CHECK (prefs_load (path, &q) == 0 && !memcmp (&p, &q, sizeof p), "prefs: read back the same (a space in the path too)");
  f = fopen (path, "w");
  if (f)
    {
      fputs ("# x\nfuture_thing 7\nexport_quality 400\nexport_size 999\nthumb_size -3\nverify\nexport_format x\n", f);
      fclose (f);
    }
  CHECK (prefs_load (path, &q) == 0 && q.export_quality == 100 && q.export_size == 0 && q.thumb_size == 0
         && q.verify == 1 && q.export_format == 0, "prefs: unknown keys ignored, values kept in range");
}

/* --- watched folders and albums from their folders */
static void drain (library *l)
{
  int guard = 0;
  clock_cs = 0;
  while (lib_busy (l) && guard++ < 100000)
    lib_work (l, fake_now, clock_cs + 3, NULL, NULL);
}

static int folder_count (cat *c, cat_id project, const char *name, int depth)
{
  cat_folder *f;
  int n, k, r = -1;
  if (cat_folder_albums (c, project, &f, &n))
    return -1;
  for (k = 0; k < n; k++)
    if (!strcmp (f[k].name, name) && f[k].depth == depth)
      r = f[k].count;
  free (f);
  return r;
}

static void test_watch (void)
{
  library *l;
  char err[256], path[1024], libdir[1024], dst[1024];
  cat_group *g;
  int ng, w, a, nf;
  cat_id proj;
  cat_folder *f;
  cat *c;
  p (libdir, "WatchLibrary");
  p (path, "Watched/2024/Summer");
  fsys_mkdirs (path);
  p (path, "Watched/2023");
  fsys_mkdirs (path);
  p (path, "Watched/top.jpg");          make_jpeg (path, 64, 48, 1, "2024:01:01 10:00:00");
  p (path, "Watched/2024/spring.jpg");  make_jpeg (path, 66, 48, 1, "2024:04:01 10:00:00");
  p (path, "Watched/2024/Summer/beach.jpg"); make_jpeg (path, 68, 48, 1, "2024:07:01 10:00:00");
  p (path, "Watched/2023/winter.jpg");  make_jpeg (path, 70, 48, 1, "2023:12:01 10:00:00");
  CHECK (lib_open (&l, libdir, err, sizeof err) == 0, "watch: library");
  c = lib_cat (l);
  lib_set_new_folders (l, 1, 1);         /* Choices: watch what's imported, albums from its folders */
  p (path, "Watched");
  lib_import (l, path);
  drain (l);
  cat_groups (c, SRC_PROJECT, &g, &ng);
  proj = ng == 1 ? g[0].id : 0;
  CHECK (ng == 1 && g[0].count == 4 && !strcmp (g[0].name, "Watched"), "watch: one project of 4");
  free (g);
  CHECK (cat_project_watching (c, proj, &w, &a) == 0 && w == 1 && a == 1, "watch: watched, with folder albums");
  CHECK (cat_project_root (c, proj, dst, sizeof dst) == 0 && !strcmp (dst, path), "watch: its folder recorded");
  CHECK (folder_count (c, proj, "2024", 1) == 2 && folder_count (c, proj, "Summer", 2) == 1
         && folder_count (c, proj, "2023", 1) == 1, "watch: albums 2024 (2: its own and Summer's), Summer (1), 2023 (1)");
  CHECK (cat_folder_albums (c, proj, &f, &nf) == 0 && nf == 3 && !strcmp (f[0].name, "2023") && !strcmp (f[1].name, "2024")
         && !strcmp (f[2].name, "Summer"), "watch: in folder order, nested after their parent");
  CHECK (nf == 3 && cat_album_is_folder (c, f[0].id), "watch: folder albums are marked");
  free (f);
  cat_groups (c, SRC_ALBUM, &g, &ng);
  CHECK (ng == 0, "watch: folder albums aren't among your own albums");
  free (g);
  /* New photos, a new folder, a photo moved between folders, and one deleted. */
  p (path, "Watched/2024/Summer/sunset.jpg"); make_jpeg (path, 72, 48, 1, "2024:07:02 20:00:00");
  p (path, "Watched/2025");
  fsys_mkdirs (path);
  p (path, "Watched/2025/new.jpg");      make_jpeg (path, 74, 48, 1, "2025:01:01 10:00:00");
  p (path, "Watched/2024/spring.jpg");   p (dst, "Watched/2023/spring.jpg");
  CHECK (rename (path, dst) == 0, "watch: a photo moved to another folder");
  p (path, "Watched/top.jpg");
  remove (path);
  CHECK (lib_watch_start (l) == 1, "watch: one folder watched");
  drain (l);
  cat_groups (c, SRC_PROJECT, &g, &ng);
  CHECK (ng == 1 && g[0].count == 6, "watch: two new photos in the same project (the deleted one is still known, missing)");
  free (g);
  CHECK (folder_count (c, proj, "2025", 1) == 1 && folder_count (c, proj, "Summer", 2) == 2
         && folder_count (c, proj, "2024", 1) == 2 && folder_count (c, proj, "2023", 1) == 2,
         "watch: a new album 2025; Summer 2; spring moved from 2024 to 2023");
  /* Renamed project: still the one its folder goes into. */
  CHECK (cat_group_rename (c, SRC_PROJECT, proj, "Holidays") == 0, "watch: project renamed");
  p (path, "Watched/2025/later.jpg");    make_jpeg (path, 76, 48, 1, "2025:02:01 10:00:00");
  lib_watch_start (l);
  drain (l);
  cat_groups (c, SRC_PROJECT, &g, &ng);
  CHECK (ng == 1 && !strcmp (g[0].name, "Holidays") && g[0].count == 7, "watch: a renamed project still gets its folder's photos");
  free (g);
  /* Folder albums off: they go; watching off: nothing more comes in. */
  CHECK (cat_project_set_watching (c, proj, 0, 0) == 0, "watch: both off");
  CHECK (cat_folder_albums (c, proj, &f, &nf) == 0 && nf == 0, "watch: folder albums gone");
  free (f);
  p (path, "Watched/2025/ignored.jpg");  make_jpeg (path, 78, 48, 1, "2025:03:01 10:00:00");
  CHECK (lib_watch_start (l) == 0, "watch: nothing watched");
  drain (l);
  cat_groups (c, SRC_PROJECT, &g, &ng);
  CHECK (ng == 1 && g[0].count == 7, "watch: unwatched, so nothing new");
  free (g);
  /* Albums back on (without watching): made again from the files. */
  CHECK (cat_project_set_watching (c, proj, 0, 1) == 0 && cat_sync_folder_albums (c, proj) == 0
         && folder_count (c, proj, "2025", 1) == 2, "watch: albums from folders again");
  lib_close (l);
}

/* --- lazy thumbnails: only the ones asked for; and they go with their photos */
static cat_id first_made;
static void note_made (void *ctx, cat_id v)
{
  (void) ctx;
  if (!first_made)
    first_made = v;
}

/* --- JPEG and PNG files typed otherwise (DOS, Text: from a card or a
   share without MimeMap) are found by their fingerprint (first bytes),
   with or without a photo's name; other types are left unopened. */
static void test_mistyped (void)
{
  library *l;
  char err[256], path[1024], libdir[1024];
  cat_item *items;
  int n, k, jpg = 0, png = 0;
  FILE *f;
  lib_progress pr;
  cat_source all = { SRC_ALL, 0, 0, "" };
  p (path, "Mistyped"); mkdir (path, 0777);
  p (path, "Mistyped/text.png,fff"); make_png (path, 120, 80, 0, 0);
  p (path, "Mistyped/dos.JPG,fe4"); make_jpeg (path, 160, 120, 1, NULL);
  p (path, "Mistyped/jpeg.jpeg,fe4"); make_jpeg (path, 200, 150, 1, NULL);
  p (path, "Mistyped/noname,fff"); make_png (path, 140, 90, 0, 0);  /* no name to go by: its fingerprint */
  p (path, "Mistyped/sprite,ff9"); make_png (path, 150, 90, 0, 0);  /* typed Sprite: left unopened */
  fsys_mimemap_add ("image/pjpeg", 0xA3B, "PJPEG", "pjp");
  p (path, "Mistyped/progressive,a3b"); make_jpeg (path, 220, 150, 1, NULL);  /* MimeMap: a JPEG type */
  p (path, "Mistyped/odd.pjp,123"); make_jpeg (path, 230, 150, 1, NULL);      /* MimeMap: a JPEG extension */
  p (path, "Mistyped/notes,fff");
  f = fopen (path, "wb");
  if (f)
    {
      fputs ("just some notes\n", f);
      fclose (f);
    }
  p (path, "Mistyped/fake.png,fff");
  f = fopen (path, "wb");
  if (f)
    {
      fputs ("not a picture at all\n", f);
      fclose (f);
    }
  p (libdir, "MistypedLibrary");
  CHECK (lib_open (&l, libdir, err, sizeof err) == 0, "mistyped: library");
  p (path, "Mistyped");
  lib_import (l, path);
  drain (l);
  lib_get_progress (l, &pr);
  cat_list (lib_cat (l), &all, &items, &n);
  for (k = 0; k < n; k++)
    {
      jpg += items[k].filetype == FT_JPEG;
      png += items[k].filetype == FT_PNG;
    }
  CHECK (n == 6, "mistyped: PNGs typed Text (one without a photo's name), two JPEGs typed DOS, and two JPEGs "
                 "MimeMap knows by their type or extension are imported");
  if (n != 6)
    for (k = 0; k < n; k++)
      fprintf (stderr, "  imported %s (type %x)\n", items[k].name, items[k].filetype);
  CHECK (jpg == 4 && png == 2, "mistyped: kept as JPEG and PNG, whatever they were typed");
  CHECK (pr.skipped == 1 && strstr (lib_last_error (l) ? lib_last_error (l) : "", "fake") != NULL,
         "mistyped: a .png that isn't one is reported");
  free (items);
  lib_close (l);
}

/* --- TIFF photos: imported by their fingerprint, their details from
   tags libtiff put after the picture; a DNG with another name is raw. */
static void small_tiff (const char *path, int w, int h, int orientation, int dng)
{
  TIFF *t = TIFFOpen (path, "w");
  unsigned char *row = malloc ((size_t) w * 3);
  int x, y;
  static const unsigned char ver[4] = { 1, 4, 0, 0 };
  TIFFSetField (t, TIFFTAG_IMAGEWIDTH, w);
  TIFFSetField (t, TIFFTAG_IMAGELENGTH, h);
  TIFFSetField (t, TIFFTAG_BITSPERSAMPLE, 8);
  TIFFSetField (t, TIFFTAG_SAMPLESPERPIXEL, 3);
  TIFFSetField (t, TIFFTAG_PHOTOMETRIC, PHOTOMETRIC_RGB);
  TIFFSetField (t, TIFFTAG_COMPRESSION, COMPRESSION_LZW);
  TIFFSetField (t, TIFFTAG_ORIENTATION, orientation);
  TIFFSetField (t, TIFFTAG_MAKE, "Scanco");
  TIFFSetField (t, TIFFTAG_DATETIME, "2025:12:24 09:15:00");
  if (dng)
    TIFFSetField (t, TIFFTAG_DNGVERSION, ver);
  for (y = 0; y < h; y++)
    {
      for (x = 0; x < w * 3; x++)
        row[x] = (unsigned char) (x + y * 7 + w);
      TIFFWriteScanline (t, row, (uint32_t) y, 0);
    }
  TIFFClose (t);
  free (row);
}

static void test_tiff_import (void)
{
  library *l;
  char err[256], path[1024], libdir[1024];
  cat_item *items;
  cat_details d;
  int n, k, tiffs = 0, ok = 0;
  cat_source all = { SRC_ALL, 0, 0, "" };
  p (path, "Scans"); mkdir (path, 0777);
  p (path, "Scans/page1.tif,ff0"); small_tiff (path, 300, 200, 1, 0);
  p (path, "Scans/page2,fff"); small_tiff (path, 240, 160, 6, 0);     /* typed Text, no name: its fingerprint */
  p (path, "Scans/camera.tif,ff0"); small_tiff (path, 64, 48, 1, 1);  /* a DNG: raw */
  p (libdir, "ScansLibrary");
  CHECK (lib_open (&l, libdir, err, sizeof err) == 0, "tiff: library");
  p (path, "Scans");
  lib_import (l, path);
  drain (l);
  cat_list (lib_cat (l), &all, &items, &n);
  CHECK (n == 3, "tiff: the two TIFFs and the DNG imported");
  for (k = 0; k < n; k++)
    if (items[k].filetype == FT_TIFF && cat_get_details (lib_cat (l), items[k].id, &d) == 0)
      {
        tiffs++;
        if (strstr (d.path, "page1"))
          ok += d.width == 300 && d.height == 200 && d.orientation == 1 && !strcmp (d.make, "Scanco")
                && !strcmp (d.taken, "2025:12:24 09:15:00") && items[k].thumb_state == THUMB_READY;
        else if (strstr (d.path, "page2"))
          ok += d.width == 240 && d.height == 160 && d.orientation == 6 && items[k].thumb_state == THUMB_READY;
      }
  CHECK (tiffs >= 2 && ok == 2, "tiff: sizes, orientation, scanner and date from the tags after the picture, thumbnails made");
  {
    cat_origin o;
    int raw = 0;
    for (k = 0; k < n; k++)
      if (cat_get_details (lib_cat (l), items[k].id, &d) == 0 && strstr (d.path, "camera")
          && cat_origin_of (lib_cat (l), d.original, &o) == 1)
        raw = o.format == IMG_RAW;
    CHECK (raw, "tiff: the DNG called camera.tif is a raw file");
  }
  free (items);
  lib_close (l);
}

/* Photos without a date taken (a print shop's CD: CD01/CNV00001.jpg...,
   no EXIF dates, each CD's files dated when it was made) are put in order
   by their file's date, then name; among photos with dates. */
static void set_mtime (const char *path, long long t)
{
  struct utimbuf u;
  u.actime = u.modtime = (time_t) t;
  utime (path, &u);
}

static void test_order_by_file_date (void)
{
  library *l;
  char err[256], path[1024], libdir[1024];
  cat_item *items;
  cat_details d;
  int n, k;
  const char *want[] = { "CD01/CNV00001", "CD01/CNV00002", "Camera/IMG_0001", "CD02/CNV00001", "CD02/CNV00002", "Clockless/DSC00001" };
  char got[6][64];
  cat_source all = { SRC_ALL, 0, 0, "" };
  p (path, "Boots"); mkdir (path, 0777);
  p (path, "Boots/CD02"); mkdir (path, 0777);
  p (path, "Boots/CD01"); mkdir (path, 0777);
  p (path, "Boots/Camera"); mkdir (path, 0777);
  p (path, "Boots/Clockless"); mkdir (path, 0777);
  /* CD02 made in 2005, CD01 in 2004: the names alone would mix them. */
  p (path, "Boots/CD02/CNV00001.jpg"); make_jpeg (path, 40, 30, 1, NULL); set_mtime (path, 1118000000);
  p (path, "Boots/CD02/CNV00002.jpg"); make_jpeg (path, 42, 30, 1, NULL); set_mtime (path, 1118000000);
  p (path, "Boots/CD01/CNV00002.jpg"); make_jpeg (path, 44, 30, 1, NULL); set_mtime (path, 1086000000);
  p (path, "Boots/CD01/CNV00001.jpg"); make_jpeg (path, 46, 30, 1, NULL); set_mtime (path, 1086000000);
  /* A camera's photo taken between the two CDs (its file dated later). */
  p (path, "Boots/Camera/IMG_0001.jpg"); make_jpeg (path, 48, 30, 1, "2005:01:15 10:00:00"); set_mtime (path, 1700000000);
  /* A camera whose clock was never set: no date, so its file's (latest). */
  p (path, "Boots/Clockless/DSC00001.jpg"); make_jpeg (path, 50, 30, 1, "0000:00:00 00:00:00"); set_mtime (path, 1600000000);
  p (libdir, "BootsLibrary");
  CHECK (lib_open (&l, libdir, err, sizeof err) == 0, "order: library");
  p (path, "Boots");
  lib_import (l, path);
  drain (l);
  cat_list (lib_cat (l), &all, &items, &n);
  CHECK (n == 6, "order: six photos");
  for (k = 0; k < n && k < 6; k++)
    {
      const char *e;
      got[k][0] = '\0';
      if (cat_get_details (lib_cat (l), items[k].id, &d) == 0 && (e = strstr (d.path, "Boots/")))
        {
          snprintf (got[k], sizeof got[k], "%s", e + 6);
          if (strrchr (got[k], '.'))
            *strrchr (got[k], '.') = '\0';
        }
      if (strstr (d.path, "DSC00001"))
        CHECK (d.taken[0] == '\0', "order: a date of 0000:00:00 isn't kept as when it was taken");
    }
  for (k = 0; k < 6 && k < n; k++)
    if (strcmp (got[k], want[k]))
      break;
  CHECK (k == 6, "order: without a date taken, by the file's date then name, among the dated photos");
  if (k < 6)
    for (k = 0; k < n && k < 6; k++)
      printf ("  %d: %s\n", k, got[k]);
  free (items);
  /* The other orders. */
  {
    cat_item *it;
    int m, ok = 1;
    cat_set_order (lib_cat (l), CAT_ORDER_NAME, 0);
    cat_list (lib_cat (l), &all, &it, &m);
    for (k = 1; k < m; k++)
      ok &= strcasecmp (it[k - 1].name, it[k].name) <= 0;
    CHECK (m == 6 && ok && !strncmp (it[0].name, "CNV00001", 8) && !strncmp (it[1].name, "CNV00001", 8),
           "order by name: the two CNV00001s first");
    {
      cat_details d0, d1;
      CHECK (cat_get_details (lib_cat (l), it[0].id, &d0) == 0 && cat_get_details (lib_cat (l), it[1].id, &d1) == 0
             && strstr (d0.path, "CD01") && strstr (d1.path, "CD02"), "order by name: the same names by date (CD01's first)");
    }
    free (it);
    cat_set_order (lib_cat (l), CAT_ORDER_NAME, 1);
    cat_list (lib_cat (l), &all, &it, &m);
    CHECK (m == 6 && !strncmp (it[0].name, "IMG_0001", 8), "order by name, reversed: IMG_0001 first");
    free (it);
    cat_set_order (lib_cat (l), CAT_ORDER_FILE, 1);
    cat_list (lib_cat (l), &all, &it, &m);
    CHECK (m == 6 && !strncmp (it[0].name, "IMG_0001", 8) && !strncmp (it[1].name, "DSC00001", 8),
           "order by file date, newest first");
    free (it);
    cat_set_order (lib_cat (l), CAT_ORDER_RATING, 0);
    cat_list (lib_cat (l), &all, &it, &m);
    if (m == 6)
      {
        cat_set_rating (lib_cat (l), &it[4].id, 1, 5);
        free (it);
        cat_list (lib_cat (l), &all, &it, &m);
        CHECK (m == 6 && it[0].rating == 5 && !strncmp (it[1].name, "CNV00001", 8), "order by rating: five stars first, then by date");
      }
    free (it);
    cat_set_order (lib_cat (l), CAT_ORDER_TAKEN, 0);
  }
  lib_close (l);
}

static void test_lazy_thumbs (void)
{
  library *l;
  char err[256], path[1024], libdir[1024], th[1100];
  cat_item *items;
  int n, k, made = 0, guard = 0;
  fsys_entry e;
  cat_source all = { SRC_ALL, 0, 0, "" };
  p (libdir, "LazyLibrary");
  CHECK (lib_open (&l, libdir, err, sizeof err) == 0, "lazy: library");
  lib_set_lazy_thumbs (l, 1);
  p (path, "Watched");                   /* test_watch's photos */
  lib_import (l, path);
  clock_cs = 0;
  while (lib_busy (l) && !lib_background_only (l) && guard++ < 100000)
    lib_work (l, fake_now, clock_cs + 3, NULL, NULL);
  CHECK (lib_background_only (l), "lazy: imported, only thumbnails nobody is looking at left");
  cat_list (lib_cat (l), &all, &items, &n);
  for (k = 0; k < n; k++)
    made += items[k].thumb_state == THUMB_READY;
  CHECK (n >= 5 && made < n, "lazy: not all made yet");
  if (n >= 5)
    {
      /* The last one comes on screen: made next, before the rest. */
      cat_id last = items[n - 1].id;
      lib_want_thumb (l, last);
      lib_want_thumb (l, last);          /* asked twice: made once */
      CHECK (!lib_background_only (l), "lazy: one on screen wanted");
      first_made = 0;
      guard = 0;
      while (!first_made && guard++ < 100000)
        lib_work (l, fake_now, clock_cs + 3, note_made, NULL);
      CHECK (first_made == last, "lazy: the one on screen made first");
      CHECK (lib_background_only (l) || !lib_busy (l), "lazy: then back to the background");
      drain (l);
      free (items);
      cat_list (lib_cat (l), &all, &items, &n);
      made = 0;
      for (k = 0; k < n; k++)
        made += items[k].thumb_state == THUMB_READY;
      CHECK (made == n && !lib_busy (l), "lazy: the rest made in the background");
      lib_thumb_path (l, items[1].id, th, sizeof th);
      CHECK (fsys_stat (th, &e) == 0, "lazy: its file is in the library's Thumbs");
      CHECK (strncmp (th, libdir, strlen (libdir)) == 0, "lazy: inside the library's own directory");
      CHECK (lib_remove (l, &items[1].id, 1) == 0 && fsys_stat (th, &e) != 0, "removed from the library: its thumbnail goes too");
    }
  free (items);
  lib_close (l);
}


/* --- FITS --- */

/* Writes a FITS file: the cards given (each "KEY     = value" or
   anything), SIMPLE/BITPIX/NAXIS first, then the samples v(x, file row,
   plane) (physical values: stored as (v - bzero)), big-endian. */
typedef double (*fits_fn) (int x, int y, int plane, void *ctx);
static void card_out (FILE *f, const char *c, int *n)
{
  char b[81];
  snprintf (b, sizeof b, "%-80s", c);
  fwrite (b, 1, 80, f);
  (*n)++;
}
static void pad_header (FILE *f, int n)
{
  card_out (f, "END", &n);
  while (n % 36)
    card_out (f, "", &n);
}
static void put_sample (FILE *f, int bitpix, double v)
{
  unsigned char b[8];
  int k, nb = (bitpix < 0 ? -bitpix : bitpix) / 8;
  uint64_t u = 0;
  if (bitpix == 8)
    u = (uint64_t) (v < 0 ? 0 : v > 255 ? 255 : (int) (v + 0.5));
  else if (bitpix == 16)
    u = (uint16_t) (int16_t) lround (v - 32768);
  else if (bitpix == 32)
    u = (uint32_t) (int32_t) lround (v);
  else if (bitpix == -32)
    {
      float fv = (float) v;
      uint32_t w;
      memcpy (&w, &fv, 4);
      u = w;
    }
  else
    memcpy (&u, &v, 8);
  for (k = 0; k < nb; k++)
    b[k] = (unsigned char) (u >> (8 * (nb - 1 - k)));
  fwrite (b, 1, (size_t) nb, f);
}
static void write_fits (const char *path, int bitpix, int w, int h, int planes, const char *const *cards,
                        fits_fn fn, void *ctx, int ext, long cut)
{
  FILE *f = fopen (path, "wb");
  char c[100];
  int n = 0, x, y, pl;
  long bytes;
  if (ext)
    {
      card_out (f, "SIMPLE  =                    T", &n);
      card_out (f, "BITPIX  =                    8", &n);
      card_out (f, "NAXIS   =                    0", &n);
      card_out (f, "EXTEND  =                    T", &n);
      pad_header (f, n);
      n = 0;
      card_out (f, ext == 2 ? "XTENSION= 'BINTABLE'" : "XTENSION= 'IMAGE   '", &n);
    }
  else
    card_out (f, "SIMPLE  =                    T", &n);
  snprintf (c, sizeof c, "BITPIX  = %20d", bitpix); card_out (f, c, &n);
  snprintf (c, sizeof c, "NAXIS   = %20d", planes > 1 ? 3 : 2); card_out (f, c, &n);
  snprintf (c, sizeof c, "NAXIS1  = %20d", w); card_out (f, c, &n);
  snprintf (c, sizeof c, "NAXIS2  = %20d", h); card_out (f, c, &n);
  if (planes > 1)
    {
      snprintf (c, sizeof c, "NAXIS3  = %20d", planes);
      card_out (f, c, &n);
    }
  if (ext == 2)
    card_out (f, "ZIMAGE  =                    T", &n);
  if (bitpix == 16)
    card_out (f, "BZERO   =              32768.0", &n);
  for (; cards && *cards; cards++)
    card_out (f, *cards, &n);
  pad_header (f, n);
  for (pl = 0; pl < planes; pl++)
    for (y = 0; y < h; y++)
      for (x = 0; x < w; x++)
        put_sample (f, bitpix, fn (x, y, pl, ctx));
  bytes = ftell (f);
  while (bytes % 2880)
    fputc (0, f), bytes++;
  fclose (f);
  if (cut > 0 && truncate (path, cut) != 0)
    fprintf (stderr, "can't cut %s\n", path);
}

static unsigned fits_noise (int x, int y)
{
  unsigned v = (unsigned) x * 2654435761u ^ (unsigned) y * 40503u;
  v ^= v >> 13;
  v *= 0x5bd1e995u;
  return (v ^ (v >> 15)) & 63;
}

/* A night sky (about 1000 of 65535, noisy) with a bright "star" in the
   top-left 8 x 8 as shown: file rows at the end when bottom-up. */
static double sky_fn (int x, int y, int plane, void *ctx)
{
  int h = *(int *) ctx, top_down = ((int *) ctx)[1], yy = top_down ? y : h - 1 - y;
  (void) plane;
  if (x < 8 && yy < 8)
    return 60000;
  return 1000 + fits_noise (x, yy);
}

/* A colour mosaic (RGGB from the top left as shown, bottom-up file): a
   grey sky, and an 8 x 8 block at the top left where only the red
   sites are bright. */
static double mosaic_fn (int x, int y, int plane, void *ctx)
{
  int h = *(int *) ctx, yy = h - 1 - y;
  int red = !(yy & 1) && !(x & 1), green = (yy & 1) != (x & 1);
  (void) plane;
  if (x < 8 && yy < 8 && red)
    return 50000;
  return (red ? 1000 : green ? 2000 : 600) + fits_noise (x, yy);
}

/* A stack saved as integers with its stars removed (as DWARF's are): a
   smooth sky only a few steps either side of a high level, so that more
   than half the pixels are at the median. Brighter in a block at the top
   left as stored. */
static double steps_fn (int x, int y, int plane, void *ctx)
{
  (void) plane; (void) ctx;
  if (x < 8 && y < 8)
    return 6030;
  return 6000 + (fits_noise (x, y) < 20 ? 1 : 0) + (fits_noise (y, x) < 10 ? 2 : 0);
}

/* The Moon: a bright disc on a black sky, which the auto-stretch blows
   out to white. */
static double moon_fn (int x, int y, int plane, void *ctx)
{
  double dx = x - 32, dy = y - 24;
  (void) plane; (void) ctx;
  if (dx * dx + dy * dy < 15 * 15)
    return 20000 + 40 * (x % 5);
  return 300 + fits_noise (x, y) / 10.0;
}

static double rgb_fn (int x, int y, int plane, void *ctx)
{
  (void) x; (void) y; (void) ctx;
  return plane == 0 ? 0.8 : plane == 1 ? 0.5 : 0.2;
}

static int fits_decode (const char *path, int max_side, rgb_image *img, char *err, size_t en)
{
  thumbgen *t = thumbgen_start (path, NULL, max_side, 1, err, en);
  int r;
  if (!t)
    return -1;
  while ((r = thumbgen_step (t, 64)) == 0)
    ;
  if (r < 0)
    snprintf (err, en, "%s", thumbgen_error (t));
  r = r == 1 ? thumbgen_take (t, img) : -1;
  thumbgen_free (t);
  return r;
}

static void test_fits (void)
{
  char path[1024], path2[1024], err[256], dir[1024], libdir[1024];
  int ctx[2] = { 64, 0 }, k, bad;
  rgb_image a, b;
  fits_hdr h;
  img_info in;
  FILE *f;
  static const char *const meta[] = {
    "DATE-OBS= '2026-11-19T22:41:07.250'", "INSTRUME= 'Canon EOS 70D'", "TELESCOP= 'Skywatcher 80ED'",
    "OBJECT  = 'M42     '", "EXPTIME =                 30.0", "ISOSPEED=                  800",
    "FOCALLEN=                510.0", "CCD-TEMP=                 12.5", "IMAGETYP= 'Light Frame'",
    "SITELAT = '+51 30 00'", "SITELONG= '-0 07 30'", "STACKCNT=                   20", "CREATOR = 'Siril v1.2.4'", NULL };
  static const char *const top_down[] = { "ROWORDER= 'TOP-DOWN'", NULL };
  static const char *const rggb[] = { "BAYERPAT= 'RGGB'", NULL };
  static const char *const ascii_d[] = { "EXPTIME =               3.0D+1", NULL };

  p (dir, "Fits"); mkdir (dir, 0777);
  /* The header */
  p (path, "Fits/sky.fits"); write_fits (path, 16, 64, 48, 1, meta, sky_fn, (ctx[0] = 48, ctx), 0, 0);
  f = fopen (path, "rb");
  {
    unsigned char head[100];
    size_t got = fread (head, 1, sizeof head, f);
    CHECK (fits_is (head, got) && img_probe_named (head, got, path) == IMG_FITS, "fits: known by its first card");
  }
  CHECK (fits_read_header (f, &h, err, sizeof err) == 0 && h.w == 64 && h.h == 48 && h.planes == 1 && h.bitpix == 16
         && h.bzero == 32768 && h.bottom_up && h.data == 2880, "fits: size, type and where the data is");
  CHECK (!strcmp (h.object, "M42") && !strcmp (h.instrume, "Canon EOS 70D") && h.exptime == 30 && h.iso == 800
         && h.focal == 510 && h.has_temp && h.ccdtemp == 12.5 && h.stacked == 20 && !strcmp (h.imagetyp, "Light Frame"),
         "fits: object, camera, exposure, ISO, focal length, temperature, frames stacked, frame type");
  CHECK (h.geo == GEO_POS && h.lat == 515000000 && h.lon == -1250000, "fits: SITELAT and SITELONG as a position");
  fits_to_info (&h, &in);
  CHECK (in.format == IMG_FITS && in.width == 64 && in.height == 48 && !strcmp (in.taken, "2026:11:19 22:41:07")
         && !strcmp (in.model, "Canon EOS 70D") && in.exposure == 30 && in.iso == 800 && in.focal == 510
         && !strcmp (in.encoding, "FITS, 16-bit integers, mono"), "fits: what the library keeps");
  fclose (f);
  CHECK (fits_has_extension ("a.fits") && fits_has_extension ("$.Sky.M42/FIT") && fits_has_extension ("x.fts,fff")
         && !fits_has_extension ("x.fitz"), "fits: its extensions");
  /* The stretch */
  {
    double c0, m;
    CHECK (fabs (fits_mtf (fits_mtf (0.25, 0.02), 0.02) - 0.25) < 1e-9, "fits: the midtones balance puts the median at the target");
    CHECK (fits_stretch_params (0.015, 0.001, &c0, &m) == 0 && c0 > 0.01 && c0 < 0.015 && m < 0.05,
           "fits: a dark linear picture is stretched");
    CHECK (fits_stretch_params (0.45, 0.1, &c0, &m) < 0, "fits: a bright one isn't");
  }
  /* Mono, bottom-up: the star at the top left as shown, the sky stretched to about a quarter. */
  CHECK (fits_decode (path, 1 << 30, &a, err, sizeof err) == 0 && a.w == 64 && a.h == 48, "fits: decoded at full size");
  if (!a.rgb) fprintf (stderr, "fits: %s\n", err);
  if (a.rgb)
    {
      int sky = a.rgb[(20 * 64 + 40) * 3], star = a.rgb[(2 * 64 + 2) * 3], br = a.rgb[(46 * 64 + 2) * 3];
      CHECK (star == 255 && br < 120, "fits: FITS's first row at the bottom (the star shown at the top)");
      CHECK (sky > 30 && sky < 110 && a.rgb[(20 * 64 + 40) * 3 + 1] == sky, "fits: the night sky stretched to be seen, grey");
    }
  ctx[1] = 1;
  p (path2, "Fits/topdown.fits"); write_fits (path2, 16, 64, 48, 1, top_down, sky_fn, ctx, 0, 0);
  CHECK (fits_decode (path2, 1 << 30, &b, err, sizeof err) == 0 && a.rgb && b.rgb
         && !memcmp (a.rgb, b.rgb, (size_t) a.w * a.h * 3), "fits: ROWORDER TOP-DOWN: the same picture");
  free (b.rgb);
  /* Which way up without ROWORDER: as the sky looks, when the WCS says. */
  {
    static const char *const wcs_td[] = { "CD1_1   =    -0.00111834224421", "CD1_2   =   -2.66205442267E-06",
                                          "CD2_1   =    3.43212406814E-06", "CD2_2   =    -0.00111944213785", NULL };
    static const char *const wcs_bu[] = { "CDELT1  =              -0.0011", "CDELT2  =               0.0011", NULL };
    static const char *const dwarf[] = { "ORIGIN  = 'DWARFLAB'", NULL };
    static const char *const dwarf_bu[] = { "ORIGIN  = 'DWARFLAB'", "ROWORDER= 'BOTTOM-UP'", NULL };
    static const char *const mosaic_wcs[] = { "BAYERPAT= 'RGGB'", "CD1_1   =              -0.0011", "CD2_2   =              -0.0011", NULL };
    p (path2, "Fits/wcs.fits"); write_fits (path2, 16, 64, 48, 3, wcs_td, sky_fn, ctx, 0, 0);
    f = fopen (path2, "rb");
    CHECK (fits_read_header (f, &h, err, sizeof err) == 0 && !h.bottom_up, "fits: a WCS mirrored bottom first (DWARF's): top first");
    fclose (f);
    CHECK (fits_decode (path2, 1 << 30, &b, err, sizeof err) == 0 && b.rgb && b.rgb[(2 * 64 + 2) * 3] == 255,
           "fits: and so shown (the star top left as stored)");
    free (b.rgb);
    p (path2, "Fits/wcs2.fits"); write_fits (path2, 16, 64, 48, 1, wcs_bu, sky_fn, ctx, 0, 0);
    f = fopen (path2, "rb");
    CHECK (fits_read_header (f, &h, err, sizeof err) == 0 && h.bottom_up, "fits: a WCS the right way round bottom first: bottom first");
    fclose (f);
    p (path2, "Fits/dwarf.fits"); write_fits (path2, 16, 64, 48, 3, dwarf, sky_fn, ctx, 0, 0);
    f = fopen (path2, "rb");
    CHECK (fits_read_header (f, &h, err, sizeof err) == 0 && !h.bottom_up && !strcmp (h.creator, "DWARFLAB"),
           "fits: DWARF's without a WCS (a mosaic): top first");
    fclose (f);
    p (path2, "Fits/dwarf2.fits"); write_fits (path2, 16, 64, 48, 3, dwarf_bu, sky_fn, ctx, 0, 0);
    f = fopen (path2, "rb");
    CHECK (fits_read_header (f, &h, err, sizeof err) == 0 && h.bottom_up, "fits: ROWORDER is believed first");
    fclose (f);
    p (path2, "Fits/mosaicwcs.fits"); write_fits (path2, 16, 64, 48, 1, mosaic_wcs, sky_fn, ctx, 0, 0);
    f = fopen (path2, "rb");
    CHECK (fits_read_header (f, &h, err, sizeof err) == 0 && h.bottom_up, "fits: a colour mosaic keeps FITS's order (its BAYERPAT)");
    fclose (f);
  }
  /* Only a few steps of a high level, more than half at the median: still stretched. */
  {
    static const char *const steps_cards[] = { "ROWORDER= 'TOP-DOWN'", NULL };
    p (path2, "Fits/steps.fits"); write_fits (path2, 16, 64, 48, 3, steps_cards, steps_fn, NULL, 0, 0);
    CHECK (fits_decode (path2, 1 << 30, &b, err, sizeof err) == 0 && b.rgb, "fits: steps: decoded");
    if (b.rgb)
      {
        int sky = b.rgb[(30 * 64 + 40) * 3], blk = b.rgb[(2 * 64 + 2) * 3];
        CHECK (sky > 20 && sky < 120 && blk > 200, "fits: a few steps above the median: stretched, not a flat grey");
      }
    free (b.rgb);
  }
  /* How much it's stretched (the recipe's Stretch group). */
  {
    static const char *const td[] = { "ROWORDER= 'TOP-DOWN'", NULL };
    double c0a, ma, c0b, mb;
    int moon_full, moon_less, sky_full, sky_less, sky_dark;
    CHECK (fits_stretch_params_ex (0.015, 0.001, 1, 0, &c0a, &ma) == 0 && fits_stretch_params (0.015, 0.001, &c0b, &mb) == 0
           && c0a == c0b && ma == mb, "fits stretch: all of it, the usual black point: the auto-stretch");
    CHECK (fits_stretch_params_ex (0.015, 0.001, 0, 0, &c0a, &ma) == 0 && fabs (ma - 0.5) < 1e-9 && c0a > 0.01,
           "fits stretch: none: no midtones lift, only the black point");
    CHECK (fits_stretch_params_ex (0.015, 0.001, 1, 1, &c0a, &ma) == 0 && c0a > c0b
           && fits_mtf (ma, (0.015 - c0a) / (1 - c0a)) < 0.25 * 0.4, "fits stretch: the black point higher: a darker sky");
    p (path2, "Fits/moon.fits"); write_fits (path2, 16, 64, 48, 1, td, moon_fn, NULL, 0, 0);
    fits_decode (path2, 1 << 30, &b, err, sizeof err);
    moon_full = b.rgb ? b.rgb[(24 * 64 + 32) * 3] : -1;
    sky_full = b.rgb ? b.rgb[(2 * 64 + 2) * 3] : -1;
    free (b.rgb);
    thumbgen_set_fits_stretch (10, 0);
    fits_decode (path2, 1 << 30, &b, err, sizeof err);
    moon_less = b.rgb ? b.rgb[(24 * 64 + 32) * 3] : -1;
    sky_less = b.rgb ? b.rgb[(2 * 64 + 2) * 3] : -1;
    free (b.rgb);
    thumbgen_set_fits_stretch (100, 100);
    fits_decode (path2, 1 << 30, &b, err, sizeof err);
    sky_dark = b.rgb ? b.rgb[(2 * 64 + 2) * 3] : -1;
    free (b.rgb);
    thumbgen_set_fits_stretch (100, 0);
    CHECK (moon_full >= 250 && moon_less < 235 && moon_less > sky_less + 100,
           "fits stretch: the Moon blown out by the auto-stretch, not with less (Amount 10)");
    CHECK (sky_less < sky_full && sky_dark < sky_full, "fits stretch: less, or the black point higher: a darker sky");
  }
  ctx[1] = 0;
  free (a.rgb);
  /* RGB floating point, already bright: shown as it is. */
  p (path, "Fits/rgb.fit"); write_fits (path, -32, 20, 10, 3, NULL, rgb_fn, NULL, 0, 0);
  CHECK (fits_decode (path, 1 << 30, &a, err, sizeof err) == 0 && a.w == 20 && a.h == 10 && a.rgb[0] == 204 && a.rgb[1] == 128
         && a.rgb[2] == 51, "fits: an RGB picture of floating point 0-1, bright, as it is");
  free (a.rgb);
  /* 64-bit float, 8-bit, 32-bit integers */
  p (path, "Fits/rgb64.fits"); write_fits (path, -64, 20, 10, 3, NULL, rgb_fn, NULL, 0, 0);
  CHECK (fits_decode (path, 1 << 30, &a, err, sizeof err) == 0 && a.rgb[0] == 204 && a.rgb[2] == 51, "fits: 64-bit floating point");
  free (a.rgb);
  /* The colour mosaic: a neutral sky, and a red star where only red sites are bright. */
  ctx[0] = 64;
  p (path, "Fits/mosaic.fits"); write_fits (path, 16, 64, 64, 1, rggb, mosaic_fn, ctx, 0, 0);
  CHECK (fits_decode (path, 1 << 30, &a, err, sizeof err) == 0 && a.w == 64 && a.h == 64, "fits mosaic: full size, demosaicked");
  if (a.rgb)
    {
      const unsigned char *s = a.rgb + (40 * 64 + 40) * 3, *st = a.rgb + (3 * 64 + 3) * 3;
      CHECK (abs (s[0] - s[1]) < 20 && abs (s[1] - s[2]) < 20 && s[1] > 30 && s[1] < 110,
             "fits mosaic: each colour stretched on its own: a neutral sky");
      CHECK (st[0] > 200 && st[1] < 120 && st[2] < 120, "fits mosaic: RGGB from the top left as shown: the star is red");
      free (a.rgb);
    }
  CHECK (fits_decode (path, 16, &a, err, sizeof err) == 0 && a.w == 16 && a.h == 16 && a.rgb[0] > 150 && a.rgb[1] < 120,
         "fits mosaic: small, read 2 x 2 to a pixel: still red at the top left");
  free (a.rgb);
  /* An IMAGE extension after an empty primary */
  ctx[0] = 48;
  p (path, "Fits/ext.fits"); write_fits (path, 16, 64, 48, 1, NULL, sky_fn, ctx, 1, 0);
  f = fopen (path, "rb");
  CHECK (fits_read_header (f, &h, err, sizeof err) == 0 && h.hdu == 1 && h.w == 64 && h.data == 2 * 2880,
         "fits: the picture in an IMAGE extension");
  fclose (f);
  /* Tile-compressed (fpack): said so */
  p (path, "Fits/packed.fz"); write_fits (path, 8, 8, 8, 1, NULL, rgb_fn, NULL, 2, 0);
  f = fopen (path, "rb");
  CHECK (fits_read_header (f, &h, err, sizeof err) < 0 && strstr (err, "fpack"), "fits: compressed (fpack): says so");
  fclose (f);
  /* Cut short */
  p (path, "Fits/cut.fits"); write_fits (path, 16, 64, 48, 1, NULL, sky_fn, ctx, 0, 2880 + 1000);
  f = fopen (path, "rb");
  CHECK (fits_read_header (f, &h, err, sizeof err) < 0 && strstr (err, "cut short"), "fits: cut short: said so");
  fclose (f);
  p (path, "Fits/d.fits"); write_fits (path, 8, 4, 4, 1, ascii_d, rgb_fn, NULL, 0, 0);
  f = fopen (path, "rb");
  CHECK (fits_read_header (f, &h, err, sizeof err) == 0 && h.exptime == 30, "fits: a Fortran D exponent");
  fclose (f);
  /* Writing (a stack): read back the same */
  {
    static const char *const wc[] = { "OBJECT  = 'Round trip'", "STACKCNT=                   12", NULL };
    float row[3 * 33], back[3 * 33];
    fits_writer *wr;
    fits_reader *rd;
    int y, x, same = 1, pl;
    for (pl = 0; pl < 2; pl++)
      {
        int bitpix = pl ? 16 : -32;
        p (path, pl ? "Fits/written16.fits" : "Fits/written.fits");
        wr = fits_writer_open (path, 33, 21, 3, bitpix, wc, err, sizeof err);
        CHECK (wr != NULL, "fits writer: opened");
        if (!wr)
          continue;
        for (y = 20; y >= 0; y--)                     /* any order */
          {
            for (x = 0; x < 33 * 3; x++)
              row[x] = (float) ((x * 7 + y * 13) % 100) / 100;
            if (y == 5)
              row[4] = NAN;
            fits_writer_row (wr, y, row);
          }
        CHECK (fits_writer_close (wr) == 0, "fits writer: closed");
        f = fopen (path, "rb");
        CHECK (f && fits_read_header (f, &h, err, sizeof err) == 0 && h.w == 33 && h.h == 21 && h.planes == 3
               && h.bitpix == bitpix && h.bottom_up && h.stacked == 12 && !strcmp (h.object, "Round trip"),
               "fits writer: its header read back");
        rd = f ? fits_reader_open (f, &h, 0, err, sizeof err) : NULL;
        for (y = 0; rd && y < 21; y++)
          {
            fits_reader_row_linear (rd, y, back);
            for (x = 0; x < 33 * 3; x++)
              {
                float want = (y == 5 && x == 4) ? 0 : (float) ((x * 7 + y * 13) % 100) / 100;
                if (fabsf (back[x] - want) > (pl ? 1.0f / 65535 : 1.0f / 65535))
                  same = 0;
              }
          }
        CHECK (rd && same, pl ? "fits writer: 16-bit, rows the right way up, values back" : "fits writer: floating point, rows the right way up, values back (NAN as 0)");
        fits_reader_free (rd);
        if (f)
          fclose (f);
        {
          struct stat sb;
          CHECK (stat (path, &sb) == 0 && sb.st_size % 2880 == 0, "fits writer: whole blocks");
        }
        remove (path);
      }
  }
  /* Importing: the library knows them */
  {
    library *l;
    cat_item *items;
    cat_details d;
    cat_source all = { SRC_ALL, 0, 0, "" };
    cat_origin o;
    int n, found = 0;
    p (path, "Fits/d.fits"); remove (path);   /* 4 x 4 is fine, but keep the count simple */
    p (path, "Fits/cut.fits"); remove (path);
    p (path, "Fits/packed.fz"); remove (path);
    /* Typed FITS, without an extension (as a RISC OS file can be) */
    ctx[0] = 24;
    p (path, "Fits/M31-mosaic,6a3"); write_fits (path, 16, 32, 24, 1, NULL, sky_fn, ctx, 0, 0);
    p (libdir, "FitsLibrary");
    CHECK (lib_open (&l, libdir, err, sizeof err) == 0, "fits: library");
    lib_import (l, dir);
    drain (l);
    cat_list (lib_cat (l), &all, &items, &n);
    CHECK (n == 14, "fits: all fourteen imported, one typed FITS without an extension");
    bad = 0;
    for (k = 0; k < n; k++)
      {
        if (items[k].thumb_state != THUMB_READY)
          bad++;
        if (cat_get_details (lib_cat (l), items[k].id, &d) == 0 && strstr (d.path, "sky.fits")
            && cat_origin_of (lib_cat (l), d.original, &o) == 1)
          found = o.format == IMG_FITS && d.width == 64 && d.height == 48 && !strcmp (d.taken, "2026:11:19 22:41:07")
                  && !strcmp (o.encoding, "FITS, 16-bit integers, mono");
      }
    CHECK (!bad, "fits: thumbnails made for all");
    CHECK (found, "fits: its format, size, date and encoding kept");
    {
      int guard = 0;
      cat_geo g;
      while (lib_geo_pending (l) && guard++ < 1000)
        lib_geo_step (l, fake_now, clock_cs + 50);
      found = 0;
      for (k = 0; k < n; k++)
        if (cat_get_details (lib_cat (l), items[k].id, &d) == 0 && strstr (d.path, "sky.fits"))
          found = cat_get_geo (lib_cat (l), items[k].id, &g) == 1 && g.lat == 515000000 && g.lon == -1250000;
      CHECK (found, "fits: where it was taken, from SITELAT and SITELONG, for maps");
    }
    free (items);
    lib_close (l);
  }
}


/* --- lining up frames of the night sky (astro) --- */

static unsigned long long arng = 88172645463325252ull;
static double aurand (void)
{
  arng ^= arng << 13; arng ^= arng >> 7; arng ^= arng << 17;
  return (double) (arng >> 11) / 9007199254740992.0;
}
static double agauss (void)
{
  double u = aurand () + 1e-12, v = aurand ();
  return sqrt (-2 * log (u)) * cos (2 * M_PI * v);
}

typedef struct { double x, y, f; } truth;

/* A w x h plane: a sky with a gradient, noise, and Gaussian stars
   (sigma 1.2 pixels) at the given places. */
static void render_sky (float *px, int w, int h, const truth *st, int n, double noise)
{
  int x, y, k;
  for (y = 0; y < h; y++)
    for (x = 0; x < w; x++)
      px[(size_t) y * w + x] = (float) (0.05 + 0.03 * x / w + 0.01 * y / h + noise * agauss ());
  for (k = 0; k < n; k++)
    {
      int x0 = (int) st[k].x, y0 = (int) st[k].y;
      for (y = y0 - 6; y <= y0 + 6; y++)
        for (x = x0 - 6; x <= x0 + 6; x++)
          if (x >= 0 && y >= 0 && x < w && y < h)
            {
              double dx = x - st[k].x, dy = y - st[k].y;
              px[(size_t) y * w + x] += (float) (st[k].f * exp (-(dx * dx + dy * dy) / (2 * 1.2 * 1.2)));
            }
    }
}

/* The largest distance between where two transforms put the corners. */
static double xform_err (const astro_xform *a, const astro_xform *b, int w, int h)
{
  double e = 0;
  int k;
  for (k = 0; k < 4; k++)
    {
      double x = k & 1 ? w - 1 : 0, y = k & 2 ? h - 1 : 0, ax, ay, bx, by, d;
      astro_apply (a, x, y, &ax, &ay);
      astro_apply (b, x, y, &bx, &by);
      d = hypot (ax - bx, ay - by);
      if (d > e)
        e = d;
    }
  return e;
}

static int align_case (const char *name, double dx, double dy, double deg, int w, int h, double *err, astro_fit *fit)
{
  truth *t1 = malloc (220 * sizeof *t1), *t2 = malloc (220 * sizeof *t2);
  float *p1 = malloc ((size_t) w * h * sizeof *p1), *p2 = malloc ((size_t) w * h * sizeof *p2);
  astro_star *s1 = malloc (300 * sizeof *s1), *s2 = malloc (300 * sizeof *s2);
  astro_xform truth_t, got;
  astro_plane a = { w, h, p1, 1 }, b = { w, h, p2, 1 };
  int n1 = 0, n2 = 0, k, r;
  double th = deg * M_PI / 180;
  (void) name;
  truth_t.a = cos (th); truth_t.b = sin (th); truth_t.tx = dx; truth_t.ty = dy;
  for (k = 0; k < 180; k++)
    {
      t1[n1].x = 10 + aurand () * (w - 20);
      t1[n1].y = 10 + aurand () * (h - 20);
      t1[n1].f = 0.02 + 0.5 * pow (aurand (), 3);
      n1++;
    }
  /* The frame: the same stars moved (a tenth gone: cloud, the edge), and a few others. */
  for (k = 0; k < n1; k++)
    {
      double x, y;
      astro_apply (&truth_t, t1[k].x, t1[k].y, &x, &y);
      if (k % 10 == 3 || x < 8 || y < 8 || x > w - 9 || y > h - 9)
        continue;
      t2[n2].x = x; t2[n2].y = y; t2[n2].f = t1[k].f * 0.9; n2++;
    }
  for (k = 0; k < 15 && n2 < 220; k++)
    {
      t2[n2].x = 10 + aurand () * (w - 20); t2[n2].y = 10 + aurand () * (h - 20); t2[n2].f = 0.05; n2++;
    }
  render_sky (p1, w, h, t1, n1, 0.002);
  render_sky (p2, w, h, t2, n2, 0.002);
  /* Hot pixels and a satellite trail in the frame. */
  for (k = 0; k < 40; k++)
    p2[(size_t) (20 + (int) (aurand () * (h - 40))) * w + 20 + (int) (aurand () * (w - 40))] = 0.9f;
  for (k = 0; k < w; k++)
    {
      int y = h / 3 + k / 4;
      if (y < h)
        p2[(size_t) y * w + k] += 0.3f;
    }
  r = astro_find_stars (&a, 0, s1, 300, NULL);
  k = astro_find_stars (&b, 0, s2, 300, NULL);
  r = r > 0 && k > 0 ? astro_match (s1, r, s2, k, &got, fit) : -1;
  *err = r == 0 ? xform_err (&got, &truth_t, w, h) : 1e9;
  free (t1); free (t2); free (p1); free (p2); free (s1); free (s2);
  return r;
}

static void test_align (void)
{
  int w = 800, h = 600, n, k, close = 0, hotfound = 0;
  float *px = malloc ((size_t) w * h * sizeof *px);
  truth st[150];
  astro_star found[300];
  astro_stats as;
  astro_plane pl = { w, h, px, 1 };
  astro_fit fit;
  double err;
  /* Finding stars */
  for (k = 0; k < 150; k++)
    {
      st[k].x = 10 + aurand () * (w - 20);
      st[k].y = 10 + aurand () * (h - 20);
      st[k].f = 0.03 + 0.4 * aurand ();
    }
  render_sky (px, w, h, st, 150, 0.002);
  for (k = 0; k < 20; k++)
    px[(size_t) (50 + 20 * k) * w + 30 + 35 * k] = 0.95f;   /* hot pixels */
  n = astro_find_stars (&pl, 0, found, 300, &as);
  for (k = 0; k < n; k++)
    {
      int j;
      for (j = 0; j < 150; j++)
        if (hypot (found[k].x - st[j].x, found[k].y - st[j].y) < 0.1)
          {
            close++;
            break;
          }
      for (j = 0; j < 20; j++)
        if (hypot (found[k].x - (30 + 35 * j), found[k].y - (50 + 20 * j)) < 1.5)
          hotfound++;
    }
  CHECK (n >= 140 && n <= 160, "align: the stars found");
  CHECK (close >= 135, "align: each measured to a tenth of a pixel");
  CHECK (hotfound == 0 && as.rejected_hot >= 18, "align: hot pixels aren't stars");
  CHECK (fabs (as.noise - 0.002) < 0.0004, "align: the sky's noise measured");
  CHECK (n < 2 || found[0].flux >= found[1].flux, "align: brightest first");
  /* Matching: shifts and turns, a meridian flip, with hot pixels, a satellite and missing stars */
  CHECK (align_case ("shift", 13.37, -7.21, 0, w, h, &err, &fit) == 0 && err < 0.1 && fit.pairs >= 100,
         "align: a shift found to a tenth of a pixel");
  CHECK (align_case ("turn", -20.5, 31.25, 0.8, w, h, &err, &fit) == 0 && err < 0.1 && fabs (fabs (fit.angle) - 0.8) < 0.01
         && fabs (fit.scale - 1) < 1e-3, "align: a shift and a slight turn (a tripod, a few minutes)");
  CHECK (align_case ("turn5", 40, -12, 5, w, h, &err, &fit) == 0 && err < 0.1, "align: five degrees");
  CHECK (align_case ("flip", w - 1 + 3.5, h - 1 - 2.25, 180, w, h, &err, &fit) == 0 && err < 0.1,
         "align: turned right round (a meridian flip)");
  printf ("test_core: align: %d stars agree, %.3f px rms\n", fit.pairs, fit.rms);
  CHECK (fit.rms < 0.2, "align: the stars within a fifth of a pixel of where it puts them (rms)");
  /* Different skies don't line up */
  {
    truth a[100], b[100];
    float *p2 = malloc ((size_t) w * h * sizeof *p2);
    astro_star s1[200], s2[200];
    astro_plane pa = { w, h, px, 1 }, pb = { w, h, p2, 1 };
    astro_xform t;
    int n1, n2;
    for (k = 0; k < 100; k++)
      {
        a[k].x = 10 + aurand () * (w - 20); a[k].y = 10 + aurand () * (h - 20); a[k].f = 0.1 + 0.3 * aurand ();
        b[k].x = 10 + aurand () * (w - 20); b[k].y = 10 + aurand () * (h - 20); b[k].f = 0.1 + 0.3 * aurand ();
      }
    render_sky (px, w, h, a, 100, 0.002);
    render_sky (p2, w, h, b, 100, 0.002);
    n1 = astro_find_stars (&pa, 0, s1, 200, NULL);
    n2 = astro_find_stars (&pb, 0, s2, 200, NULL);
    CHECK (astro_match (s1, n1, s2, n2, &t, NULL) < 0, "align: two different skies don't line up");
    CHECK (astro_match (s1, 2, s2, 2, &t, NULL) < 0, "align: two stars aren't enough");
    /* Lined up by hand */
    {
      double r[4] = { 100, 100, 500, 400 }, f[4], x, y;
      astro_xform tt = { cos (0.1), sin (0.1), 5, -3 }, got;
      astro_apply (&tt, r[0], r[1], &f[0], &f[1]);
      astro_apply (&tt, r[2], r[3], &f[2], &f[3]);
      CHECK (astro_xform_from_pairs (r, f, 2, &got) == 0 && xform_err (&got, &tt, w, h) < 1e-6, "align by hand: two stars give the turn");
      CHECK (astro_xform_from_pairs (r, f, 1, &got) == 0 && got.a == 1 && got.b == 0 && fabs (got.tx - (f[0] - 100)) < 1e-9,
             "align by hand: one star gives the shift");
      CHECK (astro_xform_from_pairs (r, r, 1, &got) == 0 && xform_err (&got, &(astro_xform) { 1, 0, 0, 0 }, w, h) < 1e-9,
             "align by hand: the same place, no move");
      (void) x; (void) y;
    }
    /* Moving a frame: the moved frame matches the reference */
    {
      astro_xform tt;
      astro_img src = { w, h, 1, p2, 0, 0 }, dst = { 0, 0, 0, NULL, 0, 0 };
      double sum = 0, peak = 0;
      int cnt = 0, nan_edge;
      truth moved[100];
      tt.a = cos (0.02); tt.b = sin (0.02); tt.tx = 7.3; tt.ty = -4.6;
      for (k = 0; k < 100; k++)
        {
          astro_apply (&tt, a[k].x, a[k].y, &moved[k].x, &moved[k].y);
          moved[k].f = a[k].f;
        }
      render_sky (px, w, h, a, 100, 0);
      render_sky (p2, w, h, moved, 100, 0);
      dst.w = w; dst.h = h; dst.ch = 1; dst.px = malloc ((size_t) w * h * sizeof *dst.px);
      astro_warp (&src, &tt, &dst, 0, h);
      for (k = 0; k < w * h; k++)
        {
          int x = k % w, y = k / w;
          if (x > 40 && y > 40 && x < w - 40 && y < h - 40 && dst.px[k] == dst.px[k])
            {
              double d = fabs (dst.px[k] - px[k]);
              sum += d;
              cnt++;
              if (d > peak)
                peak = d;
            }
        }
      nan_edge = dst.px[(size_t) (h - 1) * w + 2] != dst.px[(size_t) (h - 1) * w + 2];
      CHECK (cnt > 0 && sum / cnt < 2e-4, "align: a moved frame matches the reference (on average)");
      CHECK (peak < 0.05, "align: and at every star's peak, closely");
      CHECK (nan_edge, "align: what the frame didn't cover is left out (NAN)");
      /* RGB the same as each colour alone */
      {
        astro_img s3 = { w, h, 3, malloc ((size_t) w * h * 3 * sizeof (float)), 0, 0 }, d3 = { w, h, 3, malloc ((size_t) w * h * 3 * sizeof (float)), 0, 0 };
        int same = 1, i;
        for (i = 0; i < w * h; i++)
          s3.px[3 * i] = p2[i], s3.px[3 * i + 1] = 0.5f * p2[i], s3.px[3 * i + 2] = 1 - p2[i];
        astro_warp (&s3, &tt, &d3, 0, h);
        for (i = 0; i < w * h; i++)
          {
            float m = dst.px[i];
            if (m != m ? d3.px[3 * i] == d3.px[3 * i] : fabsf (d3.px[3 * i] - m) > 1e-6f || fabsf (d3.px[3 * i + 1] - 0.5f * m) > 1e-6f
                || fabsf (d3.px[3 * i + 2] - (1 - m)) > 1e-5f)
              same = 0;
          }
        CHECK (same, "align: an RGB frame moved as each colour alone");
        astro_warp (&s3, &(astro_xform) { 1, 0, 0, 0 }, &d3, 0, h);
        CHECK (!memcmp (s3.px, d3.px, (size_t) w * h * 3 * sizeof (float)), "align: the reference itself, as it is");
        free (s3.px);
        free (d3.px);
      }
      free (dst.px);
    }
    free (p2);
  }
  free (px);
}


/* --- stacking (stack) --- */

#define SW 360
#define SH 260
static float *stack_img;               /* the picture write_fits takes values from (mosaic, top row first) */
static double stack_fn (int x, int y, int plane, void *ctx)
{
  (void) plane; (void) ctx;
  return stack_img[(size_t) (SH - 1 - y) * SW + x];   /* stored bottom-up */
}
static int cmp_float_t (const void *a, const void *b)
{
  float x = *(const float *) a, y = *(const float *) b;
  return x < y ? -1 : x > y;
}

/* A light: the sky's stars moved by t, as an RGGB mosaic in 16-bit units,
   with the sensor's hot pixels, dark glow, noise, and perhaps a satellite. */
static void stack_light (const truth *st, int n, const astro_xform *t, int satellite, int cloudy, unsigned seed)
{
  int x, y, k;
  arng = 1234567ull + seed * 7919ull;
  for (y = 0; y < SH; y++)
    for (x = 0; x < SW; x++)
      {
        int red = !(y & 1) && !(x & 1), blue = (y & 1) && (x & 1);
        double sky = 3000 + 400.0 * x / SW, v = sky * (red ? 0.6 : blue ? 0.7 : 1.0);
        stack_img[(size_t) y * SW + x] = (float) (v + 2048 + 40 * agauss ());   /* 2048: the camera's black level */
      }
  for (k = 0; k < n && !cloudy; k++)
    {
      double fx, fy;
      int x0, y0;
      astro_apply (t, st[k].x, st[k].y, &fx, &fy);
      x0 = (int) fx; y0 = (int) fy;
      for (y = y0 - 6; y <= y0 + 6; y++)
        for (x = x0 - 6; x <= x0 + 6; x++)
          if (x >= 0 && y >= 0 && x < SW && y < SH)
            {
              double dx = x - fx, dy = y - fy;
              stack_img[(size_t) y * SW + x] += (float) (st[k].f * exp (-(dx * dx + dy * dy) / (2 * 1.6 * 1.6)));
            }
    }
  if (satellite)
    for (x = 0; x < SW; x++)
      {
        y = SH / 4 + x / 3;
        if (y < SH)
          stack_img[(size_t) y * SW + x] += 20000;
      }
  /* The sensor's own: dark glow and hot pixels, the same in every frame. */
  for (k = 0; k < 60; k++)
    stack_img[(size_t) (13 + (k * 37) % (SH - 26)) * SW + 11 + (k * 53) % (SW - 22)] += 30000;
  for (y = 0; y < SH; y++)
    for (x = 0; x < SW; x++)
      {
        float v = stack_img[(size_t) y * SW + x] + (float) (300.0 * y / SH);
        stack_img[(size_t) y * SW + x] = v > 65535 ? 65535 : v;
      }
}

static void stack_dark (unsigned seed)
{
  int x, y, k;
  arng = 99991ull + seed * 104729ull;
  for (y = 0; y < SH; y++)
    for (x = 0; x < SW; x++)
      stack_img[(size_t) y * SW + x] = (float) (2048 + 300.0 * y / SH + 40 * agauss ());
  for (k = 0; k < 60; k++)
    stack_img[(size_t) (13 + (k * 37) % (SH - 26)) * SW + 11 + (k * 53) % (SW - 22)] += 30000;
}

static void test_stack (void)
{
  static const char *const rggb[] = { "BAYERPAT= 'RGGB'", "ROWORDER= 'BOTTOM-UP'", "INSTRUME= 'Canon EOS 70D'",
                                      "EXPTIME =                 30.0", "OBJECT  = 'Test field'", NULL };
  truth st[80];
  char lights[7][1024], darks[3][1024], out[1024], err[400], what[200];
  const char *lp[7], *dp[3];
  int k, r, guard = 0, nf, pct, last = -1, monotonic = 1;
  stack_job *j;
  const stack_frame *fr;
  stack_img = malloc ((size_t) SW * SH * sizeof *stack_img);
  arng = 4242;
  for (k = 0; k < 80; k++)
    {
      st[k].x = 20 + aurand () * (SW - 40);
      st[k].y = 20 + aurand () * (SH - 40);
      st[k].f = 1500 + 20000 * pow (aurand (), 2);
    }
  p (out, "Stack"); mkdir (out, 0777);
  for (k = 0; k < 7; k++)
    {
      astro_xform t;
      double th = 0.004 * k;
      t.a = cos (th); t.b = sin (th); t.tx = 1.7 * k - 3; t.ty = -1.1 * k + 2;
      stack_light (st, 80, &t, k == 2, k == 5, (unsigned) k);
      snprintf (lights[k], sizeof lights[k], "%s/Stack/Light_%d.fits", work, k + 1);
      write_fits (lights[k], 16, SW, SH, 1, rggb, stack_fn, NULL, 0, 0);
      lp[k] = lights[k];
    }
  for (k = 0; k < 3; k++)
    {
      stack_dark ((unsigned) k);
      snprintf (darks[k], sizeof darks[k], "%s/Stack/Dark_%d.fits", work, k + 1);
      write_fits (darks[k], 16, SW, SH, 1, rggb, stack_fn, NULL, 0, 0);
      dp[k] = darks[k];
    }
  snprintf (out, sizeof out, "%s/Stack/Stacked.fits", work);
  /* Refused at once */
  CHECK (!stack_start (lp, 1, NULL, 0, out, "Lumen test", err, sizeof err) && strstr (err, "two"), "stack: one frame isn't enough");
  {
    char other[1024];
    const char *mixed[2];
    p (other, "Fits/rgb.fit");
    mixed[0] = lp[0]; mixed[1] = other;
    CHECK (!stack_start (mixed, 2, NULL, 0, out, "Lumen test", err, sizeof err) && strstr (err, "same kind"),
           "stack: frames of different sizes or kinds refused, saying which");
    p (other, "Fits/rgb.fit");
    mixed[0] = lp[0]; mixed[1] = "no such file";
    CHECK (!stack_start (mixed, 2, NULL, 0, out, "Lumen test", err, sizeof err), "stack: a missing frame refused");
  }
  /* The real thing: 7 lights (one with a satellite, one clouded out), 3 darks */
  j = stack_start (lp, 7, dp, 3, out, "Lumen test", err, sizeof err);
  CHECK (j != NULL, "stack: started");
  if (!j)
    {
      fprintf (stderr, "stack: %s\n", err);
      return;
    }
  while ((r = stack_step (j, stack_now, stack_clock + 5)) == 0 && guard++ < 1000000)
    {
      pct = stack_progress (j, what, sizeof what);
      if (pct < last)
        monotonic = 0;
      last = pct;
    }
  CHECK (r == 1, "stack: finished");
  if (r != 1)
    fprintf (stderr, "stack: %s\n", stack_error (j));
  CHECK (guard > 50, "stack: a little at a time");
  CHECK (monotonic && stack_progress (j, what, sizeof what) == 100, "stack: progress only goes forward, to 100");
  nf = stack_frames (j, &fr);
  {
    int used = 0, cloudy_out = 0, refs = 0, good = 1;
    for (k = 0; k < nf; k++)
      {
        used += fr[k].used;
        refs += fr[k].reference;
        if (k == 5)
          cloudy_out = !fr[k].used && strstr (fr[k].why, "star") != NULL;
        if (fr[k].used && !fr[k].reference && (fr[k].rms > 0.3 || fr[k].pairs < 20))
          good = 0;
      }
    CHECK (nf == 7 && used == 6 && refs == 1, "stack: six stacked, one the reference");
    CHECK (cloudy_out, "stack: the clouded-out frame left out, and why");
    CHECK (good, "stack: each lined up to a fraction of a pixel");
  }
  stack_summary (j, what, sizeof what);
  CHECK (strstr (what, "6 of 7 frames stacked") && strstr (what, "3 darks"), "stack: the summary");
  stack_free (j);
  /* The result: as big as a light, RGB, a FITS header saying what it is */
  {
    FILE *f = fopen (out, "rb");
    fits_hdr h;
    fits_reader *rd;
    float *row = malloc ((size_t) SW * 3 * sizeof *row), *all = malloc ((size_t) SW * SH * 3 * sizeof *all);
    int y, x, hot_left = 0;
    double sky_sd = 0, single_sd = 0;
    CHECK (f && fits_read_header (f, &h, err, sizeof err) == 0 && h.w == SW && h.h == SH && h.planes == 3 && h.bitpix == -32
           && h.stacked == 6 && h.exptime == 180 && !strcmp (h.object, "Test field") && !strcmp (h.instrume, "Canon EOS 70D"),
           "stack: the result: RGB floating point, 6 frames, 180 s, the object and camera");
    rd = f ? fits_reader_open (f, &h, 0, err, sizeof err) : NULL;
    for (y = 0; rd && y < SH; y++)
      {
        fits_reader_row_linear (rd, y, row);
        memcpy (all + (size_t) y * SW * 3, row, (size_t) SW * 3 * sizeof *row);
      }
    if (rd)
      {
        /* The hot pixels (the darks took them away; where the frames
           moved, sigma clipping would have) */
        for (k = 0; k < 60; k++)
          {
            int hx = 11 + (k * 53) % (SW - 22), hy = 13 + (k * 37) % (SH - 26);
            const float *q = all + ((size_t) hy * SW + hx) * 3, *nb = all + ((size_t) hy * SW + hx + 3) * 3;
            if (fabsf (q[1] - nb[1]) > 0.05f)
              hot_left++;
          }
        CHECK (hot_left <= 3, "stack: no hot pixels left");
        /* The satellite's line: gone */
        {
          int trail = 0;
          for (x = 30; x < SW - 30; x += 10)
            {
              int ty = SH / 4 + x / 3;
              const float *q = all + ((size_t) ty * SW + x) * 3, *nb = all + ((size_t) (ty + 6) * SW + x) * 3;
              if (q[1] - nb[1] > 0.05f)
                trail++;
            }
          CHECK (trail <= 2, "stack: no satellite trail");
        }
        /* Quieter than one frame: the noise (the median difference
           between pixels four apart, which stars and hot pixels don't
           move) */
        {
          static float d[SW * SH];
          int n = 0;
          for (y = 20; y < SH - 20; y += 3)
            for (x = 20; x < SW - 24; x++)
              d[n++] = fabsf (all[((size_t) y * SW + x) * 3 + 1] - all[((size_t) y * SW + x + 4) * 3 + 1]);
          qsort (d, (size_t) n, sizeof *d, cmp_float_t);
          sky_sd = d[n / 2];
        }
        fits_reader_free (rd);
      }
    if (f)
      fclose (f);
    /* One light alone, read the same way, for comparison */
    f = fopen (lights[0], "rb");
    if (f && fits_read_header (f, &h, err, sizeof err) == 0 && (rd = fits_reader_open (f, &h, 0, err, sizeof err)))
      {
        static float d[SW * SH];
        int n = 0;
        for (y = 20; y < SH - 20; y += 3)
          {
            fits_reader_row_linear (rd, y, row);
            for (x = 20; x < SW - 24; x++)
              d[n++] = fabsf (row[x * 3 + 1] - row[(x + 4) * 3 + 1]);
          }
        qsort (d, (size_t) n, sizeof *d, cmp_float_t);
        single_sd = d[n / 2];
        fits_reader_free (rd);
      }
    if (f)
      fclose (f);
    printf ("test_core: stack: noise %.6f stacked, %.6f in one frame\n", sky_sd, single_sd);
    CHECK (sky_sd > 0 && sky_sd < 0.6 * single_sd, "stack: the sky quieter than in one frame");
    /* A star in the reference where the stack has it */
    free (row);
    free (all);
  }
  /* Stopped half way: nothing left behind */
  {
    struct stat sb;
    char out2[1024];
    snprintf (out2, sizeof out2, "%s/Stack/Stopped.fits", work);
    j = stack_start (lp, 7, NULL, 0, out2, "Lumen test", err, sizeof err);
    guard = 0;
    while (j && stack_step (j, stack_now, stack_clock + 5) == 0 && stack_progress (j, what, sizeof what) < 70 && guard++ < 1000000)
      ;
    stack_free (j);
    CHECK (j && stat (out2, &sb) != 0, "stack: stopped half way: no part-written file left");
  }
  free (stack_img);
}

/* --- moving photos between libraries (libxfer) */

static long long sql1 (library *l, const char *sql)
{
  sqlite3_stmt *s;
  long long v = -1;
  if (sqlite3_prepare_v2 (cat_sqlite (lib_cat (l)), sql, -1, &s, NULL) != SQLITE_OK)
    return -2;
  if (sqlite3_step (s) == SQLITE_ROW)
    v = sqlite3_column_int64 (s, 0);
  sqlite3_finalize (s);
  return v;
}

static int xfer_drain (libxfer *x, int *list_changed)
{
  int guard = 0, f;
  while (!libx_finished (x) && guard++ < 100000)
    {
      f = libx_work (x, frozen_now, 0);
      if (list_changed && (f & LIB_LIST_CHANGED))
        (*list_changed)++;
    }
  return guard;
}

static int file_there (const char *rel)
{
  char path[1024];
  struct stat st;
  p (path, rel);
  return stat (path, &st) == 0;
}

static cat_id version_named (library *l, const char *name)
{
  cat_item *items;
  int n, i;
  cat_id id = 0;
  cat_source all = { SRC_ALL, 0, 0, "" };
  if (cat_list (lib_cat (l), &all, &items, &n))
    return 0;
  for (i = 0; i < n; i++)
    if (!strcmp (items[i].name, name) && !id)
      id = items[i].id;
  free (items);
  return id;
}

/* Presets as files (presets.h). */
static void test_presets (void)
{
  char dir[1024], path[1024], name[48], look[2048], err[300];
  cat_preset *ps;
  int n;
  FILE *f;
  library *a;

  p (dir, "Presets");
  presets_set_dir (dir);
  CHECK (presets_list (&ps, &n) == 0 && n == 0 && ps == NULL, "presets: none yet (no directory)");
  CHECK (presets_save ("Show day", "exposure ev=0.35\nenhance contrast=20\n") == 0
         && presets_save ("cool: blue/teal", "wb temp=-20") == 0
         && presets_save ("", "wb temp=1") == -1, "presets: saved (a name is needed)");
  p (path, "Presets/Show day");
  CHECK (presets_read_file (path, name, sizeof name, look, sizeof look) == 0 && !strcmp (name, "Show day")
         && !strcmp (look, "exposure ev=0.35\nenhance contrast=20\n"), "presets: a file named after it, read back");
  p (path, "Presets/cool_ blue_teal");
  CHECK (presets_read_file (path, name, sizeof name, look, sizeof look) == 0 && !strcmp (name, "cool: blue/teal")
         && !strcmp (look, "wb temp=-20\n"), "presets: what a leaf can't have is _ (the name keeps it)");
  f = fopen ((p (path, "Presets/Notes"), path), "w");
  if (f)
    {
      fputs ("Not a preset\n", f);
      fclose (f);
    }
  f = fopen ((p (path, "Presets/Renamed"), path), "w");
  if (f)
    {
      fputs ("Lumen preset 1\r\nName:  Golden hour  \r\nwb temp=15\r\n", f);
      fclose (f);
    }
  CHECK (presets_list (&ps, &n) == 0 && n == 3 && !strcmp (ps[0].name, "cool: blue/teal") && ps[0].id == 1
         && !strcmp (ps[1].name, "Golden hour") && !strcmp (ps[1].recipe, "wb temp=15\n")
         && !strcmp (ps[2].name, "Show day") && ps[2].id == 3,
         "presets: listed by name, any case; its Name counts, not the file's; other files ignored; CR LF too");
  free (ps);
  CHECK (presets_save ("SHOW DAY", "exposure ev=1") == 0 && presets_list (&ps, &n) == 0 && n == 3
         && !strcmp (ps[2].name, "SHOW DAY") && !strcmp (ps[2].recipe, "exposure ev=1\n"),
         "presets: saving one of the same name (any case) replaces it");
  free (ps);
  CHECK (file_there ("Presets/Show day") && !file_there ("Presets/SHOW DAY"), "presets: ... in its file");
  CHECK (presets_delete ("golden HOUR") == 0 && presets_delete ("Golden hour") == -1
         && !file_there ("Presets/Renamed"), "presets: deleted by name (its file goes)");

  /* A file from elsewhere (shared). */
  p (path, "shared preset");
  CHECK (presets_write_file (path, "Mono", "saturation=-100") == 0, "presets: a copy written anywhere");
  CHECK (presets_add_file (path) == 0 && presets_list (&ps, &n) == 0 && n == 3 && !strcmp (ps[1].name, "Mono"),
         "presets: and added from there");
  free (ps);
  CHECK (presets_add_file ((p (path, "Presets/Notes"), path)) == -1, "presets: not from a file that isn't one");

  /* A library's presets (from before) move in. */
  p (path, "PresetLib");
  CHECK (lib_open (&a, path, err, sizeof err) == 0, "presets: a library");
  cat_preset_save (lib_cat (a), "Old", "exposure ev=0.2");
  cat_preset_save (lib_cat (a), "mono", "saturation=-50");
  CHECK (presets_take_from (lib_cat (a)) == 1, "presets: one new one from the library");
  CHECK (cat_presets (lib_cat (a), &ps, &n) == 0 && n == 0, "presets: the catalogue's are gone");
  free (ps);
  CHECK (presets_list (&ps, &n) == 0 && n == 4 && !strcmp (ps[1].name, "Mono") && !strcmp (ps[1].recipe, "saturation=-100\n")
         && !strcmp (ps[2].name, "Old"), "presets: Old added; Mono (there already) kept as it was");
  free (ps);
  lib_close (a);
  presets_set_dir ("");
}

static void test_libxfer (void)
{
  library *a, *b;
  libxfer *x;
  libx_survey sv;
  char err[300], path[1024], dst[1024], msg[400];
  cat *c;
  cat_id va, vb, vc, va2, sel[2];
  cat_rule rule;
  cat_details d;
  int failed, changed = 0;

  p (path, "Xfer/Trip/sub");
  fsys_mkdirs (path);
  p (path, "Xfer/Other");
  fsys_mkdirs (path);
  p (path, "Xfer/Trip/a.jpg");     make_jpeg (path, 90, 60, 1, "2024:05:01 10:00:00");
  p (path, "Xfer/Trip/b.jpg");     make_jpeg (path, 92, 60, 1, "2024:05:01 11:00:00");
  p (path, "Xfer/Trip/sub/c.jpg"); make_jpeg (path, 94, 60, 1, "2024:05:02 09:00:00");
  p (path, "Xfer/Other/d.jpg");    make_jpeg (path, 96, 60, 1, "2024:06:01 09:00:00");
  p (path, "Xfer/Other/copy-of-a.jpg"); make_jpeg (path, 90, 60, 1, "2024:05:01 10:00:00");

  /* Library A: a watched project with folder albums, and everything a
     photo can have. */
  p (path, "XferA");
  CHECK (lib_open (&a, path, err, sizeof err) == 0, "libxfer: library A");
  c = lib_cat (a);
  lib_set_new_folders (a, 1, 1);
  p (path, "Xfer/Trip");
  lib_import (a, path);
  drain (a);
  va = version_named (a, "a.jpg");
  vb = version_named (a, "b.jpg");
  vc = version_named (a, "c.jpg");
  CHECK (va && vb && vc, "libxfer: three photos in A");
  cat_set_rating (c, &va, 1, 4);
  cat_set_recipe (c, vb, "exposure ev=0.5");
  cat_add_keywords (c, &va, 1, "coast");
  cat_add_keywords (c, &vc, 1, "coast, harbour");
  cat_set_caption (c, &vc, 1, "The harbour wall");
  cat_set_label (c, &vc, 1, 2);
  sel[0] = va;
  sel[1] = vb;
  cat_album_add (c, cat_album_create (c, "Best"), sel, 2);
  memset (&rule, 0, sizeof rule);
  rule.min_rating = 3;
  cat_smart_create (c, "Good", &rule);
  va2 = cat_new_version (c, va, 0);
  cat_set_recipe (c, va2, "enhance contrast=30");
  cat_preset_save (c, "Warm", "wb temp=10");
  lib_thumbs_changed (a);
  drain (a);
  CHECK (va2 > 0 && sql1 (a, "SELECT COUNT(*) FROM stack") == 1, "libxfer: a second version, stacked");

  /* Errors first. */
  p (dst, "Xfer");
  CHECK (libx_export (&x, a, dst, NULL, -1, 0, err, sizeof err) == -1 && strstr (err, "already"),
         "export: not over something already there");
  CHECK (libx_export (&x, a, dst, sel, 0, 0, err, sizeof err) == -1, "export: nothing chosen, nothing done");
  CHECK (libx_import_plan (&x, a, dst, &sv, err, sizeof err) == -1 && strstr (err, "isn't a Lumen library"),
         "import: a folder of photos isn't a library");
  CHECK (libx_import_plan (&x, a, lib_root (a), &sv, err, sizeof err) == -1 && strstr (err, "open"),
         "import: not the library itself");

  /* The whole library, with its originals (and the user's presets). */
  p (path, "XferPresets");
  presets_set_dir (path);
  presets_save ("warm", "wb temp=99");
  presets_save ("Cool", "wb temp=-10");
  p (dst, "ExportAll");
  CHECK (libx_export (&x, a, dst, NULL, -1, 1, err, sizeof err) == 0, "export: the whole library starts");
  CHECK (libx_started (x) && libx_is_export (x), "export: started");
  xfer_drain (x, NULL);
  libx_result (x, msg, sizeof msg, &failed);
  CHECK (!failed && strstr (msg, "Exported 3 photos to ExportAll, with their originals"), msg);
  libx_free (x);
  CHECK (file_there ("ExportAll/Catalogue") && file_there ("ExportAll/Originals/Trip/a.jpg")
         && file_there ("ExportAll/Originals/Trip/sub/c.jpg"), "export: originals copied, folders kept");
  CHECK (file_there ("Xfer/Trip/a.jpg"), "export: the originals themselves left alone");
  snprintf (path, sizeof path, "ExportAll/Thumbs/%02x/%lld", (unsigned) (vb & 0xff), vb);
  CHECK (file_there (path), "export: thumbnails copied, same ids");
  p (path, "ExportAll");
  CHECK (lib_open (&b, path, err, sizeof err) == 0, "export: the new library opens");
  CHECK (sql1 (b, "SELECT COUNT(*) FROM version") == 4 && sql1 (b, "SELECT COUNT(*) FROM original") == 3,
         "export: 4 versions of 3 photos");
  CHECK (cat_get_details (lib_cat (b), vc, &d) == 0 && strstr (d.path, "ExportAll/Originals/Trip/sub/c.jpg"),
         "export: paths point into the new library");
  CHECK (sql1 (b, "SELECT COUNT(*) FROM album WHERE smart IS NOT NULL") == 1
         && sql1 (b, "SELECT COUNT(*) FROM album_item ai JOIN album al ON al.id = ai.album WHERE al.name = 'Best'") == 3,
         "export: albums and smart albums (a's second version went into Best with it)");
  CHECK (sql1 (b, "SELECT COUNT(*) FROM album WHERE folder = 'sub'") == 1, "export: folder albums");
  CHECK (sql1 (b, "SELECT watch FROM project") == 1, "export: the whole library keeps watching");
  CHECK (sql1 (b, "SELECT COUNT(*) FROM version_keyword") == 4 && sql1 (b, "SELECT COUNT(*) FROM keyword") == 2,
         "export: keywords");
  CHECK (sql1 (b, "SELECT COUNT(*) FROM stack") == 1 && sql1 (b, "SELECT COUNT(*) FROM preset") == 2
         && sql1 (b, "SELECT COUNT(*) FROM preset WHERE name = 'Cool'") == 1,
         "export: stack and presets (the library's, then the user's of other names)");
  CHECK (sql1 (b, "SELECT label FROM version WHERE caption = 'The harbour wall'") == 2, "export: caption and label");
  lib_close (b);
  presets_set_dir ("");

  /* Moved: its originals are found in the new place. */
  {
    char from[1024], to[1024];
    p (from, "ExportAll");
    p (to, "Moved");
    rename (from, to);
    CHECK (lib_open (&b, to, err, sizeof err) == 0, "moved: opens");
    CHECK (cat_get_details (lib_cat (b), vc, &d) == 0 && strstr (d.path, "/Moved/Originals/Trip/sub/c.jpg")
           && !strstr (d.path, "ExportAll"), "moved: the originals inside it are found where it is now");
    CHECK (sql1 (b, "SELECT COUNT(*) FROM project WHERE root LIKE '%/Moved/Originals/Trip'") == 1, "moved: project folder too");
    lib_close (b);
  }

  /* A selection, without originals: only what goes with it. */
  p (dst, "ExportSome");
  sel[0] = va;
  sel[1] = vc;
  CHECK (libx_export (&x, a, dst, sel, 2, 0, err, sizeof err) == 0, "export: a selection");
  xfer_drain (x, NULL);
  libx_result (x, msg, sizeof msg, &failed);
  CHECK (!failed && strstr (msg, "Exported 2 photos"), msg);
  libx_free (x);
  CHECK (!file_there ("ExportSome/Originals"), "export: no originals copied when not asked");
  p (path, "ExportSome");
  CHECK (lib_open (&b, path, err, sizeof err) == 0, "export: the selection opens");
  CHECK (sql1 (b, "SELECT COUNT(*) FROM version") == 2, "export: just the two");
  CHECK (cat_get_details (lib_cat (b), va, &d) == 0 && strstr (d.path, "Xfer/Trip/a.jpg"), "export: originals referred to where they are");
  CHECK (sql1 (b, "SELECT COUNT(*) FROM stack") == 0 && sql1 (b, "SELECT COUNT(*) FROM version WHERE stack IS NOT NULL") == 0,
         "export: a stack with one version left isn't one");
  CHECK (sql1 (b, "SELECT COUNT(*) FROM album_item") >= 1 && sql1 (b, "SELECT COUNT(*) FROM album WHERE smart IS NOT NULL") == 0
         && sql1 (b, "SELECT COUNT(*) FROM album_item ai JOIN album al ON al.id = ai.album WHERE al.name = 'Best'") == 1,
         "export: Best with only a; no smart albums");
  CHECK (sql1 (b, "SELECT watch FROM project") == 0, "export: a part isn't watched");
  lib_close (b);

  /* Import the moved library into another one that has a copy of a. */
  p (path, "XferC");
  lib_close (a);
  CHECK (lib_open (&a, path, err, sizeof err) == 0, "import: library C");
  p (path, "Xfer/Other");
  lib_import (a, path);
  drain (a);
  p (path, "Moved");
  CHECK (libx_import_plan (&x, a, path, &sv, err, sizeof err) == 0, "import: planned");
  CHECK (sv.photos == 3 && sv.known == 1 && sv.versions == 4 && sv.new_versions == 3, "import: survey of photos and versions");
  CHECK (sv.projects == 1 && sv.new_projects == 1 && sv.albums == 2 && sv.new_albums == 2 && sv.missing == 0,
         "import: survey of projects and albums (folder albums stay)");
  CHECK (!strcmp (sv.name, "Moved") && sv.bytes > 0, "import: its name, and what there is to copy");
  CHECK (!libx_started (x), "import: nothing done before Import is clicked");
  libx_import_go (x, 1);
  xfer_drain (x, &changed);
  libx_result (x, msg, sizeof msg, &failed);
  CHECK (!failed && strstr (msg, "2 new photos and 1 more version") && strstr (msg, "1 was already here"), msg);
  CHECK (changed == 1, "import: the list changed once");
  libx_free (x);
  CHECK (file_there ("XferC/Originals/Trip/b.jpg") && file_there ("XferC/Originals/Trip/sub/c.jpg")
         && !file_there ("XferC/Originals/Trip/a.jpg"), "import: new photos copied in; the known one not");
  CHECK (sql1 (a, "SELECT COUNT(*) FROM version") == 5, "import: 2 + 3 versions");
  CHECK (sql1 (a, "SELECT COUNT(*) FROM version v JOIN original o ON o.id = v.original WHERE o.path LIKE '%copy-of-a.jpg'") == 2,
         "import: a's new version joins the copy already here");
  CHECK (sql1 (a, "SELECT COUNT(*) FROM version WHERE thumb = 1") == 5, "import: thumbnails came too");
  CHECK (sql1 (a, "SELECT COUNT(*) FROM original WHERE batch = (SELECT MAX(batch) FROM original)") == 2,
         "import: the new photos are the Last import");
  CHECK (sql1 (a, "SELECT watch FROM project WHERE name = 'Trip'") == 0, "import: watching stays behind");
  CHECK (sql1 (a, "SELECT COUNT(*) FROM album WHERE folder IS NOT NULL") == 0, "import: folder albums stay behind");
  CHECK (sql1 (a, "SELECT COUNT(*) FROM album_item ai JOIN album al ON al.id = ai.album WHERE al.name = 'Best'") == 3,
         "import: Best has a (the copy here), its new version and b");
  CHECK (sql1 (a, "SELECT COUNT(*) FROM album WHERE name = 'Good' AND smart IS NOT NULL") == 1, "import: smart album");
  CHECK (sql1 (a, "SELECT COUNT(*) FROM version_keyword vk JOIN keyword k ON k.id = vk.keyword WHERE k.name = 'coast'") == 3,
         "import: keywords, on the known photo too");
  CHECK (sql1 (a, "SELECT label FROM version WHERE caption = 'The harbour wall'") == 2, "import: caption and label");
  CHECK (sql1 (a, "SELECT COUNT(*) FROM preset WHERE name = 'Warm'") == 1, "import: preset");
  CHECK (sql1 (a, "SELECT COUNT(*) FROM version WHERE recipe = 'exposure ev=0.5'") == 1, "import: adjustments kept");

  /* Again: nothing new. */
  p (path, "Moved");
  CHECK (libx_import_plan (&x, a, path, &sv, err, sizeof err) == 0 && sv.known == 3 && sv.new_versions == 0
         && sv.new_albums == 0 && sv.new_projects == 0, "import again: everything is already here");
  libx_import_go (x, 0);
  xfer_drain (x, NULL);
  CHECK (sql1 (a, "SELECT COUNT(*) FROM version") == 5 && sql1 (a, "SELECT COUNT(*) FROM album") == 2,
         "import again: nothing added twice");
  libx_free (x);

  /* Without copying: the new photos are referred to where they are. */
  p (path, "ExportSome");
  CHECK (libx_import_plan (&x, a, path, &sv, err, sizeof err) == 0 && sv.known == 2 && sv.new_versions == 0,
         "import: a and c from the selection are known");
  libx_free (x);
  lib_close (a);
}

/* --- checking and repairing a library (libcheck) */

static int run_check (library *l, libcheck_report *out, int *steps)
{
  libcheck *k = libcheck_start (l);
  int n = 0;
  if (!k)
    return -1;
  while (!libcheck_step (k, frozen_now, 0) && n < 100000)
    n++;
  *out = *libcheck_result (k);
  if (steps)
    *steps = n;
  libcheck_free (k);
  return 0;
}

static void test_libcheck (void)
{
  library *l;
  libcheck *k;
  libcheck_report r;
  char err[400], path[1024], text[800], fixed[400];
  int steps;
  cat_id v1;

  p (path, "CheckLib");
  CHECK (lib_open (&l, path, err, sizeof err) == 0, "check: library");
  p (path, "Xfer/Trip");
  lib_import (l, path);
  drain (l);
  CHECK (run_check (l, &r, &steps) == 0 && !libcheck_problems (&r) && r.versions == 3, "check: a good library has no problems");
  CHECK (steps > 256, "check: done in small steps");
  libcheck_describe (&r, text, sizeof text);
  CHECK (strstr (text, "All is well: 3 photos and 3 versions checked") != NULL, text);

  /* Break it in every way it knows of. */
  v1 = version_named (l, "a.jpg");
  sqlite3_exec (cat_sqlite (lib_cat (l)),
                "PRAGMA foreign_keys=OFF;"
                "INSERT INTO project (name, created) VALUES ('Doomed', 0);"
                "UPDATE version SET project = (SELECT id FROM project WHERE name = 'Doomed') WHERE name = 'b.jpg';"
                "DELETE FROM project WHERE name = 'Doomed';"
                "INSERT INTO original (path, size, hash) VALUES ('/nowhere/lone.jpg', 5, 5);"
                "INSERT INTO version (original, project, name) VALUES (99999, 1, 'ghost.jpg');"
                "INSERT INTO album (name, created) VALUES ('A', 0);"
                "INSERT INTO album_item VALUES ((SELECT id FROM album WHERE name = 'A'), 88888, 1);"
                "INSERT INTO keyword (name) VALUES ('k');"
                "INSERT INTO version_keyword VALUES (88888, (SELECT id FROM keyword WHERE name = 'k'));"
                "INSERT INTO stack (pick, open) VALUES (1, 0);"
                "UPDATE version SET stack = (SELECT MAX(id) FROM stack) WHERE name = 'a.jpg';"
                "UPDATE version SET thumb = 2 WHERE name = 'c.jpg';"
                "PRAGMA foreign_keys=ON;", NULL, NULL, NULL);
  lib_thumb_path (l, v1, path, sizeof path);
  remove (path);
  {
    char spare[1024];
    FILE *f;
    p (spare, "CheckLib/Thumbs/0f/77777");
    p (path, "CheckLib/Thumbs/0f");
    fsys_mkdirs (path);
    f = fopen (spare, "wb");
    if (f)
      {
        fputs ("old thumbnail", f);
        fclose (f);
      }
  }
  CHECK (run_check (l, &r, NULL) == 0, "check: runs on a broken library");
  CHECK (!r.damaged && r.versions_no_photo == 1 && r.versions_no_project == 1 && r.lone_originals == 1,
         "check: a version without its photo, one without its project, a photo without a version");
  CHECK (r.bad_album_items == 1 && r.bad_keyword_links == 1 && r.bad_stacks == 1, "check: album entry, keyword, stack of one");
  CHECK (r.thumbs_missing == 1 && r.thumbs_failed == 1 && r.thumbs_unused == 1 && r.thumbs_unused_bytes == 13,
         "check: thumbnail missing, failed, and one no photo uses");
  libcheck_describe (&r, text, sizeof text);
  CHECK (strstr (text, "1 version whose project has gone") && strstr (text, "1 thumbnail file no photo uses"), text);

  /* Repair */
  k = libcheck_start (l);
  CHECK (libcheck_repair (k, fixed, sizeof fixed, err, sizeof err) == -1, "repair: not before the check has finished");
  while (!libcheck_step (k, frozen_now, 0))
    ;
  CHECK (libcheck_repair (k, fixed, sizeof fixed, err, sizeof err) == 0, err);
  CHECK (strstr (fixed, "Recovered") && strstr (fixed, "Catalogue-backup"), fixed);
  libcheck_free (k);
  CHECK (file_there ("CheckLib/Catalogue-backup") && !file_there ("CheckLib/Catalogue-backup-new"),
         "repair: the catalogue kept first");
  CHECK (!file_there ("CheckLib/Thumbs/0f/77777"), "repair: the unused thumbnail file went");
  CHECK (sql1 (l, "SELECT COUNT(*) FROM version v JOIN project p ON p.id = v.project WHERE p.name = 'Recovered'") == 2,
         "repair: b and the lone photo are in Recovered");
  CHECK (sql1 (l, "SELECT COUNT(*) FROM version WHERE thumb = 0") >= 3, "repair: thumbnails to be made again");
  drain (l);
  CHECK (run_check (l, &r, NULL) == 0 && libcheck_problems (&r) == 0, "repair: checked again, all is well");
  CHECK (r.versions == 4, "repair: 3 photos and the lone one; the ghost version gone");
  {
    /* The kept copy is the catalogue as it was (still broken). */
    library *b;
    char dir[1024], from[1024], to[1024];
    p (dir, "CheckBackup");
    fsys_mkdirs (dir);
    p (from, "CheckLib/Catalogue-backup");
    p (to, "CheckBackup/Catalogue");
    fsys_copy_tree (from, to);
    CHECK (lib_open (&b, dir, err, sizeof err) == 0 && sql1 (b, "SELECT COUNT(*) FROM original WHERE id NOT IN (SELECT original FROM version)") == 1, "repair: the backup is the catalogue before");
    lib_close (b);
  }
  lib_close (l);
}

/* Regressions found in review: a plan gone stale, a library moved inside
   its old place, project names that make the same leaf. */
static void test_libxfer_more (void)
{
  library *a, *b;
  libxfer *x;
  libx_survey sv;
  char err[300], path[1024], msg[400], from[1024], to[1024];
  cat_id vp;
  int failed;
  p (path, "Stale/P"); fsys_mkdirs (path);
  p (path, "Stale/Q"); fsys_mkdirs (path);
  p (path, "Stale/P/p.jpg"); make_jpeg (path, 70, 50, 1, "2024:01:01 10:00:00");
  p (path, "Stale/Q/q.jpg"); make_jpeg (path, 72, 52, 1, "2024:02:01 10:00:00");
  p (path, "StaleA");
  lib_open (&a, path, err, sizeof err);
  p (path, "Stale/P"); lib_import (a, path); drain (a);
  vp = version_named (a, "p.jpg");
  cat_add_keywords (lib_cat (a), &vp, 1, "secret");
  p (path, "StaleB");
  CHECK (libx_export (&x, a, path, NULL, -1, 0, err, sizeof err) == 0, "stale: export");
  xfer_drain (x, NULL);
  libx_free (x);
  CHECK (libx_import_plan (&x, a, path, &sv, err, sizeof err) == 0 && sv.known == 1, "stale: the plan sees p here");
  /* Meanwhile p is removed and q imported (it may take p's old id). */
  lib_remove (a, &vp, 1);
  p (path, "Stale/Q"); lib_import (a, path); drain (a);
  libx_import_go (x, 0);
  xfer_drain (x, NULL);
  libx_result (x, msg, sizeof msg, &failed);
  CHECK (!failed && strstr (msg, "1 new photo"), msg);
  libx_free (x);
  CHECK (sql1 (a, "SELECT COUNT(*) FROM version WHERE name = 'p.jpg'") == 1, "stale: p comes back");
  CHECK (sql1 (a, "SELECT COUNT(*) FROM version_keyword vk JOIN version v ON v.id = vk.version"
                  " WHERE v.name = 'q.jpg'") == 0, "stale: q doesn't get p's keyword");
  lib_close (a);

  /* A library moved inside the place it was: paths moved once, not again and again. */
  p (from, "Moved");
  p (to, "MovedTmp");
  rename (from, to);
  fsys_mkdirs (from);
  p (from, "Moved/Zed");
  rename (to, from);
  CHECK (lib_open (&b, from, err, sizeof err) == 0, "nested move: opens");
  CHECK (sql1 (b, "SELECT COUNT(*) FROM original WHERE path LIKE '%/Moved/Zed/Originals/Trip/%'") == 3
         && sql1 (b, "SELECT COUNT(*) FROM original WHERE path LIKE '%Zed/Zed%'") == 0,
         "nested move: each path moved once");
  lib_close (b);

  /* Two projects whose names make the same leaf get a directory each. */
  p (path, "Leaf/My Trip"); fsys_mkdirs (path);
  p (path, "Leaf/My_Trip"); fsys_mkdirs (path);
  p (path, "Leaf/My Trip/one.jpg"); make_jpeg (path, 74, 50, 1, NULL);
  p (path, "Leaf/My_Trip/two.jpg"); make_jpeg (path, 76, 50, 1, NULL);
  p (path, "LeafLib");
  lib_open (&a, path, err, sizeof err);
  p (path, "Leaf/My Trip"); lib_import (a, path);
  p (path, "Leaf/My_Trip"); lib_import (a, path);
  drain (a);
  p (path, "LeafOut");
  CHECK (libx_export (&x, a, path, NULL, -1, 1, err, sizeof err) == 0, "leaves: export");
  xfer_drain (x, NULL);
  libx_free (x);
  CHECK (file_there ("LeafOut/Originals/My_Trip/one.jpg") && file_there ("LeafOut/Originals/My_Trip_2/two.jpg"),
         "leaves: My Trip and My_Trip in directories of their own");
  lib_close (a);
}

/* thumbgen: the box filter (now with its columns worked out once) gives
   exactly what the per-pixel division gave, and decoding at any eighth. */
static int open_at (const char *path, int max_side, rgb_image *img)
{
  char err[200];
  thumbgen *t = thumbgen_start (path, NULL, max_side, 1, err, sizeof err);
  int r;
  if (!t)
    return -1;
  while ((r = thumbgen_step (t, 37)) == 0)
    ;
  r = r < 0 || thumbgen_take (t, img) ? -1 : 0;
  thumbgen_free (t);
  return r;
}

static void test_jpeg_scaling (void)
{
  char path[1024], err[200];
  rgb_image src, full, small, e8;
  int x, y, k, same = 1, close = 1;
  p (path, "scale.jpg");
  src.w = 640; src.h = 427;
  src.rgb = malloc ((size_t) src.w * src.h * 3);
  for (y = 0; y < src.h; y++)
    for (x = 0; x < src.w; x++)
      {
        unsigned char *q = src.rgb + ((size_t) y * src.w + x) * 3;
        q[0] = (unsigned char) (x * 255 / src.w);
        q[1] = (unsigned char) (y * 255 / src.h);
        q[2] = (unsigned char) (((x / 7) ^ (y / 5)) * 9);
      }
  CHECK (img_save_jpeg (&src, path, 92, err, sizeof err) == 0, "scaling: test JPEG written");
  free (src.rgb);

  /* The scale chosen: halvings only unless eighths are allowed. */
  thumbgen_eighths (0);
  CHECK (thumbgen_jpeg_eighths (6000, 1600) == 4, "scaling: 6000 at 1600 decodes at 1/2");
  CHECK (thumbgen_jpeg_eighths (6000, 256) == 1, "scaling: 6000 at 256 decodes at 1/8");
  CHECK (thumbgen_jpeg_eighths (6000, 1 << 30) == 8, "scaling: full size at 8/8");
  CHECK (thumbgen_jpeg_eighths (4000, 1100) == 4, "scaling: 4000 at 1100 decodes at 1/2");
  thumbgen_eighths (1);
  CHECK (thumbgen_jpeg_eighths (6000, 1600) == 3, "scaling: eighths: 6000 at 1600 decodes at 3/8");
  CHECK (thumbgen_jpeg_eighths (6000, 2251) == 4, "scaling: eighths: just over 3/8 takes 4/8");
  CHECK (thumbgen_jpeg_eighths (4000, 1100) == 3, "scaling: eighths: 4000 at 1100 decodes at 3/8");
  CHECK (thumbgen_jpeg_eighths (6000, 4600) == 7, "scaling: eighths: 7/8");
  thumbgen_eighths (0);

  /* At 400 the JPEG is decoded whole (640 is the first halving >= 400),
     then box-filtered: the result must match the filter worked out here. */
  CHECK (open_at (path, 1 << 30, &full) == 0 && full.w == 640, "scaling: decoded whole");
  CHECK (open_at (path, 400, &small) == 0 && small.w == 400 && small.h == 267, "scaling: 400 wide");
  if (full.rgb && small.rgb && small.w == 400)
    {
      unsigned *sum = calloc ((size_t) small.w * small.h * 3, sizeof *sum), *cnt = calloc ((size_t) small.w * small.h, sizeof *cnt);
      for (y = 0; y < full.h; y++)
        for (x = 0; x < full.w; x++)
          {
            int ty = (int) ((long long) y * small.h / full.h), tx = (int) ((long long) x * small.w / full.w);
            for (k = 0; k < 3; k++)
              sum[((size_t) ty * small.w + tx) * 3 + k] += full.rgb[((size_t) y * full.w + x) * 3 + k];
            cnt[(size_t) ty * small.w + tx]++;
          }
      for (k = 0; k < small.w * small.h * 3; k++)
        {
          unsigned c = cnt[k / 3] ? cnt[k / 3] : 1;
          if (small.rgb[k] != (unsigned char) ((sum[k] + c / 2) / c))
            same = 0;
        }
      free (sum);
      free (cnt);
    }
  CHECK (same, "scaling: the box filter gives exactly the reference pixels");

  /* At 240 with eighths: decoded at 3/8 (240 x 161), no box filter needed;
     the halvings decode at 1/2 (320) and filter down. Close, not equal. */
  thumbgen_eighths (1);
  CHECK (open_at (path, 240, &e8) == 0 && e8.w == 240 && e8.h == 161, "scaling: eighths: 240 wide");
  thumbgen_eighths (0);
  free (small.rgb);
  small.rgb = NULL;
  CHECK (open_at (path, 240, &small) == 0 && small.w == 240 && small.h == 161, "scaling: halvings: 240 wide");
  if (e8.rgb && small.rgb && e8.w == small.w && e8.h == small.h)
    {
      long long diff = 0;
      for (k = 0; k < small.w * small.h * 3; k++)
        diff += abs (e8.rgb[k] - small.rgb[k]);
      close = diff / (small.w * small.h * 3) <= 6;
    }
  CHECK (close, "scaling: 3/8 decode looks like the 1/2 decode filtered down");
  thumbgen_eighths (-1);
  free (full.rgb);
  free (small.rgb);
  free (e8.rgb);
}

/* Previews: made in the background once thumbnails are done, found by
   their key, made again when out of date, discarded. */
static int preview_count (const char *libdir)
{
  char top[1100], dir[1100], shard[8];
  int k, n = 0;
  fsys_join (top, sizeof top, libdir, "Previews");
  for (k = 0; k < 256; k++)
    {
      fsys_dir *d;
      fsys_entry e;
      snprintf (shard, sizeof shard, "%02x", k);
      fsys_join (dir, sizeof dir, top, shard);
      if (!(d = fsys_opendir (dir)))
        continue;
      while (fsys_readdir (d, &e) == 1)
        n += !e.is_dir;
      fsys_closedir (d);
    }
  return n;
}

static char last_started[2400];
static int started_only (const char *cmd, int tag)
{
  (void) tag;                          /* still running: never ends */
  snprintf (last_started, sizeof last_started, "%s", cmd);
  return 0;
}

static void preview_drain (library *l)
{
  int guard = 0;
  clock_cs = 0;
  while (lib_previews_busy (l) && guard++ < 100000)
    lib_preview_work (l, fake_now, clock_cs + 3);
}

static void test_previews (void)
{
  library *l;
  char err[256], dir[1024], path[1024], libdir[1024], pv[1300], pv2[1300];
  cat_origin o;
  cat_id first = 0, second = 0;
  rgb_image img;
  long long bytes = 0;
  int n;
  p (dir, "PreviewPhotos");
  fsys_mkdirs (dir);
  p (path, "PreviewPhotos/wide.jpg");
  make_jpeg (path, 3000, 2000, 1, "2026:01:01 10:00:00");
  p (path, "PreviewPhotos/turned.jpg");
  make_jpeg (path, 2400, 1600, 6, "2026:01:01 10:01:00");   /* shown portrait */
  p (path, "PreviewPhotos/small.jpg");
  make_jpeg (path, 800, 600, 1, "2026:01:01 10:02:00");
  p (libdir, "PreviewLibrary");
  CHECK (lib_open (&l, libdir, err, sizeof err) == 0, "previews: library");
  CHECK (lib_previews (l) == PREVIEWS_OFF && !lib_previews_busy (l), "previews: off until set");
  lib_import (l, dir);
  drain (l);
  lib_set_previews (l, PREVIEWS_RAW);
  CHECK (!lib_preview_wanted (l, IMG_JPEG, "x.jpg") && lib_previews_busy (l), "previews: raw only: not for JPEGs, a look through");
  preview_drain (l);
  CHECK (preview_count (libdir) == 0, "previews: raw only: none made for JPEGs");

  lib_set_previews (l, PREVIEWS_ALL);
  CHECK (lib_preview_wanted (l, IMG_JPEG, "x.jpg") && lib_preview_wanted (l, IMG_PNG, "x.png"), "previews: all: JPEGs and PNGs");
  lib_hold_previews (l, 1);
  CHECK (!lib_previews_busy (l), "previews: held (the Viewer busy)");
  lib_hold_previews (l, 0);
  preview_drain (l);
  CHECK (!lib_previews_busy (l) && preview_count (libdir) == 3, "previews: one for each photo");
  if (cat_origin_below (lib_cat (l), 0, &o) == 1)
    {
      second = o.id;
      if (cat_origin_below (lib_cat (l), o.id, &o) == 1)
        first = o.id;
    }
  CHECK (first > 0 && second > first, "previews: walked newest first");
  {
    cat_id id;
    int seen = 0, ok = 1;
    for (id = 0; cat_origin_below (lib_cat (l), id, &o) == 1; id = o.id)
      {
        thumbgen *t;
        int r, w = 0, h = 0, big = o.size > 0;
        seen++;
        if (!lib_preview_find (l, o.id, pv, sizeof pv))
          {
            ok = 0;
            continue;
          }
        t = thumbgen_start (pv, NULL, 1 << 30, 1, err, sizeof err);
        while (t && (r = thumbgen_step (t, 64)) == 0)
          ;
        if (t)
          thumbgen_size (t, &w, &h);
        thumbgen_free (t);
        (void) big;
        if (strstr (o.path, "wide"))
          ok &= w == 1600 && h == 1067;
        else if (strstr (o.path, "turned"))
          ok &= w == 1067 && h == 1600;   /* upright, as the Viewer shows it */
        else
          ok &= w == 800 && h == 600;     /* smaller than 1600: as it is */
      }
    CHECK (seen == 3 && ok, "previews: 1600 at most, upright");
  }
  /* Out of date (the file changed): not found, made again, the old one gone. */
  CHECK (lib_preview_find (l, first, pv, sizeof pv) == 1, "previews: found");
  sqlite3_exec (cat_sqlite (lib_cat (l)), "UPDATE original SET mtime = mtime + 100", NULL, NULL, NULL);
  CHECK (lib_preview_find (l, first, pv2, sizeof pv2) == 0 && strcmp (pv, pv2) != 0, "previews: out of date: not found");
  lib_previews_changed (l);
  preview_drain (l);
  CHECK (lib_preview_find (l, first, pv2, sizeof pv2) == 1 && preview_count (libdir) == 3,
         "previews: made again, the old ones removed");
  {
    char leaf[40];
    CHECK (lib_preview_leaf (l, first, leaf, sizeof leaf) && strstr (pv2, leaf), "previews: the leaf Check library expects");
  }
  /* The Viewer opening the photo whose preview is being made takes it over. */
  {
    thumbgen *t;
    int r = 0, w = 0, h = 0;
    char leaf[64];
    sqlite3_exec (cat_sqlite (lib_cat (l)), "UPDATE original SET mtime = mtime + 100", NULL, NULL, NULL);
    lib_previews_changed (l);
    clock_cs = 0;
    lib_preview_work (l, fake_now, clock_cs + 1);   /* one started */
    CHECK (lib_preview_now (l, leaf, sizeof leaf), "previews: one being made");
    CHECK (lib_preview_adopt (l, 999999) == NULL, "previews: not handed over for another photo");
    if (cat_origin_below (lib_cat (l), 0, &o) == 1)
      {
        t = lib_preview_adopt (l, o.id);
        CHECK (t != NULL && !lib_preview_now (l, leaf, sizeof leaf), "previews: handed over to the Viewer");
        while (t && (r = thumbgen_step (t, 64)) == 0)
          ;
        if (t)
          thumbgen_size (t, &w, &h);
        CHECK (r == 1 && (w == 1600 || h == 1600 || w == 800), "previews: and finished there");
        if (r == 1 && thumbgen_take (t, &img) == 0)
          {
            CHECK (lib_preview_save (l, o.id, &img) == 1, "previews: kept by the Viewer");   /* as proxy_ready does */
            free (img.rgb);
          }
        thumbgen_free (t);
      }
    preview_drain (l);
  }
  /* Check library: a preview of no photo, and one out of date. */
  {
    char stray[1300], d2[1100], top[1100];
    libcheck *k;
    const libcheck_report *r;
    char fixed[300], e2[200], said[600];
    FILE *f;
    fsys_join (top, sizeof top, libdir, "Previews");
    fsys_join (d2, sizeof d2, top, "07");
    fsys_mkdirs (d2);
    fsys_join (stray, sizeof stray, d2, "999991_0000beef");
    f = fopen (stray, "wb");
    if (f)
      {
        fputs ("not a preview", f);
        fclose (f);
      }
    k = libcheck_start (l);
    clock_cs = 0;
    while (!libcheck_step (k, fake_now, clock_cs + 3))
      ;
    r = libcheck_result (k);
    libcheck_describe (r, said, sizeof said);
    CHECK (r->previews == 4 && r->previews_unused == 1 && strstr (said, "1 preview out of date or of no photo"),
           "previews: Check library finds the one of no photo");
    CHECK (libcheck_repair (k, fixed, sizeof fixed, e2, sizeof e2) == 0 && preview_count (libdir) == 3
           && lib_preview_find (l, first, pv, sizeof pv) == 1, "previews: repair removes it, the others kept");
    libcheck_free (k);
  }
  /* The Viewer keeps what it opened: only when wanted. */
  img.w = 4; img.h = 3;
  img.rgb = calloc (4 * 3, 3);
  lib_set_previews (l, PREVIEWS_RAW);
  CHECK (lib_preview_save (l, first, &img) == 0, "previews: the Viewer's picture not kept when not wanted");
  lib_set_previews (l, PREVIEWS_ALL);
  CHECK (lib_preview_save (l, first, &img) == 1 && lib_preview_find (l, first, pv, sizeof pv) == 1
         && preview_count (libdir) == 3, "previews: the Viewer's picture kept, in place of the old");
  /* Developed with the raw's own noise reduction: kept beside the plain one. */
  CHECK (lib_preview_find_noise (l, first, 1, pv2, sizeof pv2) == 0, "previews: none with noise reduction yet");
  CHECK (lib_preview_save_noise (l, first, 1, &img) == 1 && lib_preview_find_noise (l, first, 1, pv2, sizeof pv2) == 1
         && lib_preview_find (l, first, pv, sizeof pv) == 1 && strcmp (pv, pv2) && preview_count (libdir) == 4
         && lib_preview_find_noise (l, first, 2, pv2, sizeof pv2) == 0, "previews: one with Light noise reduction, beside it");
  CHECK (lib_preview_save (l, first, &img) == 1 && lib_preview_find_noise (l, first, 1, pv2, sizeof pv2) == 1,
         "previews: the plain one made again keeps it");
  {
    libcheck *k = libcheck_start (l);
    clock_cs = 0;
    while (!libcheck_step (k, fake_now, clock_cs + 3))
      ;
    CHECK (libcheck_result (k)->previews == 4 && libcheck_result (k)->previews_unused == 0,
           "previews: Check library knows it for one in use");
    libcheck_free (k);
  }
  {
    char sql[120];
    snprintf (sql, sizeof sql, "UPDATE original SET mtime = mtime + 100 WHERE id = %lld", (long long) first);
    sqlite3_exec (cat_sqlite (lib_cat (l)), sql, NULL, NULL, NULL);
  }
  CHECK (lib_preview_save (l, first, &img) == 1 && lib_preview_find_noise (l, first, 1, pv2, sizeof pv2) == 0
         && preview_count (libdir) == 3, "previews: out of date: the noise-reduced one goes with the old");
  free (img.rgb);
  /* A photo removed from the library: its preview goes too. */
  {
    cat_source all = { SRC_ALL, 0, 0, "" };
    cat_item *items = NULL;
    cat_details dd;
    int ni = 0;
    cat_list (lib_cat (l), &all, &items, &ni);
    CHECK (ni == 3 && cat_get_details (lib_cat (l), items[0].id, &dd) == 0 && lib_preview_find (l, dd.original, pv, sizeof pv),
           "previews: the one to remove has one");
    if (ni == 3)
      CHECK (lib_remove (l, &items[0].id, 1) == 0 && !lib_preview_find (l, dd.original, pv, sizeof pv)
             && preview_count (libdir) == 2, "previews: removed with its photo");
    free (items);
  }
  n = lib_discard_previews (l, &bytes);
  CHECK (n == 2 && bytes > 0 && preview_count (libdir) == 0 && !lib_preview_find (l, first, pv, sizeof pv),
         "previews: discarded");
  lib_set_previews (l, PREVIEWS_OFF);
  CHECK (!lib_previews_busy (l), "previews: off: nothing made again");
  lib_close (l);

  /* A raw file: developed by the plug-in for its preview. */
  {
    const char *helper = getenv ("LUMENRAW");
    char nef[700], scratch[1024];
    if (helper && sample (nef, sizeof nef, "iss030e122639.NEF"))
      {
        cat_id id = 0;
        thumbgen *t;
        int r, w = 0, h = 0;
        p (scratch, "RawScratch");
        rawjob_setup (helper, scratch, run_now);
        p (libdir, "PreviewRawLibrary");
        CHECK (lib_open (&l, libdir, err, sizeof err) == 0, "raw previews: library");
        lib_import (l, nef);
        drain (l);
        lib_set_previews (l, PREVIEWS_RAW);
        CHECK (lib_preview_wanted (l, IMG_RAW, "x.nef"), "raw previews: wanted");
        preview_drain (l);
        if (cat_origin_below (lib_cat (l), 0, &o) == 1)
          id = o.id;
        CHECK (lib_preview_find (l, id, pv, sizeof pv) == 1, "raw previews: made");
        t = thumbgen_start (pv, NULL, 1 << 30, 1, err, sizeof err);
        while (t && (r = thumbgen_step (t, 64)) == 0)
          ;
        if (t)
          thumbgen_size (t, &w, &h);
        thumbgen_free (t);
        CHECK (w == 1600 && h == 1062, "raw previews: 1600 wide, like the Viewer's");
        /* Another plug-in (a new LumenRaw): out of date. */
        rawjob_setup (NULL, NULL, NULL);
        CHECK (!lib_preview_wanted (l, IMG_RAW, "x.nef") && lib_preview_find (l, id, pv2, sizeof pv2) == 0,
               "raw previews: no plug-in: another key, none wanted");
        rawjob_setup (helper, scratch, run_now);
        CHECK (lib_preview_find (l, id, pv2, sizeof pv2) == 1, "raw previews: the same plug-in again: found");
        lib_close (l);
        /* The photo being opened goes to the front of the plug-in's queue. */
        {
          char e3[200];
          rawjob *a, *b2, *c2;
          rawjob_setup (helper, scratch, started_only);
          a = rawjob_start ("first.nef", 1, 1600, e3, sizeof e3);
          b2 = rawjob_start ("second.nef", 1, 1600, e3, sizeof e3);
          c2 = rawjob_start ("opened.nef", 1, 1600, e3, sizeof e3);
          CHECK (strstr (last_started, "first.nef") != NULL, "plug-in queue: the first runs");
          rawjob_hurry (c2);
          rawjob_free (a);
          CHECK (strstr (last_started, "opened.nef") != NULL, "plug-in queue: the one hurried runs next");
          rawjob_free (c2);
          rawjob_free (b2);
          rawjob_setup (helper, scratch, run_now);
        }
      }
  }
}

/* A format plug-in: LumenHEIF. Lumen knows nothing of HEIF itself; its
   Plugin file says which files are its, and the plug-in gives the rest
   (LUMENHEIF, with the photos in tests/heif): imported, the EXIF where
   it says, previews made. */
static void test_heif (void)
{
  const char *helper = getenv ("LUMENHEIF");
  char path[1024], libdir[1024], dir[1024], err[256], scratch[1024], pv[1300];
  unsigned char head[256 * 1024];
  FILE *f;
  size_t got;
  library *l;
  thumbgen *t;
  plugin_info pi;
  int r, kind;
  snprintf (dir, sizeof dir, "%s", getenv ("HEIF_SAMPLES") ? getenv ("HEIF_SAMPLES") : "tests/heif");
  snprintf (path, sizeof path, "%s/plain.heic", dir);
  f = fopen (path, "rb");
  if (!f)
    {
      printf ("test_core: no tests/heif: HEIF not tested\n");
      return;
    }
  got = fread (head, 1, sizeof head, f);
  fclose (f);
  CHECK (img_probe_named (head, got, "x.heic") == IMG_UNKNOWN && rawjob_kind_for (IMG_UNKNOWN, path, head, got) < 0,
         "format plug-ins: Lumen itself knows nothing of HEIF");
  CHECK (img_is_plugin_format (IMG_PLUGIN) && img_is_plugin_format (IMG_RAW) && !img_is_plugin_format (IMG_JPEG),
         "format plug-ins: read by a plug-in, like raws");
  /* No plug-in: not imported. */
  p (libdir, "HeifNone");
  CHECK (lib_open (&l, libdir, err, sizeof err) == 0, "format plug-ins: a library");
  lib_import (l, dir);
  drain (l);
  {
    cat_source all = { SRC_ALL, 0, 0, "" };
    cat_item *items = NULL;
    int n = -1;
    cat_list (lib_cat (l), &all, &items, &n);
    CHECK (n == 0, "format plug-ins: with none for HEIF, HEIF photos aren't imported");
    free (items);
  }
  lib_close (l);

  /* LumenHEIF's own Plugin file says what it reads. */
  CHECK (plugins_read ("plugins/heif", &pi) == 0 && !strcmp (pi.kind, "format") && pi.api == 1
         && pi.nformats == 2 && !strcmp (pi.formats[0].name, "HEIF photo") && pi.formats[0].nsigs == 8
         && !strcmp (pi.formats[1].name, "AVIF photo") && pi.formats[1].nsigs == 2 && !strcmp (pi.formats[1].extensions, "avif")
         && !strcmp (pi.package, "LumenHEIF"),
         "format plug-ins: LumenHEIF's Plugin file (Kind, API, two Formats with their Extensions and Signatures)");
  CHECK (plugins_claims_name (&pi, "SDFS::Pi.$.IMG_0001/HEIC") && plugins_claims_name (&pi, "x.heif")
         && plugins_claims_name (&pi, "a/b.hif") && plugins_claims_name (&pi, "Photos.Lake/AVIF") && !plugins_claims_name (&pi, "x.heics")
         && !plugins_claims_name (&pi, "x.jpg"),
         "format plug-ins: its extensions, RISC OS and POSIX, any case");
  {
    unsigned char avif[32] = { 0, 0, 0, 0x1C, 'f', 't', 'y', 'p', 'a', 'v', 'i', 'f', 0, 0, 0, 0,
                               'm', 'i', 'f', '1', 'a', 'v', 'i', 'f', 'm', 'i', 'a', 'f', 0, 0, 0, 0 };
    unsigned char mp4[32] = { 0, 0, 0, 0x1C, 'f', 't', 'y', 'p', 'i', 's', 'o', 'm', 0, 0, 0, 0,
                              'i', 's', 'o', 'm', 'a', 'v', 'c', '1', 'm', 'p', '4', '1', 0, 0, 0, 0 };
    CHECK (plugins_claims (&pi, "photo", head, got) == 1 && plugins_claims (&pi, "photo", avif, sizeof avif) == 2
           && plugins_claims_name (&pi, "x.HEIC") == 1 && plugins_claims_name (&pi, "x/avif") == 2
           && plugins_claims (&pi, "x.avif", head, got) == 2,
           "format plug-ins: which of its formats a file is in (its name first, then its bytes)");
    CHECK (plugins_claims (&pi, "photo", head, got) && plugins_claims (&pi, "photo", avif, sizeof avif)
           && !plugins_claims (&pi, "photo", mp4, sizeof mp4) && !plugins_claims (&pi, "photo", head, 8),
           "format plug-ins: its signatures (HEIC and AVIF, not an MP4 film), whatever the name");
  }
  /* Its MIME types: claimed by those, and by any extension MimeMap gives one of them. */
  CHECK (!strcmp (pi.formats[0].mime, "image/heic image/heif") && !strcmp (pi.formats[1].mime, "image/avif")
         && plugins_claims_mime (&pi, "image/avif") == 2 && plugins_claims_mime (&pi, "image/HEIF") == 1
         && !plugins_claims_mime (&pi, "image/jpeg"), "format plug-ins: MIME lines, a format's own");
  fsys_mimemap_add ("image/heif", 0xA3C, "HEIF", "hif2 heifs");
  CHECK (plugins_claims_name (&pi, "Pics.Lake/hif2") == 1 && plugins_claims_name (&pi, "x.heifs") == 1
         && !plugins_claims_name (&pi, "x.tph"), "format plug-ins: an extension MimeMap says is image/heif");
  {
    plugin_info q;
    memset (&q, 0, sizeof q);
    snprintf (q.kind, sizeof q.kind, "format");
    q.nformats = 1;
    snprintf (q.formats[0].extensions, sizeof q.formats[0].extensions, "jxl");
    CHECK (!plugins_claims (&q, "x.heic", head, got) && plugins_claims_name (&q, "x/JXL"),
           "format plug-ins: each claims only its own");
  }
  kind = rawjob_set_format (&pi);
  CHECK (kind > 0 && rawjob_set_format (&pi) == kind && rawjob_format (kind) && rawjob_claims_name ("a.HEIC")
         && rawjob_kind_for (IMG_UNKNOWN, path, NULL, 0) == kind && rawjob_kind_for (IMG_PLUGIN, "photo", head, got) == kind
         && rawjob_kind_for (IMG_RAW, path, NULL, 0) == RAWJOB_RAW, "format plug-ins: a kind of its own, kept");

  /* Installed, but no program to run: says so, in its own words. */
  rawjob_setup_kind (kind, NULL, NULL, NULL);
  t = thumbgen_start (path, NULL, 256, 1, err, sizeof err);
  CHECK (!t && strstr (err, "This is a HEIF photo") && strstr (err, "LumenHEIF"), "format plug-ins: can't be run: install LumenHEIF again");
  rawjob_set_switched_off_kind (kind, 1);
  CHECK (strstr (rawjob_missing_message_kind (kind), "switched off")
         && strstr (rawjob_missing_message_kind (kind), "the LumenHEIF plug-in"), "format plug-ins: switched off: says so (by its package's name)");
  rawjob_set_switched_off_kind (kind, 0);
  CHECK (strstr (rawjob_missing_message_kind (-1), "no plug-in"), "format plug-ins: a photo none claims now");
  {
    char avpath[1100];
    snprintf (avpath, sizeof avpath, "%s/plain.avif", dir);
    CHECK (!strcmp (rawjob_format_name (kind, avpath, NULL, 0), "AVIF photo")
           && !strcmp (rawjob_format_name (kind, path, NULL, 0), "HEIF photo")
           && strstr (rawjob_missing_message_for (kind, avpath), "This is an AVIF photo")
           && strstr (rawjob_missing_message_for (kind, path), "This is a HEIF photo"),
           "format plug-ins: each file named by its own format (a HEIF photo, an AVIF photo)");
  }
  if (!helper)
    {
      printf ("test_core: no LUMENHEIF: HEIF photos not imported\n");
      rawjob_clear_format (kind);
      return;
    }
  p (scratch, "HeifScratch");
  rawjob_setup_kind (kind, helper, scratch, run_now);
  CHECK (rawjob_available_kind (kind) && rawjob_stamp_kind (kind) != 0, "heif: the plug-in set up");
  /* The 1600 proxy, as the Viewer asks. */
  snprintf (path, sizeof path, "%s/thumb.heic", dir);
  t = thumbgen_start (path, NULL, 1600, 1, err, sizeof err);
  while (t && (r = thumbgen_step (t, 64)) == 0)
    ;
  {
    int w = 0, h = 0;
    img_info ri;
    if (t)
      thumbgen_size (t, &w, &h);
    CHECK (t && r == 1 && w == 1600 && h == 1067 && thumbgen_raw_developed (t), "heif: the Viewer's 1600 picture");
    CHECK (t && thumbgen_raw_details (t, &ri) && ri.width == 2400 && ri.height == 1600 && ri.format == IMG_PLUGIN,
           "heif: the plug-in's details");
  }
  thumbgen_free (t);

  p (libdir, "HeifLibrary");
  CHECK (lib_open (&l, libdir, err, sizeof err) == 0, "heif: library");
  lib_import (l, dir);
  drain (l);
  {
    cat_source all = { SRC_ALL, 0, 0, "" };
    cat_item *items = NULL;
    int n = 0, k, ready = 0, turned = 0, cam = 0, tags = -1, enc = 0;
    cat_list (lib_cat (l), &all, &items, &n);
    for (k = 0; k < n; k++)
      {
        cat_details d;
        ready += items[k].thumb_state == THUMB_READY;
        if (cat_get_details (lib_cat (l), items[k].id, &d) == 0)
          {
            {
              cat_origin eo;
              if (cat_origin_of (lib_cat (l), d.original, &eo) == 1)
                {
                  if (strstr (d.path, "plain.heic"))
                    enc += !strcmp (eo.encoding, "HEVC, 8 bits, 4:2:0");
                  if (strstr (d.path, "deep.avif"))
                    enc += !strcmp (eo.encoding, "AV1, 10 bits, 4:2:0");
                }
            }
            if (strstr (d.path, "turned"))
              turned = d.width == 800 && d.height == 1200 && d.orientation == 1;
            if (strstr (d.path, "plain"))
              {
                cat_origin o;
                cam = !strcmp (d.make, "TestCam") && d.iso == 200 && d.width == 1200
                      && !strcmp (d.taken, "2026:11:04 12:34:56");
                /* The Info tab: the EXIF where the plug-in said it is (at the
                   end of the file, as libheif writes it). */
                if (cat_origin_of (lib_cat (l), d.original, &o) == 1 && o.exif_offset > 300000 && o.exif_length > 8)
                  {
                    unsigned char *x = malloc ((size_t) o.exif_length);
                    FILE *ef = fopen (d.path, "rb");
                    if (x && ef && fseek (ef, (long) o.exif_offset, SEEK_SET) == 0
                        && fread (x, 1, (size_t) o.exif_length, ef) == (size_t) o.exif_length)
                      tags = exif_list_tiff (x, (size_t) o.exif_length, NULL, NULL);
                    if (ef)
                      fclose (ef);
                    free (x);
                  }
              }
          }
      }
    CHECK (n == 9 && ready == 9, "heif: the nine (six HEIC, three AVIF) imported, thumbnails made");

    CHECK (turned, "heif: a turned photo upright in the catalogue (800 x 1200, orientation 1)");
    CHECK (cam, "heif: camera, date and size in the catalogue, from the plug-in");
    CHECK (tags > 5, "heif: the Info tab's EXIF list, where the plug-in said it is");
    CHECK (enc == 2, "format plug-ins: each photo's encoding kept in the catalogue (HEVC 8-bit, AV1 10-bit)");
    free (items);
  }
  lib_set_previews (l, PREVIEWS_RAW);
  snprintf (path, sizeof path, "%s/plain.heic", dir);
  CHECK (lib_preview_wanted (l, IMG_PLUGIN, path) && !lib_preview_wanted (l, IMG_PLUGIN, "x.jxl"),
         "heif: previews made for them (as for raws)");
  preview_drain (l);
  {
    cat_origin o;
    int found = 0;
    cat_id id;
    for (id = 0; cat_origin_below (lib_cat (l), id, &o) == 1; id = o.id)
      found += lib_preview_find (l, o.id, pv, sizeof pv);
    CHECK (found == 9, "heif: a preview of each");
  }
    /* One typed as MimeMap's type for image/heic, without an extension:
       taken by its type (a double-clicked photo comes in this way). */
    {
      char td[700], tf[760], src[760];
      FILE *a, *bf;
      size_t got2;
      unsigned char buf[65536];
      fsys_mimemap_add ("image/heic", 0xA3A, "HEIC", "heic");
      p (td, "HeifTyped");
      fsys_mkdirs (td);
      snprintf (src, sizeof src, "%s/plain.heic", dir);
      snprintf (tf, sizeof tf, "%s/Safe,a3a", td);
      a = fopen (src, "rb");
      bf = fopen (tf, "wb");
      while (a && bf && (got2 = fread (buf, 1, sizeof buf, a)) > 0)
        fwrite (buf, 1, got2, bf);
      if (bf) fputc (0, bf);           /* not the same file as plain.heic (by its content) */
      if (a) fclose (a);
      if (bf) fclose (bf);
      lib_import (l, tf);
      drain (l);
      CHECK (cat_version_of_path (lib_cat (l), tf) != 0, "heif: a photo typed image/heic's filetype, no extension, imported alone");
    }
  lib_close (l);
  rawjob_clear_format (kind);
  CHECK (!rawjob_format (kind) && rawjob_kind_for (IMG_UNKNOWN, path, NULL, 0) < 0, "format plug-ins: removed: forgotten");
}

/* --- the lens database (the LumenLens plug-in's) */
/* A clock that is always past the time: each step reads one slice. */
static unsigned step_clock (void)
{
  return 1;
}

static lens_photo photo_of (const char *make, const char *model, const char *lens, double focal, double f)
{
  lens_photo ph;
  memset (&ph, 0, sizeof ph);
  ph.camera = -1;
  snprintf (ph.make, sizeof ph.make, "%s", make);
  snprintf (ph.model, sizeof ph.model, "%s", model);
  snprintf (ph.lens, sizeof ph.lens, "%s", lens);
  ph.focal = focal;
  ph.fnumber = f;
  return ph;
}

static void test_lensdb (void)
{
  char path[600], dir[600];
  FILE *f;
  lens_db *db;
  lens_photo ph;
  recipe_lens L;
  plugin_info pi;
  const char *mount;
  double crop;
  int i, c, list[12], n, k, ok;
  /* A small one: every kind of line. */
  snprintf (dir, sizeof dir, "%s/lensplug", work);
  fsys_mkdirs (dir);
  snprintf (path, sizeof path, "%s/Plugin", dir);
  f = fopen (path, "w");
  fputs ("Name: Lens profiles (test)\nVersion: 1.0\nKind: lens\nData: Lenses\nPackage: LumenLens\n", f);
  fclose (f);
  snprintf (path, sizeof path, "%s/Lenses", dir);
  f = fopen (path, "w");
  fputs ("LumenLens 1\n# test\n"
         "M Nikon F AF|Nikon F|Generic\nM Nikon Z|Nikon F AF|Generic\n"
         "C Nikon Corporation~Nikon|Nikon D750~D750|Nikon F AF|1\n"
         "C Nikon Corporation~Nikon|Nikon D7500~D7500|Nikon F AF|1.534\n"
         "C Nikon Corporation~Nikon|Nikon Z 6~Z 6|Nikon Z|1\n"
         "C Canon|Canon PowerShot G12~PowerShot G12|canonG12|4.63\n"
         "L Nikon|Nikon AF-S Nikkor 16-35mm f/4G ED VR~Nikkor AF-S 16-35mm f/4G ED VR|Nikon F AF|1|1.5|r\n"
         "D 16 p 0.01589 -0.08067 0.06712\nD 20 p 0.00036 -0.01787 0.02308\nD 35 p 0 0.00551 0\n"
         "T 16 3 1.0001 0 0 1.0003 0 0\nT 35 3 1.0003 0 0 1.0001 0 0\n"
         "V 16 4 -0.5 0 0\nV 16 8 -0.3 0 0\nV 35 4 -0.4 0 0\nV 35 8 -0.2 0 0\n"
         "L Nikon|Nikon AF-S DX Nikkor 18-55mm f/3.5-5.6G VR|Nikon F AF|1.53|1.5|r\n"
         "D 18 3 -0.03\nD 55 3 0.01\n"
         "L Sigma|Sigma 17-50mm f/2.8 EX DC OS HSM|Nikon F AF;Canon EF|1.53|1.5|r\n"
         "D 17 5 -0.05 0.01\n"
         "L Canon|Canon PowerShot G12 & compatibles (Standard)~fixed lens|canonG12|4.63|1.333333|r\n"
         "D 6.1 3 -0.02\n"
         "L Nikon|Nikon Nothing Measured|Nikon F AF|1|1.5|r\n", f);
  fclose (f);
  CHECK (plugins_read (dir, &pi) == 0 && !strcmp (pi.kind, "lens") && !strcmp (pi.data, path) && !pi.program[0],
         "lens db: a data plug-in, its Data file found");
  pi.enabled = 1;
  CHECK (plugins_find (&pi, 1, "lens") == &pi, "lens db: found by kind without a program");
  db = lens_db_open (path);
  CHECK (db && lens_db_count (db) == 4 && lens_db_cameras (db) == 4, "lens db: read (a lens with nothing measured left out)");
  if (!db)
    return;
  CHECK (lens_db_camera (db, "NIKON CORPORATION", "NIKON D750", &mount, &crop) == 0 && !strcmp (mount, "Nikon F AF") && crop == 1,
         "lens db: the camera from EXIF's make and model");
  CHECK (lens_db_camera (db, "Nikon", "D7500", &mount, &crop) == 1 && fabs (crop - 1.534) < 1e-6, "lens db: and LibRaw's");
  CHECK (lens_db_camera (db, "NIKON CORPORATION", "NIKON D850", &mount, &crop) < 0, "lens db: one it hasn't");
  ph = photo_of ("NIKON CORPORATION", "NIKON D750", "AF-S Nikkor 16-35mm f/4G ED VR", 20, 5.6);
  CHECK ((i = lens_db_match (db, &ph)) == 0, "lens db: the lens by its name");
  CHECK (lens_name_score ("EF24-105mm f/4L IS USM", "Canon EF 24-105mm f/4L IS USM") > 0.9
         && lens_name_score ("16.0-35.0 mm f/4.0", "16-35mm f/4") == 1, "lens db: names alike, in words and numbers");
  ph = photo_of ("NIKON CORPORATION", "NIKON D7500", "17-50mm F2.8", 30, 4);
  CHECK (lens_db_match (db, &ph) == 2, "lens db: another maker's, by its numbers");
  ph = photo_of ("NIKON CORPORATION", "NIKON D7500", "17-55mm F2.8", 30, 4);
  CHECK (lens_db_match (db, &ph) < 0, "lens db: numbers that differ aren't it");
  ph = photo_of ("NIKON CORPORATION", "NIKON D7500", "", 30, 4);
  ph.fmin = 18; ph.fmax = 55; ph.amin = 3.5;
  CHECK (lens_db_match (db, &ph) == 1, "lens db: by the lens's focal lengths and aperture (LensSpecification)");
  ph = photo_of ("Canon", "Canon PowerShot G12", "", 10, 4);
  CHECK (lens_db_match (db, &ph) == 3, "lens db: a compact's own lens");
  ph = photo_of ("Canon", "Canon EOS 5D Mark III", "Nikon AF-S Nikkor 16-35mm f/4G ED VR", 20, 4);
  CHECK (lens_db_match (db, &ph) == 0, "lens db: the camera unknown, the lens by name");
  /* Suggestions: what fits and covers the focal length. */
  ph = photo_of ("NIKON CORPORATION", "NIKON D7500", "", 30, 4);
  n = lens_db_suggest (db, &ph, list, 12);
  for (k = 0, ok = 1; k < n; k++)
    if (list[k] == 3)
      ok = 0;                          /* not the compact's */
  CHECK (n == 3 && ok, "lens db: suggestions fit the camera");
  /* Profiles: between measurements. */
  ph = photo_of ("NIKON CORPORATION", "NIKON D750", "AF-S Nikkor 16-35mm f/4G ED VR", 18, 5.6);
  CHECK (lens_db_profile (db, 0, &ph, &L) == 0 && L.on && !strcmp (L.name, "Nikon AF-S Nikkor 16-35mm f/4G ED VR")
         && L.dist == LENS_D_PTLENS && fabsf (L.d[0] - (0.01589f + 0.00036f) / 2) < 1e-6f, "lens db: distortion between 16 and 20 mm");
  CHECK (L.tca == LENS_T_POLY3 && L.t[0] > 1.0001f && L.t[0] < 1.0003f, "lens db: colour between 16 and 35 mm");
  {
    /* f/5.6 between f/4 and f/8 in 1/N: a third of the way at 16 mm... */
    double t = (1 / 5.6 - 1 / 4.0) / (1 / 8.0 - 1 / 4.0), a16 = -0.5 + 0.2 * t, a35 = -0.4 + 0.2 * t;
    double want = a16 + (a35 - a16) * (18 - 16) / (35.0 - 16);
    CHECK (L.vig && fabs (L.k[0] - want) < 1e-5, "lens db: vignetting between focal lengths and apertures");
  }
  CHECK (fabsf (L.rh - 1.8028f) < 1e-3f && fabsf (L.rv - 1) < 1e-6f && !L.camera[0], "lens db: scales for a camera the size measured on");
  /* A smaller sensor sees less of the lens's circle. */
  ph = photo_of ("NIKON CORPORATION", "NIKON D7500", "AF-S Nikkor 16-35mm f/4G ED VR", 20, 5.6);
  lens_db_profile (db, 0, &ph, &L);
  CHECK (fabsf (L.rv - 1 / 1.534f) < 1e-4f, "lens db: scaled for the camera's crop factor");
  /* A camera it hasn't: one like it chosen instead. */
  ph = photo_of ("NIKON CORPORATION", "NIKON D850", "AF-S Nikkor 16-35mm f/4G ED VR", 20, 5.6);
  ph.crop = 1.5;
  n = lens_db_similar (db, 0, ph.crop, ph.make, list, 12);
  CHECK (n == 3 && list[0] == 1 && list[1] == 0, "lens db: cameras it fits (a Z with an adapter), the nearest sensor first");
  ph.camera = list[0];
  CHECK (lens_db_photo_camera (db, &ph) == 1 && lens_db_profile (db, 0, &ph, &L) == 0 && fabsf (L.rv - 1 / 1.534f) < 1e-4f
         && !strcmp (L.camera, "Nikon D7500"), "lens db: the chosen camera's sensor, and its name kept");
  n = lens_db_similar (db, 3, 0, NULL, list, 12);
  CHECK (n == 1 && list[0] == 3, "lens db: a compact's lens fits only its camera");
  lens_db_close (db);
  /* The real one. */
  db = lens_db_open ("plugins/lens/Lenses");
  CHECK (db && lens_db_count (db) > 1400 && lens_db_cameras (db) > 1000, "lens db: the plug-in's");
  if (db)
    {
      ph = photo_of ("NIKON CORPORATION", "NIKON D750", "AF-S Nikkor 16-35mm f/4G ED VR", 20, 5.6);
      i = lens_db_match (db, &ph);
      CHECK (i >= 0 && strstr (lens_db_name (db, i), "16-35mm f/4G") && lens_db_profile (db, i, &ph, &L) == 0 && L.dist && L.vig,
             "lens db: the plug-in's: a Nikon lens");
      ph = photo_of ("Canon", "Canon EOS 5D Mark III", "EF24-105mm f/4L IS USM", 50, 8);
      i = lens_db_match (db, &ph);
      CHECK (i >= 0 && !strcmp (lens_db_name (db, i), "Canon EF 24-105mm f/4L IS USM"), "lens db: the plug-in's: a Canon lens");
      c = lens_db_camera (db, "SONY", "ILCE-7M3", &mount, &crop);
      CHECK (c >= 0 && !strcmp (mount, "Sony E"), "lens db: the plug-in's: a Sony camera");
      /* Read a piece at a time (as Lumen does in the background): the same. */
      {
        lens_loading *ld = lens_db_start ("plugins/lens/Lenses");
        lens_db *db2 = NULL;
        int steps = 0, same = 1, k;
        while (ld && !lens_db_step (ld, step_clock, 0, &db2))
          steps++;
        lens_db_stop (ld);
        same = db2 && lens_db_count (db2) == lens_db_count (db) && lens_db_cameras (db2) == lens_db_cameras (db);
        for (k = 0; same && k < lens_db_count (db); k++)
          same = !strcmp (lens_db_name (db, k), lens_db_name (db2, k));
        ph = photo_of ("NIKON CORPORATION", "NIKON D700", "AF Zoom-Nikkor 80-200mm f/2.8D ED", 135, 4);
        CHECK (steps > 5 && same && db2 && lens_db_match (db2, &ph) == lens_db_match (db, &ph)
               && lens_db_profile (db2, lens_db_match (db2, &ph), &ph, &L) == 0 && L.dist,
               "lens db: read in pieces, the same database");
        lens_db_close (db2);
        lens_db_stop (lens_db_start ("plugins/lens/Lenses"));   /* abandoned at once */
        CHECK (!lens_db_start ("no/such/Lenses"), "lens db: no file, nothing to read");
      }
      lens_db_close (db);
    }
  /* The raw samples' lenses, as their files say. */
  {
    const char *rs = getenv ("RAW_SAMPLES");
    if (rs && *rs)
      {
        char nef[700];
        snprintf (nef, sizeof nef, "%s/iss030e122639.NEF", rs);
        memset (&ph, 0, sizeof ph);
        ph.camera = -1;
        CHECK (lens_photo_read (&ph, nef) == 0 && ph.make[0] && ph.focal > 0, "lens db: a raw's camera and focal length");
        printf ("test_core: %s: %s %s, lens \"%s\" %g mm f/%g (%g-%g mm), crop %.2f\n", nef, ph.make, ph.model, ph.lens,
                ph.focal, ph.fnumber, ph.fmin, ph.fmax, ph.crop);
      }
  }
}

/* --- where photos were taken (EXIF GPS), and IPTC captions */

static void put16be (unsigned char *p, unsigned v) { p[0] = (unsigned char) (v >> 8); p[1] = (unsigned char) v; }

/* A big-endian EXIF block with a GPS IFD: 33 51' 35.46" S, 151 12' 30.48" E,
   58.4 m, facing 270.5 (true). With south 0: north. */
static size_t make_gps_exif (unsigned char *ex, int south, int below)
{
  unsigned char *t = ex + 6;
  size_t g = 26, d;
  memset (ex, 0, 400);
  memcpy (ex, "Exif\0\0", 6);
  memcpy (t, "MM\0\x2a", 4); put32be (t + 4, 8);
  put16be (t + 8, 1);                                        /* IFD0: GPSInfo */
  put16be (t + 10, 0x8825); put16be (t + 12, 4); put32be (t + 14, 1); put32be (t + 18, (unsigned long) g);
  put32be (t + 22, 0);
  d = g + 2 + 12 * 9 + 4;                                     /* the rationals after the IFD */
  put16be (t + g, 9);
#define ENT(k, tag, type, count) (put16be (t + g + 2 + 12 * (k), tag), put16be (t + g + 4 + 12 * (k), type), \
                                  put32be (t + g + 6 + 12 * (k), count))
  ENT (0, 1, 2, 2); t[g + 2 + 8] = south ? 'S' : 'N';
  ENT (1, 2, 5, 3); put32be (t + g + 2 + 12 + 8, (unsigned long) d);
  put32be (t + d, 33); put32be (t + d + 4, 1); put32be (t + d + 8, 51); put32be (t + d + 12, 1);
  put32be (t + d + 16, 3546); put32be (t + d + 20, 100);
  ENT (2, 3, 2, 2); t[g + 2 + 24 + 8] = 'E';
  ENT (3, 4, 5, 3); put32be (t + g + 2 + 36 + 8, (unsigned long) d + 24);
  put32be (t + d + 24, 151); put32be (t + d + 28, 1); put32be (t + d + 32, 12); put32be (t + d + 36, 1);
  put32be (t + d + 40, 3048); put32be (t + d + 44, 100);
  ENT (4, 5, 1, 1); t[g + 2 + 48 + 8] = (unsigned char) below;
  ENT (5, 6, 5, 1); put32be (t + g + 2 + 60 + 8, (unsigned long) d + 48);
  put32be (t + d + 48, 584); put32be (t + d + 52, 10);
  ENT (6, 16, 2, 2); t[g + 2 + 72 + 8] = 'T';
  ENT (7, 17, 5, 1); put32be (t + g + 2 + 84 + 8, (unsigned long) d + 56);
  put32be (t + d + 56, 2705); put32be (t + d + 60, 10);
  ENT (8, 0, 1, 4); t[g + 2 + 96 + 8] = 2; t[g + 2 + 96 + 9] = 3;
#undef ENT
  put32be (t + g + 2 + 12 * 9, 0);
  return 6 + d + 64;
}

static void test_geo (void)
{
  unsigned char ex[400], buf[4096];
  unsigned char rgb[3] = { 90, 120, 160 };
  char path[1024], libdir[1024], err[256], text[64];
  size_t exn, n;
  img_info in;
  FILE *f;
  library *l;
  cat_geo g;
  cat_id v;

  exn = make_gps_exif (ex, 1, 0);
  p (path, "geo-sydney.jpg");
  make_flat_jpeg (path, 40, 30, rgb, NULL, 0, ex, exn);
  f = fopen (path, "rb");
  n = fread (buf, 1, sizeof buf, f);
  fclose (f);
  CHECK (img_read_info (buf, n, &in) == 0 && (in.geo & GEO_POS), "GPS: a position read");
  CHECK (in.lat == -338598500 && in.lon == 1512084667, "GPS: degrees, minutes, seconds to ten-millionths, S negative");
  CHECK ((in.geo & GEO_ALT) && in.alt_mm == 58400, "GPS: altitude in mm");
  CHECK ((in.geo & GEO_DIR) && in.bearing == 270500, "GPS: bearing in thousandths of a degree");
  img_geo_text (in.lat, in.lon, text, sizeof text);
  CHECK (strcmp (text, "33.859850\xB0 S, 151.208467\xB0 E") == 0, "GPS: the position for people");
  {
    unsigned char e2[400];
    size_t m = make_gps_exif (e2, 0, 1);
    img_info i2;
    memset (&i2, 0, sizeof i2);
    img_read_tiff_tags (e2 + 6, m - 6, &i2);
    CHECK (i2.lat == 338598500 && i2.alt_mm == -58400, "GPS: north, and below sea level");
    /* 0,0 is a camera without a fix: no position. */
    put32be (e2 + 6 + 140 + 0, 0); put32be (e2 + 6 + 140 + 8, 0); put32be (e2 + 6 + 140 + 16, 0);
    put32be (e2 + 6 + 140 + 24, 0); put32be (e2 + 6 + 140 + 32, 0); put32be (e2 + 6 + 140 + 40, 0);
    memset (&i2, 0, sizeof i2);
    img_read_tiff_tags (e2 + 6, m - 6, &i2);
    CHECK (!(i2.geo & GEO_POS), "GPS: 0,0 isn't a position");
    for (n = 0; n < m; n++)
      {
        memset (&i2, 0, sizeof i2);
        img_read_tiff_tags (e2 + 6, n, &i2);
      }
    CHECK (1, "GPS: cut-off blocks read safely");
  }

  /* IPTC caption and keywords (APP13, Photoshop 3.0, resource &404). */
  {
    static const unsigned char iptc[] = {
      0xFF, 0xD8, 0xFF, 0xED, 0x00, 0x44,
      'P', 'h', 'o', 't', 'o', 's', 'h', 'o', 'p', ' ', '3', '.', '0', 0,
      '8', 'B', 'I', 'M', 0x04, 0x04, 0, 0, 0, 0, 0, 0x27,
      0x1C, 0x02, 0x00, 0x00, 0x02, 0x00, 0x04,
      0x1C, 0x02, 0x78, 0x00, 0x0A, 'O', 'l', 'd', ' ', 'h', 'a', 'r', 'b', 'o', 'u',
      0x1C, 0x02, 0x19, 0x00, 0x03, 's', 'e', 'a',
      0x1C, 0x02, 0x19, 0x00, 0x04, 'b', 'o', 'a', 't', 0,
      0xFF, 0xD9 };
    char cap[64], kw[64];
    CHECK (img_iptc_read (iptc, sizeof iptc, cap, sizeof cap, kw, sizeof kw) == 1
           && strcmp (cap, "Old harbou") == 0 && strcmp (kw, "sea, boat") == 0, "IPTC: caption and keywords");
    for (n = 0; n < sizeof iptc; n++)
      img_iptc_read (iptc, n, cap, sizeof cap, kw, sizeof kw);
    CHECK (1, "IPTC: cut-off segments read safely");
  }

  /* The library keeps it, and reads it for photos it had before. */
  p (libdir, "LibraryGeo");
  CHECK (lib_open (&l, libdir, err, sizeof err) == 0, "geo: library opens");
  lib_import (l, path);
  drain (l);
  v = cat_version_of_path (lib_cat (l), path);
  CHECK (v > 0 && cat_get_geo (lib_cat (l), v, &g) == 1 && g.lat == -338598500 && g.lon == 1512084667
         && g.alt_mm == 58400 && g.bearing == 270500 && g.geo == (GEO_POS | GEO_ALT | GEO_DIR), "geo: kept at import");
  sqlite3_exec (cat_sqlite (lib_cat (l)), "UPDATE original SET geo = -1, lat = 0, lon = 0", NULL, NULL, NULL);
  CHECK (cat_get_geo (lib_cat (l), v, &g) == 0, "geo: not read yet, none");
  lib_close (l);
  CHECK (lib_open (&l, libdir, err, sizeof err) == 0, "geo: library reopens");
  drain (l);
  CHECK (lib_geo_pending (l), "geo: photos to read");
  clock_cs = 0;
  {
    int guard = 0;
    while (lib_geo_pending (l) && guard++ < 100)
      lib_geo_step (l, fake_now, clock_cs + 3);
  }
  CHECK (!lib_geo_pending (l) && cat_get_geo (lib_cat (l), v, &g) == 1 && g.lat == -338598500,
         "geo: read in the background");

  /* Searching by place, as a map program asks. */
  {
    cat_geo_query q;
    cat_geo_hit *h = NULL;
    int nh = 0, total = 0, x0, y0, x1, y1;
    char p2[1024];
    unsigned char e3[400];
    size_t m3 = make_gps_exif (e3, 0, 0);       /* 33.86 N, 151.21 E */
    p (p2, "geo-north.jpg");
    make_flat_jpeg (p2, 40, 30, rgb, NULL, 0, e3, m3);
    lib_import (l, p2);
    p (p2, "geo-none.jpg");
    make_flat_jpeg (p2, 40, 30, rgb, NULL, 0, NULL, 0);
    lib_import (l, p2);
    drain (l);
    CHECK (cat_geo_bounds (lib_cat (l), &x0, &y0, &x1, &y1) == 2 && y0 == -338598500 && y1 == 338598500
           && x0 == 1512084667 && x1 == 1512084667, "geo: the box round the library's places");
    memset (&q, 0, sizeof q);
    CHECK (cat_geo_find (lib_cat (l), &q, &h, &nh, &total) == 0 && nh == 2 && total == 2, "geo: all with a place");
    free (h);
    q.how = CAT_GEO_BOX;
    q.x0 = 1510000000; q.x1 = 1520000000; q.y0 = -340000000; q.y1 = -330000000;
    CHECK (cat_geo_find (lib_cat (l), &q, &h, &nh, NULL) == 0 && nh == 1 && h[0].version == v, "geo: in a box");
    free (h);
    q.x0 = 1520000000; q.x1 = -1700000000;      /* across 180 degrees, east of it */
    CHECK (cat_geo_find (lib_cat (l), &q, &h, &nh, NULL) == 0 && nh == 0, "geo: a box across 180 degrees");
    free (h);
    memset (&q, 0, sizeof q);
    q.how = CAT_GEO_RADIUS;
    q.x0 = 1512084667; q.y0 = -338598500 + 9000;   /* 100 m north */
    q.radius = 120;
    CHECK (cat_geo_find (lib_cat (l), &q, &h, &nh, NULL) == 0 && nh == 1, "geo: within 120 m");
    free (h);
    q.radius = 80;
    CHECK (cat_geo_find (lib_cat (l), &q, &h, &nh, NULL) == 0 && nh == 0, "geo: not within 80 m");
    free (h);
    CHECK (fabs (cat_geo_distance (-338598500, 1512084667, -338598500 + 9000, 1512084667) - 100.08) < 0.5,
           "geo: distance in metres");
    memset (&q, 0, sizeof q);
    snprintf (q.words, sizeof q.words, "north");
    CHECK (cat_geo_find (lib_cat (l), &q, &h, &nh, NULL) == 0 && nh == 1 && strcmp (h[0].name, "geo-north.jpg") == 0,
           "geo: with words");
    free (h);
    memset (&q, 0, sizeof q);
    q.page_size = 1; q.page = 2;
    CHECK (cat_geo_find (lib_cat (l), &q, &h, &nh, &total) == 0 && nh == 1 && total == 2, "geo: a page at a time");
    free (h);

    /* The messages. */
    {
      int b[GEODATA_WORDS];
      unsigned char t[5];
      char u[64];
      cat_geo_hit hit;
      geodata_fetch f;
      memset (&hit, 0, sizeof hit);
      hit.version = 77;
      hit.geo.geo = GEO_POS | GEO_DIR;
      hit.geo.lat = -338598500; hit.geo.lon = 1512084667; hit.geo.bearing = 270500;
      snprintf (hit.taken, sizeof hit.taken, "2026:06:16 04:52:10");
      snprintf (hit.name, sizeof hit.name, "IMG_1/jpg");
      snprintf (hit.caption, sizeof hit.caption, "Caf\xE9 by the harbour");
      CHECK (geodata_time (hit.taken, t) == 0 && t[0] == 168 && t[1] == 49 && t[2] == 173 && t[3] == 233 && t[4] == 92,
             "geodata: a RISC OS time");
      geodata_transmit (b, 0, 1, &hit, GD_DRAW_MAP | GD_FILE);
      CHECK (b[4] == GeoData_TransmitData && b[6] == 1 && b[7] == 77 && b[8] == GD_PHOTOS, "geodata: TransmitData ids");
      CHECK (b[9] == (GD_DRAW_MAP | GD_FILE | GD_LATLON | GD_TIME | GD_HAS_BEARING | GD_TITLE | GD_DESCRIPTION),
             "geodata: TransmitData flags");
      CHECK (b[10] == 1512084667 && b[11] == -338598500 && b[16] == 270500 && memcmp ((char *) b + 52, t, 5) == 0,
             "geodata: longitude, latitude, time, bearing");
      CHECK (strcmp ((char *) b + 76, "Caf\xC3\xA9 by the harbour") == 0
             && strcmp ((char *) b + 76 + strlen ((char *) b + 76) + 1, "IMG_1/jpg") == 0, "geodata: title (UTF-8) and description");
      CHECK (b[0] % 4 == 0 && b[0] <= 256 && b[0] == 108, "geodata: its length");
      memset (hit.caption, 'x', 400);
      hit.caption[400] = '\0';
      geodata_transmit (b, 0, 1, &hit, 0);
      CHECK (b[0] <= 256 && ((char *) b)[255] == '\0', "geodata: a long caption fits in 256 bytes");
      geodata_utf8 ("Caf\xC3\xA9", u, sizeof u);
      CHECK (strcmp (u, "Caf\xC3\xA9") == 0, "geodata: UTF-8 kept");
      geodata_utf8 ("\xE9\xE9\xE9", u, 6);
      CHECK (strcmp (u, "\xC3\xA9\xC3\xA9") == 0, "geodata: cut at a whole character");
      geodata_latin1 ("Caf\xC3\xA9 \xE2\x82\xAC", u, sizeof u);
      CHECK (strcmp (u, "Caf\xE9 ?") == 0, "geodata: UTF-8 to Latin-1");
      memset (b, 0, sizeof b);
      b[5] = 9; b[6] = 1; b[9] = 1512084667; b[10] = -338598500; b[11] = 500;
      b[13] = GD_PHOTOS | GD_RADIUS | GD_PAGED; b[16] = 20; b[17] = 2;
      geodata_read_fetch (b, &f);
      CHECK (geodata_query (&f, "harbour", &q) == 0 && q.how == CAT_GEO_RADIUS && q.radius == 500 && q.y0 == -338598500
             && q.page_size == 20 && q.page == 2 && strcmp (q.words, "harbour") == 0, "geodata: FetchData read");
      b[13] = (1 << 2) | GD_BOX;               /* fixed points only */
      geodata_read_fetch (b, &f);
      CHECK (geodata_query (&f, NULL, &q) != 0, "geodata: not asked for photos");
      /* OpenMap: one photo, centred; two far apart, a box round them with a margin. */
      {
        cat_geo_hit two[2];
        memset (two, 0, sizeof two);
        two[0].geo.lat = 545000000; two[0].geo.lon = -31000000;
        two[1].geo.lat = 546200000; two[1].geo.lon = -30000000;
        geodata_open_map (b, two, 1, "Derwentwater");
        CHECK (b[4] == GeoData_OpenMap && b[5] == GD_OPEN_TITLE && b[6] == -31000000 && b[7] == 545000000
               && strcmp ((char *) b + 60, "Derwentwater") == 0 && b[0] == 76, "geodata: OpenMap on one photo, centred");
        geodata_open_map (b, two, 2, NULL);
        CHECK (b[5] == GD_OPEN_BOUNDS && b[6] < -31000000 && b[8] > -30000000 && b[7] < 545000000 && b[9] > 546200000
               && b[7] == 545000000 - (1200000 / 6 + 2000) && b[0] == 60, "geodata: OpenMap round two, with a margin");
        two[1].geo.lat = 545001000;              /* 11 m apart: centred between them */
        two[1].geo.lon = -31000000;
        geodata_open_map (b, two, 2, NULL);
        CHECK (b[5] == 0 && b[7] == 545000500, "geodata: OpenMap on a close group, centred");
      }
      geodata_details (b, 1, 2, x0, y0, x1, y1, "Lumen photos", "2 photos");
      CHECK (b[4] == GeoData_DatasetDetails && b[5] == 1 && b[7] == -338598500 && (b[10] & GD_PHOTOS) && (b[10] & GD_TEXT)
             && strcmp ((char *) b + 48, "Lumen photos") == 0 && strcmp ((char *) b + 48 + 13, "2 photos") == 0,
             "geodata: DatasetDetails");
    }
  }
  lib_close (l);
}

int main (int argc, char **argv)
{
  bus_trap_install ();
  if (argc < 2)
    {
      fprintf (stderr, "usage: test_core WORKDIR\n");
      return 2;
    }
  snprintf (work, sizeof work, "%s", argv[1]);
  fsys_mkdirs (work);
  test_exif ();
  test_geo ();
  test_colour ();
  test_jpeg_scaling ();
  test_raw ();
  test_plugins ();
  test_lensdb ();
  test_raw_names ();
  test_mistyped ();
  test_tiff_import ();
  test_order_by_file_date ();
  test_fits ();
  test_align ();
  test_stack ();
  test_prefs ();
  rawjob_set_switched_off (1);
  checks++; if (!strstr (rawjob_missing_message (), "switched off")) { fails++; fprintf (stderr, "FAIL: raw plug-in off: says so\n"); }
  rawjob_set_switched_off (0);
  checks++; if (!strstr (rawjob_missing_message (), "install LumenRaw")) { fails++; fprintf (stderr, "FAIL: no raw plug-in: says to install\n"); }
  make_fixtures ();
  test_library ();
  test_time_slicing ();
  test_watch ();
  test_lazy_thumbs ();
  test_previews ();
  test_heif ();
  test_presets ();
  test_libxfer ();
  test_libcheck ();
  test_libxfer_more ();
  printf ("test_core: %d checks, %d failed\n", checks, fails);
  return fails != 0;
}
