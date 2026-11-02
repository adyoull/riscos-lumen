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
#include <sys/stat.h>
#include "../src/core/catalog.h"
#include "../src/core/xmp.h"
#include "../src/core/exif.h"
#include "../src/core/colour.h"
#include "../src/core/rawjob.h"
#include "../src/core/fsys.h"
#include "../src/core/library.h"
#include "../src/core/thumbgen.h"
#include "../src/core/plugins.h"
#include "../src/core/prefs.h"

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

/* --- camera raw files through the plug-in (when LUMENRAW and RAW_SAMPLES are set) */

/* Runs the helper there and then (on RISC OS it runs in a task window). */
static int run_now (const char *cmd, int tag)
{
  int r = system (cmd);
  rawjob_ended (tag);
  return r < 0 ? -1 : 0;
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
  CHECK (img_probe_named (head, (size_t) r, "picture/tif") == IMG_UNKNOWN, "a TIFF with another name isn't taken for raw");

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
  test_colour ();
  test_raw ();
  test_plugins ();
  test_raw_names ();
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
  printf ("test_core: %d checks, %d failed\n", checks, fails);
  return fails != 0;
}
