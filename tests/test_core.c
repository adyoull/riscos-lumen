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
#include "../src/core/exif.h"
#include "../src/core/colour.h"
#include "../src/core/fsys.h"
#include "../src/core/library.h"
#include "../src/core/thumbgen.h"

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
  make_fixtures ();
  test_library ();
  test_time_slicing ();
  printf ("test_core: %d checks, %d failed\n", checks, fails);
  return fails != 0;
}
