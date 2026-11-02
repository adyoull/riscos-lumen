/* The develop pipeline and recipes on Linux (x86-64, and ARM under
   qemu-arm-aligntrap). Usage: test_develop WORKDIR */
#include <math.h>
#include <stdio.h>
#include "bus_trap.h"
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <jpeglib.h>
#include "../src/core/autoadj.h"
#include "../src/core/develop.h"
#include "../src/core/geometry.h"
#include "../src/core/resample.h"
#include "../src/core/sharpen.h"
#include "../src/core/export.h"
#include "../src/core/xmp.h"
#include "../src/core/denoise.h"
#include "../src/core/exifout.h"
#include <png.h>
#include "../src/core/fsys.h"
#include "../src/core/library.h"
#include "../src/core/recipe.h"
#include "../src/core/thumbgen.h"
#include "../src/core/styles.h"
#include "../src/core/naming.h"
#include "../src/core/colour.h"

static int checks, fails;
#define CHECK(c, what) do { checks++; if (!(c)) { fails++; fprintf (stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, what); } } while (0)

/* Develops one 8-bit sRGB pixel with recipe r. */
static void dev1 (const recipe *r, int R, int G, int B, int out[3])
{
  static develop d;
  uint16_t lin[3];
  unsigned char in[3];
  uint32_t px;
  in[0] = (unsigned char) R; in[1] = (unsigned char) G; in[2] = (unsigned char) B;
  develop_prepare (&d, r);
  dev_to_linear (in, lin, 1);
  develop_rows (&d, lin, 1, 0, 1, &px, NULL);
  out[0] = (int) (px & 255); out[1] = (int) (px >> 8 & 255); out[2] = (int) (px >> 16 & 255);
}

static void test_recipe (void)
{
  recipe r, s;
  char text[512];
  recipe_default (&r);
  CHECK (recipe_is_default (&r), "defaults are default");
  CHECK (recipe_format (&r, text, sizeof text) == 0 && text[0] == '\0', "the defaults' text is empty");
  recipe_set (&r, P_EV, 0.3333f);
  CHECK (fabsf (r.v[P_EV] - 0.35f) < 1e-4f, "exposure rounds to its 0.05 step");
  recipe_set (&r, P_TEMP, 500);
  CHECK (r.v[P_TEMP] == 100, "clamped to the range");
  recipe_set (&r, P_VIBRANCE, 18);
  recipe_set (&r, P_TEMP, -12);
  recipe_format (&r, text, sizeof text);
  CHECK (strcmp (text, "wb temp=-12 tint=0\nexposure ev=0.35\nenhance contrast=0 saturation=0 vibrance=18\n") == 0,
         "text form: only changed groups, one line each");
  recipe_parse (&s, text);
  CHECK (recipe_equal (&r, &s), "parse(format(r)) == r");
  recipe_parse (&s, "wb temp=7 future=3\nsharpen amount=50\n\n  levels   white=99.5\nexposure ev=-9");
  CHECK (s.v[P_TEMP] == 7 && s.v[P_WHITE] == 99.5f && s.v[P_EV] == -4, "unknown keys and lines ignored; clamped");
  CHECK (recipe_format (&r, text, 10) == -1, "too small a buffer is refused");
  recipe_set (&r, P_TEMP, -0.2f);
  recipe_format (&r, text, sizeof text);
  CHECK (strstr (text, "-0") == NULL || strstr (text, "temp=0") != NULL, "no -0");
}

static void test_pixels (void)
{
  recipe r;
  int o[3], v, worst = 0, i;
  recipe_default (&r);
  for (v = 0; v < 256; v++)
    {
      dev1 (&r, v, v, v, o);
      if (abs (o[0] - v) > worst) worst = abs (o[0] - v);
    }
  CHECK (worst == 0, "an unedited photo comes back unchanged (all 256 levels)");

  /* Exposure +1 EV: 18% grey (sRGB 118) to 36% (sRGB ~163). */
  recipe_set (&r, P_EV, 1);
  dev1 (&r, 118, 118, 118, o);
  CHECK (o[0] >= 160 && o[0] <= 166 && o[0] == o[1] && o[1] == o[2], "+1 EV doubles the light of mid grey");
  recipe_default (&r);
  recipe_set (&r, P_EV, -1);
  dev1 (&r, 255, 255, 255, o);
  CHECK (o[0] >= 186 && o[0] <= 190, "-1 EV: white to half light (sRGB ~188)");

  recipe_default (&r);
  recipe_set (&r, P_TEMP, 50);
  dev1 (&r, 128, 128, 128, o);
  CHECK (o[0] > 128 && o[2] < 128, "warmer: grey gets redder and less blue");
  recipe_set (&r, P_TEMP, -50);
  dev1 (&r, 128, 128, 128, o);
  CHECK (o[0] < 128 && o[2] > 128, "cooler: the other way");
  recipe_default (&r);
  recipe_set (&r, P_TINT, 60);
  dev1 (&r, 128, 128, 128, o);
  CHECK (o[1] < o[0] && o[1] < o[2], "tint +: magenta (less green)");

  recipe_default (&r);
  recipe_set (&r, P_BLACK, 10);
  dev1 (&r, 20, 20, 20, o);
  CHECK (o[0] == 0, "black point 10%: sRGB 20 goes to black");
  recipe_default (&r);
  recipe_set (&r, P_WHITE, 80);
  dev1 (&r, 210, 210, 210, o);
  CHECK (o[0] == 255, "white point 80%: sRGB 210 goes to white");
  recipe_default (&r);
  recipe_set (&r, P_MID, 60);
  dev1 (&r, 100, 100, 100, o);
  CHECK (o[0] > 115, "midtones +: brighter mid tones");
  dev1 (&r, 0, 0, 0, o);
  CHECK (o[0] == 0, "and black stays black");

  recipe_default (&r);
  recipe_set (&r, P_CONTRAST, 60);
  dev1 (&r, 60, 60, 60, o);
  CHECK (o[0] < 60, "contrast +: shadows darker");
  dev1 (&r, 200, 200, 200, o);
  CHECK (o[0] > 200, "contrast +: highlights lighter");
  recipe_set (&r, P_CONTRAST, -60);
  dev1 (&r, 60, 60, 60, o);
  CHECK (o[0] > 60, "contrast -: shadows lifted");

  recipe_default (&r);
  recipe_set (&r, P_SATURATION, -100);
  dev1 (&r, 200, 60, 40, o);
  CHECK (o[0] == o[1] && o[1] == o[2], "saturation -100: grey");
  recipe_set (&r, P_SATURATION, 50);
  dev1 (&r, 150, 120, 110, o);
  CHECK (o[0] > 150 && o[2] < 110, "saturation +: colour stronger");
  recipe_default (&r);
  recipe_set (&r, P_VIBRANCE, 80);
  {
    int dull[3], bright[3];
    dev1 (&r, 140, 120, 110, dull);
    dev1 (&r, 230, 40, 30, bright);
    CHECK (dull[0] - 140 > 0 && (bright[1] - 40) >= -14, "vibrancy: dull colours gain, strong ones barely move");
  }

  /* Every table is monotonic for a spread of recipes: no tone reversals. */
  {
    develop *d = malloc (sizeof *d);
    int k, c, mono = 1;
    srand (3);
    for (k = 0; k < 200; k++)
      {
        int p;
        for (p = 0; p < P_COUNT; p++)
          {
            const recipe_param *q = &recipe_params[p];
            recipe_set (&r, p, q->min + (q->max - q->min) * (float) rand () / (float) RAND_MAX);
          }
        develop_prepare (d, &r);
        for (c = 0; c < 3; c++)
          for (i = 1; i < DEV_LUT_SIZE; i++)
            if (d->tone[c][i] < d->tone[c][i - 1])
              mono = 0;
      }
    CHECK (mono, "tone tables never reverse (200 random recipes)");
    free (d);
  }
}

static void test_histogram_and_speed (void)
{
  int w = 1600, h = 1067, y, i;
  uint16_t *src = malloc ((size_t) w * h * 6);
  uint32_t *dst = malloc ((size_t) w * h * 4);
  static dev_hist hist;
  unsigned total = 0, totals[4] = { 0, 0, 0, 0 };
  develop *d = malloc (sizeof *d);
  recipe r;
  clock_t t0;
  double ms_plain, ms_full;
  for (i = 0; i < w * h * 3; i++)
    src[i] = (uint16_t) ((i * 2654435761u) >> 16);
  recipe_default (&r);
  recipe_set (&r, P_TEMP, 12); recipe_set (&r, P_EV, 0.35f); recipe_set (&r, P_CONTRAST, 15);
  develop_prepare (d, &r);
  memset (&hist, 0, sizeof hist);
  t0 = clock ();
  for (y = 0; y < h; y += 64)
    develop_rows (d, src, w, y, y + 64 < h ? y + 64 : h, dst, &hist);
  ms_plain = (double) (clock () - t0) * 1000 / CLOCKS_PER_SEC;
  for (i = 0; i < DEV_HIST_BINS; i++)
    {
      int c;
      for (c = 0; c < 4; c++)
        totals[c] += hist.c[c][i];
    }
  total = totals[0];
  CHECK (total == (unsigned) (w * h) && totals[1] == total && totals[2] == total && totals[3] == total,
         "the histogram counts every pixel, in each channel and luminance");
  {
    /* Each channel's bins match the pixels' own levels. */
    unsigned want = 0;
    int v = (int) (dst[12345] & 255);
    for (i = 0; i < w * h; i++)
      if ((int) (dst[i] & 255) == v)
        want++;
    CHECK (hist.c[HIST_R][v] == want, "red bin counts the pixels with that red");
  }
  recipe_set (&r, P_SATURATION, 20); recipe_set (&r, P_VIBRANCE, 20);
  develop_prepare (d, &r);
  t0 = clock ();
  develop_rows (d, src, w, 0, h, dst, NULL);
  ms_full = (double) (clock () - t0) * 1000 / CLOCKS_PER_SEC;
  printf ("develop 1600x1067 on this machine: %.1f ms tone only, %.1f ms with saturation and vibrancy\n",
          ms_plain, ms_full);
  t0 = clock ();
  for (i = 0; i < 10; i++)
    develop_prepare (d, &r);
  printf ("develop_prepare: %.2f ms\n", (double) (clock () - t0) * 100 / CLOCKS_PER_SEC);
  free (src); free (dst); free (d);
}

static void make_jpeg (const char *path, int w, int h, int v)
{
  struct jpeg_compress_struct c;
  struct jpeg_error_mgr e;
  FILE *f = fopen (path, "wb");
  unsigned char *row = malloc ((size_t) w * 3);
  memset (row, v, (size_t) w * 3);
  c.err = jpeg_std_error (&e);
  jpeg_create_compress (&c);
  jpeg_stdio_dest (&c, f);
  c.image_width = (JDIMENSION) w; c.image_height = (JDIMENSION) h;
  c.input_components = 3; c.in_color_space = JCS_RGB;
  jpeg_set_defaults (&c);
  jpeg_start_compress (&c, TRUE);
  while (c.next_scanline < c.image_height)
    {
      JSAMPROW rr = row;
      jpeg_write_scanlines (&c, &rr, 1);
    }
  jpeg_finish_compress (&c);
  jpeg_destroy_compress (&c);
  fclose (f);
  free (row);
}

static unsigned clk;
static unsigned now (void) { return clk += 1; }

static int mean_of_thumb (library *l, cat_id v)
{
  char path[1100], err[200];
  rgb_image img;
  long sum = 0;
  int i, n;
  lib_thumb_path (l, v, path, sizeof path);
  if (img_load_jpeg (path, &img, err, sizeof err))
    return -1;
  n = img.w * img.h * 3;
  for (i = 0; i < n; i++)
    sum += img.rgb[i];
  free (img.rgb);
  return (int) (sum / n);
}

static void test_edited_thumbnail (const char *work)
{
  char path[700], libdir[700], err[256], text[256];
  library *l;
  cat_item *items;
  int n, before, after;
  cat_source all = { SRC_ALL, 0, 0, "" };
  recipe r;
  snprintf (path, sizeof path, "%s/grey.jpg", work);
  make_jpeg (path, 400, 300, 128);
  snprintf (libdir, sizeof libdir, "%s/Library", work);
  lib_open (&l, libdir, err, sizeof err);
  lib_import (l, path);
  while (lib_busy (l))
    lib_work (l, now, clk + 5, NULL, NULL);
  cat_list (lib_cat (l), &all, &items, &n);
  CHECK (n == 1 && !items[0].edited, "one photo, not edited");
  before = mean_of_thumb (l, items[0].id);

  recipe_default (&r);
  recipe_set (&r, P_EV, -2);
  recipe_format (&r, text, sizeof text);
  CHECK (cat_set_recipe (lib_cat (l), items[0].id, text) == 0, "recipe saved");
  lib_thumbs_changed (l);
  CHECK (lib_busy (l), "its thumbnail is to be made again");
  while (lib_busy (l))
    lib_work (l, now, clk + 5, NULL, NULL);
  after = mean_of_thumb (l, items[0].id);
  CHECK (before > 120 && after < before / 2 + 10 && after > 20, "the new thumbnail shows -2 EV");
  {
    char back[256];
    cat_get_recipe (lib_cat (l), items[0].id, back, sizeof back);
    CHECK (strcmp (back, text) == 0, "the recipe reads back");
  }
  free (items);
  cat_list (lib_cat (l), &all, &items, &n);
  CHECK (n == 1 && items[0].edited && items[0].thumb_state == THUMB_READY, "listed as edited, thumbnail ready");
  free (items);
  lib_close (l);
}


/* ---- geometry ---- */

/* A test card: each pixel's channels say where it came from (x, y), so a
   transform can be checked by reading back the coordinates. */
static uint16_t *card (int w, int h)
{
  uint16_t *p = malloc ((size_t) w * h * 6);
  int x, y;
  for (y = 0; y < h; y++)
    for (x = 0; x < w; x++)
      {
        uint16_t *q = p + ((size_t) y * w + x) * 3;
        q[0] = (uint16_t) (x * 65535 / (w - 1));
        q[1] = (uint16_t) (y * 65535 / (h - 1));
        q[2] = 30000;
      }
  return p;
}

static void at (const uint16_t *img, int w, int x, int y, double *fx, double *fy)
{
  const uint16_t *q = img + ((size_t) y * w + x) * 3;
  *fx = q[0] / 65535.0;
  *fy = q[1] / 65535.0;
}

static void test_geometry (void)
{
  int w = 120, h = 80;
  uint16_t *src = card (w, h), *dst = malloc ((size_t) w * h * 6 * 2);
  recipe r;
  geom_plan g;
  double fx, fy;
  int x, y, worst = 0;

  recipe_default (&r);
  geom_plan_make (&g, &r, w, h, 1, 0, 0);
  CHECK (g.ow == w && g.oh == h && fabs (g.zoom - 1) < 0.01, "identity: same size, no zoom");
  geom_rows (&g, src, dst, 0, g.oh);
  for (y = 0; y < h; y++)
    for (x = 0; x < w; x++)
      {
        int c;
        for (c = 0; c < 3; c++)
          {
            int d = abs ((int) dst[((size_t) y * w + x) * 3 + c] - (int) src[((size_t) y * w + x) * 3 + c]);
            if (d > worst) worst = d;
          }
      }
  CHECK (worst <= 64, "identity: every pixel back where it was");

  recipe_set (&r, P_ROTATE, 1);
  geom_plan_make (&g, &r, w, h, 1, 0, 0);
  CHECK (g.ow == h && g.oh == w, "a quarter turn swaps width and height");
  geom_rows (&g, src, dst, 0, g.oh);
  at (dst, g.ow, g.ow - 1, 0, &fx, &fy);
  CHECK (fx < 0.03 && fy < 0.03, "90 clockwise: the top left goes to the top right");
  at (dst, g.ow, 0, g.oh - 1, &fx, &fy);
  CHECK (fx > 0.97 && fy > 0.97, "and the bottom right to the bottom left");
  recipe_set (&r, P_ROTATE, 2);
  geom_plan_make (&g, &r, w, h, 1, 0, 0);
  geom_rows (&g, src, dst, 0, g.oh);
  at (dst, g.ow, 0, 0, &fx, &fy);
  CHECK (g.ow == w && fx > 0.97 && fy > 0.97, "180: the bottom right comes to the top left");
  recipe_set (&r, P_ROTATE, 3);
  geom_plan_make (&g, &r, w, h, 1, 0, 0);
  geom_rows (&g, src, dst, 0, g.oh);
  at (dst, g.ow, 0, 0, &fx, &fy);
  CHECK (fx > 0.97 && fy < 0.03, "90 anticlockwise: the top right comes to the top left");

  recipe_default (&r);
  recipe_set (&r, P_CROP_X0, 0.25f); recipe_set (&r, P_CROP_X1, 0.75f);
  recipe_set (&r, P_CROP_Y0, 0.5f);
  geom_plan_make (&g, &r, w, h, 1, 0, 0);
  CHECK (g.ow == 60 && g.oh == 40, "crop: half the width, the bottom half");
  geom_rows (&g, src, dst, 0, g.oh);
  at (dst, g.ow, 0, 0, &fx, &fy);
  CHECK (fabs (fx - 0.25) < 0.02 && fabs (fy - 0.5) < 0.02, "crop: starts at a quarter across, half down");
  geom_plan_make (&g, &r, w, h, 0, 0, 0);
  CHECK (g.ow == w && g.oh == h, "the crop tool sees the whole picture");

  /* Straightening: zooms in so that no corner reaches outside the picture. */
  recipe_default (&r);
  recipe_set (&r, P_ANGLE, 5);
  geom_plan_make (&g, &r, w, h, 1, 0, 0);
  CHECK (g.zoom > 1.05 && g.zoom < 1.3, "5 degrees: zooms in a little");
  {
    double corners[4][2] = { { 0, 0 }, { w, 0 }, { 0, h }, { w, h } }, sx, sy;
    int k, inside = 1;
    for (k = 0; k < 4; k++)
      {
        geom_to_source (&g, corners[k][0], corners[k][1], &sx, &sy);
        if (sx < -0.01 || sy < -0.01 || sx > w + 0.01 || sy > h + 0.01)
          inside = 0;
      }
    CHECK (inside, "every corner comes from inside the picture");
    geom_to_source (&g, w / 2.0, h / 2.0, &sx, &sy);
    CHECK (fabs (sx - w / 2.0) < 0.01 && fabs (sy - h / 2.0) < 0.01, "the centre stays put");
    /* Clockwise: a point right of centre moves down. */
    geom_to_source (&g, w / 2.0, h / 2.0 + 10, &sx, &sy);
    CHECK (sx > w / 2.0, "turned clockwise (output below centre came from the right of it)");
  }
  recipe_set (&r, P_ANGLE, -15);
  geom_plan_make (&g, &r, w, h, 1, 0, 0);
  {
    double sx, sy;
    int k, inside = 1;
    for (k = 0; k <= 10; k++)
      {
        geom_to_source (&g, k * w / 10.0, 0, &sx, &sy);
        if (sx < -0.01 || sy < -0.01 || sx > w + 0.01 || sy > h + 0.01) inside = 0;
        geom_to_source (&g, k * w / 10.0, h, &sx, &sy);
        if (sx < -0.01 || sy < -0.01 || sx > w + 0.01 || sy > h + 0.01) inside = 0;
      }
    CHECK (inside, "-15 degrees: the whole edge comes from inside the picture");
  }

  /* Perspective: +vertical widens the top, so a point near the top edge
     samples nearer the centre than it would otherwise. */
  recipe_default (&r);
  recipe_set (&r, P_PERSP_V, 60);
  geom_plan_make (&g, &r, w, h, 1, 0, 0);
  {
    double tx, ty, bx, by, sx2, sy2;
    geom_to_source (&g, w * 0.9, 1, &tx, &ty);
    geom_to_source (&g, w * 0.9, h - 1, &bx, &by);
    CHECK (fabs (tx - w / 2.0) < fabs (bx - w / 2.0), "vertical +: the top is spread wider than the bottom");
    geom_to_source (&g, 0, 0, &sx2, &sy2);
    CHECK (sx2 >= -0.01 && sy2 >= -0.01, "and the corners still come from inside");
  }
  recipe_default (&r);
  recipe_set (&r, P_PERSP_H, 60);
  geom_plan_make (&g, &r, w, h, 1, 0, 0);
  {
    double lx, ly, rx, ry;
    geom_to_source (&g, 1, h * 0.9, &lx, &ly);
    geom_to_source (&g, w - 1, h * 0.9, &rx, &ry);
    CHECK (fabs (ly - h / 2.0) > fabs (ry - h / 2.0), "horizontal +: the right is spread wider than the left");
  }

  /* Aspect ratios for the crop tool. */
  {
    double x0 = 0, y0 = 0, x1 = 1, y1 = 1;
    geom_crop_aspect (&x0, &y0, &x1, &y1, 1.0, 300, 200);
    CHECK (fabs ((x1 - x0) * 300 - (y1 - y0) * 200) < 0.5 && fabs (y1 - y0 - 1) < 1e-9, "1:1 on 3:2: full height, centred square");
    CHECK (fabs ((x0 + x1) / 2 - 0.5) < 1e-9, "centred");
    x0 = 0.8; x1 = 1; y0 = 0; y1 = 1;
    geom_crop_aspect (&x0, &y0, &x1, &y1, 16.0 / 9, 300, 200);
    CHECK (x1 <= 1 + 1e-9 && x0 >= -1e-9 && fabs ((x1 - x0) * 300 / ((y1 - y0) * 200) - 16.0 / 9) < 0.01, "16:9 stays inside");
  }
  free (src);
  free (dst);
}

/* Develops a whole 8-bit image through a recipe with geometry. */
static void test_develop_image (void)
{
  rgb_image img;
  recipe r;
  int x;
  img.w = 40; img.h = 20;
  img.rgb = malloc (40 * 20 * 3);
  for (x = 0; x < 40 * 20; x++)
    {
      img.rgb[3 * x] = (unsigned char) (x % 40 < 20 ? 200 : 30);
      img.rgb[3 * x + 1] = 60;
      img.rgb[3 * x + 2] = 60;
    }
  recipe_default (&r);
  recipe_set (&r, P_ROTATE, 1);
  recipe_set (&r, P_EV, 1);
  CHECK (develop_image (&r, &img) == 0 && img.w == 20 && img.h == 40, "develop_image: turned, and the size follows");
  CHECK (img.rgb[0] > 220 && img.rgb[(19 * 20 + 19) * 3] > 220, "the red left half is now the top half, brighter");
  CHECK (img.rgb[(20 * 20) * 3] < 120 && img.rgb[(39 * 20 + 19) * 3] < 120, "and the dark half the bottom");
  free (img.rgb);
}

/* ---- auto adjust and white balance ---- */

static uint16_t *scene (int w, int h, double cast_r, double cast_g, double cast_b, double light)
{
  uint16_t *p = malloc ((size_t) w * h * 6);
  int x, y;
  const uint16_t *s2l = dev_srgb_to_linear ();
  for (y = 0; y < h; y++)
    for (x = 0; x < w; x++)
      {
        /* Grey ramps (half of it neutral), some colour patches. */
        double v = (double) x / w, r, g, b;
        if (y < h / 2)
          r = g = b = v;
        else
          {
            r = 0.5 + 0.4 * sin (x * 0.1); g = 0.45; b = 0.4 + 0.3 * cos (y * 0.07);
            r *= v + 0.2; g *= v + 0.2; b *= v + 0.2;
          }
        r = s2l[(int) (fmin (r, 1) * 255)] / 65535.0 * cast_r * light;
        g = s2l[(int) (fmin (g, 1) * 255)] / 65535.0 * cast_g * light;
        b = s2l[(int) (fmin (b, 1) * 255)] / 65535.0 * cast_b * light;
        p[((size_t) y * w + x) * 3] = (uint16_t) (fmin (r, 1) * 65535);
        p[((size_t) y * w + x) * 3 + 1] = (uint16_t) (fmin (g, 1) * 65535);
        p[((size_t) y * w + x) * 3 + 2] = (uint16_t) (fmin (b, 1) * 65535);
      }
  return p;
}

static double cast_of (const recipe *r, double R, double G, double B)
{
  /* How far from neutral a grey with this cast is after r's white balance. */
  static develop d;
  uint16_t in[3];
  uint32_t px;
  int o[3];
  in[0] = (uint16_t) (R * 65535); in[1] = (uint16_t) (G * 65535); in[2] = (uint16_t) (B * 65535);
  develop_prepare (&d, r);
  develop_rows (&d, in, 1, 0, 1, &px, NULL);
  o[0] = (int) (px & 255); o[1] = (int) (px >> 8 & 255); o[2] = (int) (px >> 16 & 255);
  return abs (o[0] - o[1]) + abs (o[2] - o[1]);
}

static void test_auto (void)
{
  int w = 400, h = 300;
  recipe r;
  float t, n;
  double rgb[3];
  uint16_t *warm = scene (w, h, 1.0, 0.85, 0.62, 1.0);
  uint16_t *dark = scene (w, h, 1, 1, 1, 0.18);
  uint16_t *fine = scene (w, h, 1, 1, 1, 1.0);

  /* The eyedropper: a picked grey becomes exactly neutral. */
  CHECK (wb_neutral (0.30, 0.25, 0.18, &t, &n) == 0, "wb_neutral solves");
  recipe_default (&r);
  recipe_set (&r, P_TEMP, t); recipe_set (&r, P_TINT, n);
  CHECK (t < -10, "a warm grey needs a cooler temperature");
  CHECK (cast_of (&r, 0.30, 0.25, 0.18) <= 3, "the picked grey comes out neutral (within 3 levels)");
  wb_sample (warm, w, h, 50, 10, 2, rgb);
  CHECK (rgb[0] > rgb[1] && rgb[1] > rgb[2], "sampling the warm card's grey: red > green > blue");
  CHECK (wb_neutral (1, 1, 1, &t, &n) == 0 && fabsf (t) < 0.01f && fabsf (n) < 0.01f, "a neutral grey needs nothing");
  CHECK (wb_neutral (0, 0.5, 0.5, &t, &n) == -1, "black can't be balanced");

  /* Auto white balance takes most of a cast away. */
  recipe_default (&r);
  auto_white_balance (warm, w, h, &t, &n);
  recipe_set (&r, P_TEMP, t); recipe_set (&r, P_TINT, n);
  {
    double before, after;
    recipe z;
    recipe_default (&z);
    before = cast_of (&z, 0.40, 0.34, 0.25);
    after = cast_of (&r, 0.40, 0.34, 0.25);
    CHECK (t < 0 && after < before * 0.45, "auto white balance: the warm cast mostly goes");
  }
  auto_white_balance (fine, w, h, &t, &n);
  CHECK (fabsf (t) <= 2 && fabsf (n) <= 2, "a neutral picture's white balance is left alone");

  /* Auto adjust: a dark picture is brightened; a good one barely changes. */
  recipe_default (&r);
  recipe_set (&r, P_ROTATE, 1);
  recipe_set (&r, P_CROP_X0, 0.1f);
  auto_adjust (dark, w, h, &r);
  CHECK (r.v[P_EV] > 0.7f, "auto: a dark picture gets more exposure");
  CHECK (r.v[P_ROTATE] == 1 && fabsf (r.v[P_CROP_X0] - 0.1f) < 1e-4f, "auto leaves the geometry alone");
  recipe_default (&r);
  auto_adjust (fine, w, h, &r);
  CHECK (fabsf (r.v[P_EV]) < 0.8f, "auto: a well-exposed picture keeps its exposure, near enough");
  CHECK (r.v[P_CONTRAST] >= 0 && r.v[P_CONTRAST] <= 35 && r.v[P_BLACK] <= 6 && r.v[P_WHITE] >= 92, "auto stays in sensible ranges");
  recipe_default (&r);
  auto_adjust (warm, w, h, &r);
  CHECK (r.v[P_TEMP] < -5, "auto adjust includes the white balance");
  free (warm); free (dark); free (fine);
}

static void test_presets (const char *work)
{
  char libdir[700], err[256], text[1024];
  library *l;
  cat_preset *ps;
  int np;
  recipe a;
  snprintf (libdir, sizeof libdir, "%s/Library", work);
  lib_open (&l, libdir, err, sizeof err);
  recipe_default (&a);
  recipe_set (&a, P_TEMP, 15); recipe_set (&a, P_CONTRAST, 20); recipe_set (&a, P_ROTATE, 1);
  recipe_format_look (&a, text, sizeof text);
  CHECK (strstr (text, "geometry") == NULL && strstr (text, "temp=15") != NULL, "a preset holds the look, not the geometry");
  CHECK (cat_preset_save (lib_cat (l), "Warm punch", text) > 0, "preset saved");
  CHECK (cat_preset_save (lib_cat (l), "Cool", "wb temp=-20 tint=0\n") > 0, "a second preset");
  recipe_set (&a, P_CONTRAST, 30);
  recipe_format_look (&a, text, sizeof text);
  cat_preset_save (lib_cat (l), "Warm punch", text);
  cat_presets (lib_cat (l), &ps, &np);
  CHECK (np == 2 && strcmp (ps[0].name, "Cool") == 0 && strstr (ps[1].recipe, "contrast=30") != NULL,
         "saving under the same name replaces it; sorted by name");
  {
    cat_item *items;
    int n;
    cat_source all = { SRC_ALL, 0, 0, "" };
    recipe got;
    cat_list (lib_cat (l), &all, &items, &n);
    CHECK (n == 1, "the library from the thumbnail test");
    /* The photo has its own crop; the preset must keep it. */
    cat_set_recipe (lib_cat (l), items[0].id, "crop x0=0.100 y0=0.000 x1=1.000 y1=1.000\n");
    CHECK (cat_apply_look (lib_cat (l), &items[0].id, 1, ps[1].recipe) == 0, "preset applied");
    cat_get_recipe (lib_cat (l), items[0].id, text, sizeof text);
    recipe_parse (&got, text);
    CHECK (got.v[P_TEMP] == 15 && got.v[P_CONTRAST] == 30 && fabsf (got.v[P_CROP_X0] - 0.1f) < 1e-4f,
           "the look arrives, the photo's own crop stays");
    CHECK (cat_preset_delete (lib_cat (l), ps[0].id) == 0, "preset deleted");
    free (items);
  }
  free (ps);
  cat_presets (lib_cat (l), &ps, &np);
  CHECK (np == 1, "one left");
  free (ps);
  lib_close (l);
}

/* ---- resampling for the screen ---- */
static void test_resample (void)
{
  int sw = 300, sh = 200, x, y, worst, lo, hi;
  uint32_t *src = malloc ((size_t) sw * sh * 4), *dst = malloc ((size_t) 600 * 400 * 4);
  double ms = 0, md = 0;
  /* A one-pixel black and white checkerboard: sampling would give black,
     white or moire; averaging gives mid grey. */
  for (y = 0; y < sh; y++)
    for (x = 0; x < sw; x++)
      src[y * sw + x] = (x + y) & 1 ? 0xFFFFFF : 0;
  CHECK (resample32 (src, sw, sh, dst, 150, 100) == 0, "halve");
  for (lo = 255, hi = 0, x = 0; x < 150 * 100; x++)
    {
      int v = (int) (dst[x] & 255);
      if (x % 150 == 0 || x % 150 == 149 || x < 150 || x >= 150 * 99)
        continue;                      /* the edges have only half a filter */
      if (v < lo) lo = v;
      if (v > hi) hi = v;
    }
  CHECK (lo >= 127 && hi <= 128, "a checkerboard halved is flat mid grey");
  resample32 (src, sw, sh, dst, 211, 137);
  for (lo = 255, hi = 0, x = 0; x < 211 * 137; x++)
    {
      int v = (int) (dst[x] >> 8 & 255);
      if (x % 211 == 0 || x % 211 == 210 || x < 211 || x >= 211 * 136)
        continue;
      if (v < lo) lo = v;
      if (v > hi) hi = v;
    }
    CHECK (lo >= 118 && hi <= 137, "and at an odd ratio (300x200 to 211x137) too: no moire");
  /* Average brightness is kept, channels stay apart. */
  for (y = 0; y < sh; y++)
    for (x = 0; x < sw; x++)
      {
        unsigned r = (unsigned) ((x * 7 + y * 3) % 256), g = (unsigned) (x % 200), b = 40;
        src[y * sw + x] = r | g << 8 | b << 16;
        ms += r;
      }
  ms /= sw * sh;
  resample32 (src, sw, sh, dst, 97, 61);
  for (worst = 0, x = 0; x < 97 * 61; x++)
    {
      md += dst[x] & 255;
      if (abs ((int) (dst[x] >> 16 & 255) - 40) > worst)
        worst = abs ((int) (dst[x] >> 16 & 255) - 40);
    }
  md /= 97 * 61;
  CHECK (fabs (md - ms) < 1.5 && worst == 0, "shrinking keeps the mean, and a flat channel exact");
  /* Enlarging: bilinear, ends kept, smooth in between. */
  src[0] = 0; src[1] = 200; 
  CHECK (resample32 (src, 2, 1, dst, 8, 1) == 0, "enlarge 2 to 8");
  CHECK ((dst[0] & 255) == 0 && (dst[7] & 255) == 200, "the ends stay");
  for (worst = 1, x = 1; x < 8; x++)
    if ((dst[x] & 255) < (dst[x - 1] & 255))
      worst = 0;
  CHECK (worst && (dst[3] & 255) > 50 && (dst[3] & 255) < 150, "and rise smoothly between");
  CHECK (resample32 (src, 300, 200, dst, 300, 200) == 0 && memcmp (src, dst, 300 * 200 * 4) == 0, "same size: a copy");
  free (src);
  free (dst);
}

/* ---- export ---- */
static int run_export (exporter *e)
{
  int r, steps = 0;
  while ((r = export_step (e, 7)) == 0)
    steps++;
  return r == 1 ? steps : -1;
}

static int read_back (const char *path, rgb_image *img)
{
  char err[200];
  thumbgen *t = thumbgen_start (path, NULL, 1 << 20, 1, err, sizeof err);
  int r;
  if (!t)
    return -1;
  while ((r = thumbgen_step (t, 64)) == 0)
    ;
  r = r == 1 ? thumbgen_take (t, img) : -1;
  thumbgen_free (t);
  return r;
}

/* --- EXIF carried into exports */
static unsigned char *mm;              /* a big-endian TIFF being built */
static size_t mm_len;
static void be16 (size_t at, unsigned v) { mm[at] = (unsigned char) (v >> 8); mm[at + 1] = (unsigned char) v; }
static void be32 (size_t at, unsigned long v) { be16 (at, (unsigned) (v >> 16)); be16 (at + 2, (unsigned) (v & 0xFFFF)); }
/* An entry; values over 4 bytes go at *data. */
static void be_entry (size_t at, unsigned tag, unsigned type, unsigned long count, const unsigned char *val, size_t len,
                      size_t *data)
{
  be16 (at, tag); be16 (at + 2, type); be32 (at + 4, count);
  memset (mm + at + 8, 0, 4);
  if (len <= 4)
    memcpy (mm + at + 8, val, len);
  else
    {
      be32 (at + 8, (unsigned long) *data);
      memcpy (mm + *data, val, len);
      *data += (len + 1) & ~(size_t) 1;
    }
  if (*data > mm_len)
    mm_len = *data;
}

/* Motorola-order EXIF, as a Nikon writes it: IFD0 (with a raw strip
   tag and orientation 6), Exif (maker's notes, interop pointer, size),
   GPS, and IFD1 with a thumbnail. */
static size_t make_mm_exif (unsigned char *buf)
{
  size_t d = 600, i0 = 8, ie = 200, ig = 400, i1 = 500;
  unsigned char v[64];
  mm = buf; mm_len = 0;
  memset (buf, 0, 4096);
  memcpy (buf, "MM\0\x2A", 4); be32 (4, i0);
  be16 (i0, 7);
  be_entry (i0 + 2, 0x010F, 2, 6, (const unsigned char *) "NIKON", 6, &d);
  be_entry (i0 + 14, 0x0110, 2, 9, (const unsigned char *) "NIKON Z6", 9, &d);
  v[0] = 0; v[1] = 6; be_entry (i0 + 26, 0x0112, 3, 1, v, 2, &d);
  be_entry (i0 + 38, 0x0111, 4, 1, (const unsigned char *) "\0\0\x10\0", 4, &d);   /* a strip: left out */
  be_entry (i0 + 50, 0x8298, 2, 10, (const unsigned char *) "(c) Andy", 9, &d);
  be_entry (i0 + 62, 0x8769, 4, 1, (const unsigned char *) "\0\0\0\xC8", 4, &d);   /* Exif at 200 */
  be_entry (i0 + 74, 0x8825, 4, 1, (const unsigned char *) "\0\0\x01\x90", 4, &d); /* GPS at 400 */
  be32 (i0 + 86, i1);
  be16 (ie, 7);
  memcpy (v, "\0\0\0\x01\0\0\0\xFA", 8); be_entry (ie + 2, 0x829A, 5, 1, v, 8, &d);   /* 1/250 */
  memcpy (v, "\0\0\0\x38\0\0\0\x0A", 8); be_entry (ie + 14, 0x829D, 5, 1, v, 8, &d);  /* f/5.6 */
  v[0] = 0x01; v[1] = 0x90; be_entry (ie + 26, 0x8827, 3, 1, v, 2, &d);               /* ISO 400 */
  be_entry (ie + 38, 0x9003, 2, 20, (const unsigned char *) "2024:06:01 10:20:30", 20, &d);
  be_entry (ie + 50, 0x927C, 7, 12, (const unsigned char *) "Nikon\0\x02\x10\0\0MM", 12, &d);
  v[0] = 0xFF; v[1] = 0xFF; be_entry (ie + 62, 0xA001, 3, 1, v, 2, &d);               /* uncalibrated */
  be_entry (ie + 74, 0xA002, 4, 1, (const unsigned char *) "\0\0\x0F\xA0", 4, &d);  /* 4000 */
  be32 (ie + 86, 0);
  be16 (ig, 2);
  be_entry (ig + 2, 0x0001, 2, 2, (const unsigned char *) "N", 2, &d);
  memcpy (v, "\0\0\0\x36\0\0\0\x01\0\0\0\x1E\0\0\0\x01\0\0\0\0\0\0\0\x01", 24);
  be_entry (ig + 14, 0x0002, 5, 3, v, 24, &d);                                        /* 54 30 0 */
  be32 (ig + 26, 0);
  be16 (i1, 1);
  be_entry (i1 + 2, 0x0201, 4, 1, (const unsigned char *) "\0\0\x09\0", 4, &d);
  be32 (i1 + 14, 0);
  return mm_len > 700 ? mm_len : 700;
}

static char exif_seen[4096];
static void collect (void *ctx, const char *group, const char *name, const char *value)
{
  size_t l = strlen (exif_seen);
  (void) ctx;
  snprintf (exif_seen + l, sizeof exif_seen - l, "%s|%s|%s\n", group, name, value);
}

/* The EXIF of a file Lumen wrote: listed into exif_seen. */
static int exif_of (const char *path, int png)
{
  static unsigned char buf[300000];
  size_t got, off, n;
  FILE *f = fopen (path, "rb");
  exif_seen[0] = '\0';
  if (!f)
    return -1;
  got = fread (buf, 1, sizeof buf, f);
  fclose (f);
  if (png)
    {
      /* The eXIf chunk. */
      size_t p = 8;
      while (p + 12 <= got)
        {
          size_t len = (size_t) buf[p] << 24 | (size_t) buf[p + 1] << 16 | (size_t) buf[p + 2] << 8 | buf[p + 3];
          if (!memcmp (buf + p + 4, "eXIf", 4))
            return exif_list_tiff (buf + p + 8, len, collect, NULL);
          p += 12 + len;
        }
      return -1;
    }
  if (exif_find_tiff (buf, got, &off, &n))
    return -1;
  CHECK (off == 12, "EXIF's APP1 straight after SOI (no JFIF first)");
  return exif_list_tiff (buf + off, n, collect, NULL);
}

static void test_export_exif (const char *work)
{
  static unsigned char exif[4096], file[400000];
  char src[700], dst[700], err[200];
  rgb_image img;
  recipe r;
  export_opts o = { EXPORT_JPEG, 90, 0, "Lumen 9.9" };
  exporter *e;
  size_t n, got;
  FILE *f;
  img_info known;
  img.w = 120; img.h = 80;
  img.rgb = calloc (120 * 80, 3);
  snprintf (src, sizeof src, "%s/exif-src.jpg", work);
  CHECK (img_save_jpeg (&img, src, 90, err, sizeof err) == 0, "exif: a source");
  free (img.rgb);
  /* Splice an APP1 Exif segment in after SOI. */
  n = make_mm_exif (exif);
  f = fopen (src, "rb");
  got = f ? fread (file, 1, sizeof file, f) : 0;
  if (f)
    fclose (f);
  f = fopen (src, "wb");
  if (f)
    {
      unsigned char seg[10];
      seg[0] = 0xFF; seg[1] = 0xE1; seg[2] = (unsigned char) ((n + 8) >> 8); seg[3] = (unsigned char) (n + 8);
      memcpy (seg + 4, "Exif\0\0", 6);
      fwrite (file, 1, 2, f);
      fwrite (seg, 1, 10, f);
      fwrite (exif, 1, n, f);
      fwrite (file + 2, 1, got - 2, f);
      fclose (f);
    }
  recipe_default (&r);
  recipe_set (&r, P_ROTATE, 1);        /* a quarter turn: 80 x 120 out */
  snprintf (dst, sizeof dst, "%s/exif-out.jpg", work);
  e = export_start (src, 120, 80, 1, &r, dst, &o, err, sizeof err);
  if (e)
    run_export (e);
  export_free (e);
  CHECK (exif_of (dst, 0) > 0, "exif: the export has EXIF");
  CHECK (strstr (exif_seen, "Camera|Make|NIKON\n") && strstr (exif_seen, "Model|NIKON Z6\n"), "exif: camera kept");
  CHECK (strstr (exif_seen, "|Copyright|(c) Andy\n") != NULL, "exif: copyright kept");
  CHECK (strstr (exif_seen, "Orientation|Normal") != NULL && !strstr (exif_seen, "90"), "exif: orientation 1 (it is turned already)");
  CHECK (strstr (exif_seen, "Software|Lumen 9.9\n") != NULL, "exif: Software is Lumen");
  CHECK (strstr (exif_seen, "1/250 s") && strstr (exif_seen, "f/5.6") && strstr (exif_seen, "|400\n"),
         "exif: exposure, aperture, ISO kept");
  CHECK (strstr (exif_seen, "2024:06:01 10:20:30") != NULL, "exif: date kept");
  CHECK (strstr (exif_seen, "GPS|") != NULL && strstr (exif_seen, "54") != NULL, "exif: GPS kept");
  CHECK (!strstr (exif_seen, "Maker") && !strstr (exif_seen, "4000") && !strstr (exif_seen, "Uncalibrated"),
         "exif: no maker's notes, old size or uncalibrated colour");
  CHECK (strstr (exif_seen, "sRGB") != NULL, "exif: colour space sRGB");
  CHECK (strstr (exif_seen, "|80\n") && strstr (exif_seen, "|120\n"), "exif: the export's own size");
  /* PNG: the same, in eXIf. */
  o.format = EXPORT_PNG;
  snprintf (dst, sizeof dst, "%s/exif-out.png", work);
  e = export_start (src, 120, 80, 1, &r, dst, &o, err, sizeof err);
  if (e)
    run_export (e);
  export_free (e);
  CHECK (exif_of (dst, 1) > 0 && strstr (exif_seen, "Model|NIKON Z6\n"), "exif: in a PNG's eXIf chunk");
  /* No EXIF in the original: from what the library knows. */
  o.format = EXPORT_JPEG;
  snprintf (src, sizeof src, "%s/source.jpg", work);   /* test_export's, plain */
  snprintf (dst, sizeof dst, "%s/exif-known.jpg", work);
  memset (&known, 0, sizeof known);
  snprintf (known.make, sizeof known.make, "Canon");
  snprintf (known.model, sizeof known.model, "Canon EOS R5");
  snprintf (known.taken, sizeof known.taken, "2025:01:02 03:04:05");
  known.exposure = 0.004; known.fnumber = 2.8; known.iso = 3200; known.focal = 50;
  e = export_start (src, 400, 300, 1, &r, dst, &o, err, sizeof err);
  if (e)
    {
      export_set_known (e, &known);
      run_export (e);
    }
  export_free (e);
  CHECK (exif_of (dst, 0) > 0 && strstr (exif_seen, "Model|Canon EOS R5\n") && strstr (exif_seen, "1/250 s")
         && strstr (exif_seen, "f/2.8") && strstr (exif_seen, "3200") && strstr (exif_seen, "2025:01:02 03:04:05"),
         "exif: made from the library's details");
  /* Nothing known, no EXIF: none written (and JFIF as before). */
  snprintf (dst, sizeof dst, "%s/exif-none.jpg", work);
  e = export_start (src, 400, 300, 1, &r, dst, &o, err, sizeof err);
  if (e)
    run_export (e);
  export_free (e);
  f = fopen (dst, "rb");
  got = f ? fread (file, 1, 20, f) : 0;
  if (f)
    fclose (f);
  CHECK (got == 20 && file[3] == 0xE0 && !memcmp (file + 6, "JFIF", 4), "exif: none to write: a plain JFIF file");
  /* Builder limits: rubbish in, nothing out. */
  CHECK (exif_for_export ((const unsigned char *) "XXXXXXXXXXXX", 12, NULL, 10, 10, "", 1, exif, sizeof exif) == 0,
         "exif: not TIFF, nothing known: nothing");
}

static void test_export (const char *work)
{
  char src[700], dst[700], err[200];
  rgb_image img, back;
  recipe r;
  export_opts o = { EXPORT_JPEG, 92, 0 };
  exporter *e;
  int x, y, steps, worst = 0;
  FILE *f;
  img.w = 400; img.h = 300;
  img.rgb = malloc (400 * 300 * 3);
  for (y = 0; y < 300; y++)
    for (x = 0; x < 400; x++)
      {
        unsigned char *p = img.rgb + (y * 400 + x) * 3;
        int red = x < 100 && y < 100;
        p[0] = (unsigned char) (red ? 220 : 40 + x / 3);
        p[1] = (unsigned char) (red ? 30 : 60 + y / 3);
        p[2] = (unsigned char) (red ? 30 : 90);
      }
  snprintf (src, sizeof src, "%s/source.jpg", work);
  CHECK (img_save_jpeg (&img, src, 95, err, sizeof err) == 0, "a 400x300 source");

  /* Untouched, full size: the same picture back. */
  recipe_default (&r);
  snprintf (dst, sizeof dst, "%s/out-full.jpg", work);
  e = export_start (src, 400, 300, 1, &r, dst, &o, err, sizeof err);
  CHECK (e != NULL, "export started");
  steps = e ? run_export (e) : -1;
  CHECK (steps > 10, "in many small steps");
  CHECK (e && export_progress (e) == 100, "progress reaches 100");
  export_free (e);
  CHECK (read_back (dst, &back) == 0 && back.w == 400 && back.h == 300, "written at 400x300");
  for (y = 0; y < 300; y += 7)
    for (x = 0; x < 400; x += 7)
      {
        int c;
        for (c = 0; c < 3; c++)
          {
            int d = abs (back.rgb[(y * 400 + x) * 3 + c] - img.rgb[(y * 400 + x) * 3 + c]);
            if (d > worst) worst = d;
          }
      }
  CHECK (worst <= 12, "pixels as the original (within JPEG's error)");
  free (back.rgb);

  /* Smaller, brighter. */
  o.max_side = 100;
  recipe_set (&r, P_EV, 1);
  snprintf (dst, sizeof dst, "%s/out-small.jpg", work);
  e = export_start (src, 400, 300, 1, &r, dst, &o, err, sizeof err);
  run_export (e);
  export_free (e);
  CHECK (read_back (dst, &back) == 0 && back.w == 100 && back.h == 75, "max 100: 100x75");
  CHECK (back.rgb[(70 * 100 + 90) * 3 + 1] > img.rgb[(280 * 400 + 360) * 3 + 1] + 20, "and brighter (+1 EV)");
  free (back.rgb);

  /* Turned, cropped, as PNG. */
  recipe_default (&r);
  recipe_set (&r, P_ROTATE, 1);
  recipe_set (&r, P_CROP_X1, 0.5f);
  o.format = EXPORT_PNG;
  o.max_side = 0;
  snprintf (dst, sizeof dst, "%s/out-turned.png", work);
  e = export_start (src, 400, 300, 1, &r, dst, &o, err, sizeof err);
  CHECK (run_export (e) >= 0, "PNG export");
  export_free (e);
  {
    unsigned char head[8] = { 0 };
    f = fopen (dst, "rb");
    if (f) { if (fread (head, 1, 8, f) != 8) head[0] = 0; fclose (f); }
    CHECK (head[1] == 'P' && head[2] == 'N' && head[3] == 'G', "a PNG file");
  }
  CHECK (read_back (dst, &back) == 0 && back.w == 150 && back.h == 400, "turned (300 wide) and half cropped: 150x400");
  /* Turned clockwise: the red top left corner is now at the top right,
     which the crop (left half) leaves out; the top left is from the bottom left. */
  CHECK (back.rgb[0] < 120 && back.rgb[(5 * 150 + 5) * 3] < 120, "the red corner went with the turn");
  free (back.rgb);

  /* Highlights & Shadows, turned and cropped as PNG: the same as the whole
     recipe on the decoded picture (edited thumbnails' way), though export
     works in bands. */
  {
    rgb_image dec;
    char e2[200];
    thumbgen *t = thumbgen_start (src, NULL, 1 << 30, 1, e2, sizeof e2);
    int r2, diff = 0;
    while (t && (r2 = thumbgen_step (t, 64)) == 0)
      ;
    CHECK (t && thumbgen_take (t, &dec) == 0, "the source decoded");
    thumbgen_free (t);
    recipe_default (&r);
    recipe_set (&r, P_SHADOWS, 70);
    recipe_set (&r, P_HIGHLIGHTS, 40);
    recipe_set (&r, P_ROTATE, 1);
    recipe_set (&r, P_CROP_X1, 0.5f);
    o.format = EXPORT_PNG;
    o.max_side = 0;
    snprintf (dst, sizeof dst, "%s/out-hs.png", work);
    e = export_start (src, 400, 300, 1, &r, dst, &o, err, sizeof err);
    CHECK (run_export (e) >= 0, "export with Highlights & Shadows");
    export_free (e);
    CHECK (develop_image (&r, &dec) == 0, "developed whole");
    CHECK (read_back (dst, &back) == 0 && back.w == dec.w && back.h == dec.h, "the same size");
    for (y = 0; y < back.h; y++)
      for (x = 0; x < back.w * 3; x++)
        {
          int d = abs (back.rgb[y * back.w * 3 + x] - dec.rgb[y * dec.w * 3 + x]);
          if (d > diff) diff = d;
        }
    CHECK (diff <= 1, "export and thumbnail agree, pixel for pixel");
    free (back.rgb);
    free (dec.rgb);
  }

  /* Failures. */
  o.format = EXPORT_JPEG;
  CHECK (export_start ("/nonexistent/x.jpg", 400, 300, 1, &r, dst, &o, err, sizeof err) == NULL && err[0],
         "a missing original: an error");
  e = export_start (src, 400, 300, 1, &r, "/nonexistent/dir/out.jpg", &o, err, sizeof err);
  CHECK (e && run_export (e) == -1 && strstr (export_error (e), "Can't write") != NULL, "an unwritable place: an error");
  export_free (e);
  /* Abandoned half way: no partial file. */
  snprintf (dst, sizeof dst, "%s/out-abandoned.jpg", work);
  e = export_start (src, 400, 300, 1, &r, dst, &o, err, sizeof err);
  for (x = 0; x < 60; x++)              /* about 43 steps to decode, 43 to write */
    export_step (e, 7);
  f = fopen (dst, "rb");
  CHECK (f != NULL, "part written");
  if (f) fclose (f);
  export_free (e);
  f = fopen (dst, "rb");
  CHECK (f == NULL, "abandoned: the partial file is removed");
  if (f) fclose (f);
  /* The 100% view's region matches the same part of whole rows. */
  {
    geom_plan g;
    uint16_t *rows = malloc ((size_t) 400 * 10 * 6), *reg = malloc ((size_t) 50 * 10 * 6);
    int same = 1, xx, yy;
    recipe_default (&r);
    recipe_set (&r, P_ANGLE, 3);
    geom_plan_make (&g, &r, 400, 300, 1, 0, 0);
    geom_rows_rgb8 (&g, img.rgb, dev_srgb_to_linear (), rows, 100, 110, 100);
    geom_region_rgb8 (&g, img.rgb, dev_srgb_to_linear (), reg, 200, 100, 50, 10);
    for (yy = 0; yy < 10; yy++)
      for (xx = 0; xx < 50 * 3; xx++)
        if (reg[yy * 150 + xx] != rows[yy * 1200 + 600 + xx])
          same = 0;
    CHECK (same, "a region is exactly that part of the rows");
    free (rows);
    free (reg);
  }
  /* Sharpening in bands writes exactly what sharpening the whole would. */
  {
    rgb_image plain, sharp;
    uint32_t *px;
    int i, same = 1;
    recipe_default (&r);
    o.format = EXPORT_PNG;
    o.max_side = 0;
    snprintf (dst, sizeof dst, "%s/out-plain.png", work);
    e = export_start (src, 400, 300, 1, &r, dst, &o, err, sizeof err);
    run_export (e);
    export_free (e);
    recipe_set (&r, P_SHARPEN, 150);
    recipe_set (&r, P_SHARP_RADIUS, 2);
    snprintf (dst, sizeof dst, "%s/out-sharp.png", work);
    e = export_start (src, 400, 300, 1, &r, dst, &o, err, sizeof err);
    CHECK (run_export (e) >= 0, "a sharpened export");
    export_free (e);
    snprintf (dst, sizeof dst, "%s/out-plain.png", work);
    read_back (dst, &plain);
    snprintf (dst, sizeof dst, "%s/out-sharp.png", work);
    read_back (dst, &sharp);
    px = malloc ((size_t) 400 * 300 * 4);
    for (i = 0; i < 400 * 300; i++)
      px[i] = plain.rgb[3 * i] | plain.rgb[3 * i + 1] << 8 | (uint32_t) plain.rgb[3 * i + 2] << 16;
    sharpen32 (px, 400, 300, 150, sharpen_sigma (&r, 1));
    for (i = 0; i < 400 * 300; i++)
      if (sharp.rgb[3 * i] != (px[i] & 255) || sharp.rgb[3 * i + 2] != (px[i] >> 16 & 255))
        same = 0;
    CHECK (same, "sharpened in 48-row bands: no seams, the same as all at once");
    /* The red square's edge is crisper: more contrast across it. */
    /* The red square is darker (in luminance) than what's right of it. */
    /* Across the square's bottom edge (red above, at luminance 70, the
       gradient below at about 110): darker above it, lighter below. */
    CHECK (sharp.rgb[(98 * 400 + 50) * 3 + 1] < plain.rgb[(98 * 400 + 50) * 3 + 1]
           && sharp.rgb[(101 * 400 + 50) * 3 + 1] > plain.rgb[(101 * 400 + 50) * 3 + 1], "edges get more contrast");
    free (px); free (plain.rgb); free (sharp.rgb);
  }
  /* Sizes from the plan. */
  {
    int ow, oh;
    recipe_default (&r);
    export_plan_size (6000, 4000, &r, 2048, &ow, &oh);
    CHECK (ow == 2048 && oh == 1365, "6000x4000 at 2048: 2048x1365");
    export_plan_size (6000, 4000, &r, 0, &ow, &oh);
    CHECK (ow == 6000 && oh == 4000, "full size");
  }
  free (img.rgb);
}

static void test_sharpen (void)
{
  uint32_t px[40 * 10], before[40 * 10];
  int i, flat = 1;
  for (i = 0; i < 400; i++)
    px[i] = (i % 40) < 20 ? 0x505050 : 0xA0A0A0;
  memcpy (before, px, sizeof px);
  CHECK (sharpen32 (px, 40, 10, 0, 1.0) == 0 && memcmp (px, before, sizeof px) == 0, "amount 0: nothing");
  sharpen32 (px, 40, 10, 100, 1.0);
  CHECK ((px[5 * 40 + 19] & 255) < 0x50 && (px[5 * 40 + 20] & 255) > 0xA0, "a step: darker before it, lighter after");
  for (i = 0; i < 10; i++)
    if (px[i * 40 + 2] != before[i * 40 + 2] || px[i * 40 + 37] != before[i * 40 + 37])
      flat = 0;
  CHECK (flat, "flat areas away from the edge unchanged");
  CHECK ((px[5 * 40 + 19] & 255) == (px[5 * 40 + 19] >> 8 & 255) && (px[5 * 40 + 19] & 255) == (px[5 * 40 + 19] >> 16 & 255),
         "grey stays grey (luminance only)");
  {
    recipe z;
    recipe_default (&z);
    CHECK (sharpen_margin (1.0) == 3 && sharpen_sigma (&z, 0.1) >= 0.35, "margin, and the smallest sigma");
  }
}

/* Highlights & Shadows: local, and the same however the picture is cut up. */
static void test_highlights_shadows (void)
{
  enum { W = 256, H = 128 };
  static uint16_t lin[W * H * 3];
  static uint32_t a[W * H], b[W * H];
  static develop d;
  static dev_lummap m;
  recipe r;
  char text[512];
  int x, y;
  /* Left half dark (sRGB 30), right half light (sRGB 220), a little texture. */
  {
    const uint16_t *s2l = dev_srgb_to_linear ();
    for (y = 0; y < H; y++)
      for (x = 0; x < W; x++)
        {
          int v = (x < W / 2 ? 30 : 220) + ((x ^ y) & 3);
          uint16_t *p = lin + ((size_t) y * W + x) * 3;
          p[0] = p[1] = p[2] = s2l[v];
        }
  }
  dev_lummap_lin (&m, lin, W, H);
  CHECK (m.w == HS_MAP && m.h == HS_MAP / 2, "the map: 64 x 32 cells for a 2:1 picture");
  CHECK (m.l[0] < 0.2f && m.l[m.w - 1] > 0.8f, "dark on the left, light on the right");

  /* None: unchanged, and no map needed. */
  recipe_default (&r);
  develop_prepare (&d, &r);
  CHECK (!develop_needs_mask (&d), "no Highlights & Shadows: no map wanted");
  develop_rows (&d, lin, W, 0, H, a, NULL);

  /* Shadows: the dark side lifts a lot, the light side hardly. */
  recipe_set (&r, P_SHADOWS, 80);
  develop_prepare (&d, &r);
  CHECK (develop_needs_mask (&d), "Shadows wants the map");
  develop_mask (&d, &m, NULL, W, H);
  develop_rows (&d, lin, W, 0, H, b, NULL);
  {
    int dark0 = (int) (a[64 * W + 20] & 255), dark1 = (int) (b[64 * W + 20] & 255);
    int lite0 = (int) (a[64 * W + 230] & 255), lite1 = (int) (b[64 * W + 230] & 255);
    CHECK (dark1 > dark0 + 18, "Shadows 80: the dark side is lifted");
    CHECK (abs (lite1 - lite0) <= 2, "the light side stays");
    CHECK ((b[64 * W + 21] & 255) != (b[64 * W + 20] & 255) || ((21 ^ 64) & 3) == ((20 ^ 64) & 3),
           "texture inside the dark side survives (the gain is smooth, not per pixel)");
  }

  /* Highlights: the light side comes down, the dark side stays. */
  recipe_default (&r);
  recipe_set (&r, P_HIGHLIGHTS, 80);
  develop_prepare (&d, &r);
  develop_mask (&d, &m, NULL, W, H);
  develop_rows (&d, lin, W, 0, H, b, NULL);
  CHECK ((int) (b[64 * W + 230] & 255) < (int) (a[64 * W + 230] & 255) - 15, "Highlights 80: the light side comes down");
  CHECK (abs ((int) (b[64 * W + 20] & 255) - (int) (a[64 * W + 20] & 255)) <= 2, "the dark side stays");

  /* In bands, or as a region, the result is the same as all at once
     (export renders bands, 100% and the loupe render regions). */
  recipe_set (&r, P_SHADOWS, 60);
  develop_prepare (&d, &r);
  develop_mask (&d, &m, NULL, W, H);
  develop_region (&d, W, H, 0, 0);
  develop_rows (&d, lin, W, 0, H, a, NULL);
  develop_region (&d, W, H, 0, 50);
  develop_rows (&d, lin + (size_t) 50 * W * 3, W, 0, H - 50, b + (size_t) 50 * W, NULL);
  CHECK (memcmp (a + 50 * W, b + 50 * W, sizeof (uint32_t) * W * (H - 50)) == 0, "a band from row 50: identical");
  {
    static uint16_t reg[40 * 30 * 3];
    static uint32_t out[40 * 30];
    int ok = 1;
    for (y = 0; y < 30; y++)
      memcpy (reg + (size_t) y * 40 * 3, lin + ((size_t) (y + 70) * W + 100) * 3, 40 * 3 * sizeof (uint16_t));
    develop_region (&d, W, H, 100, 70);
    develop_rows (&d, reg, 40, 0, 30, out, NULL);
    for (y = 0; y < 30; y++)
      for (x = 0; x < 40; x++)
        if (out[y * 40 + x] != a[(y + 70) * W + x + 100])
          ok = 0;
    CHECK (ok, "a 40 x 30 region at (100, 70): identical");
  }
  /* At half the size, the same look (the map is in fractions of the picture). */
  {
    static uint16_t half[(W / 2) * (H / 2) * 3];
    static uint32_t hout[(W / 2) * (H / 2)];
    for (y = 0; y < H / 2; y++)
      for (x = 0; x < W / 2; x++)
        memcpy (half + ((size_t) y * (W / 2) + x) * 3, lin + ((size_t) (2 * y) * W + 2 * x) * 3, 6);
    develop_region (&d, W / 2, H / 2, 0, 0);
    develop_rows (&d, half, W / 2, 0, H / 2, hout, NULL);
    CHECK (abs ((int) (hout[32 * (W / 2) + 10] & 255) - (int) (a[64 * W + 20] & 255)) <= 3
           && abs ((int) (hout[32 * (W / 2) + 115] & 255) - (int) (a[64 * W + 230] & 255)) <= 3,
           "half size: the same brightness either side");
  }
  /* The recipe's text. */
  recipe_default (&r);
  recipe_set (&r, P_HIGHLIGHTS, 30);
  recipe_set (&r, P_SHADOWS, 45);
  recipe_format (&r, text, sizeof text);
  CHECK (strstr (text, "hs highlights=30 shadows=45") != NULL, "saved as: hs highlights=30 shadows=45");
  {
    recipe q;
    recipe_parse (&q, text);
    CHECK (recipe_equal (&q, &r), "and read back");
  }
  /* The whole recipe on an 8-bit image (edited thumbnails) uses it too. */
  {
    rgb_image img;
    img.w = W; img.h = H;
    img.rgb = malloc ((size_t) W * H * 3);
    for (y = 0; y < H; y++)
      for (x = 0; x < W; x++)
        memset (img.rgb + ((size_t) y * W + x) * 3, x < W / 2 ? 30 : 220, 3);
    recipe_default (&r);
    recipe_set (&r, P_SHADOWS, 80);
    CHECK (develop_image (&r, &img) == 0 && img.rgb[(64 * W + 20) * 3] > 48 && img.rgb[(64 * W + 230) * 3] >= 218,
           "thumbnails: shadows lifted, highlights kept");
    free (img.rgb);
  }
}

static void test_curves (void)
{
  recipe r, back;
  recipe_curve *c;
  char text[2048];
  int i, mono = 1;
  static develop d;
  uint16_t in[9];
  uint32_t px[3];
  recipe_default (&r);
  c = &r.curve[CURVE_RGB];
  CHECK (recipe_curve_is_identity (c) && fabs (recipe_curve_eval (c, 0.3) - 0.3) < 1e-9, "no points: a straight line");
  i = recipe_curve_add (c, 0.5f);
  CHECK (i == 1 && c->n == 3 && fabsf (c->y[1] - 0.5f) < 1e-6f, "a point added on the line, between the ends");
  recipe_curve_move (c, 1, 0.5f, 0.7f);
  CHECK (fabs (recipe_curve_eval (c, 0.5) - 0.7) < 1e-6 && recipe_curve_eval (c, 0) == 0 && recipe_curve_eval (c, 1) == 1,
         "through its points; the ends stay");
  recipe_curve_add (c, 0.8f);
  recipe_curve_move (c, 2, 0.8f, 0.72f);
  for (i = 1; i <= 100; i++)
    if (recipe_curve_eval (c, i / 100.0) < recipe_curve_eval (c, (i - 1) / 100.0) - 1e-9)
      mono = 0;
  CHECK (mono, "rising points: the curve never turns back (no overshoot)");
  recipe_curve_move (c, 1, 0.95f, 0.7f);
  CHECK (c->x[1] < c->x[2], "a point can't pass its neighbour");
  recipe_curve_move (c, 1, 0.5f, 0.7f);
  CHECK (recipe_curve_add (c, 0.51f) == -1, "not on top of another point");
  recipe_curve_remove (c, 0);
  CHECK (c->n == 4, "the ends can't be removed");
  recipe_curve_remove (c, 2);
  CHECK (c->n == 3, "a middle point can");
  r.curve[CURVE_RED].n = 2;
  r.curve[CURVE_RED].x[0] = 0; r.curve[CURVE_RED].y[0] = 0.1f;
  r.curve[CURVE_RED].x[1] = 1; r.curve[CURVE_RED].y[1] = 0.9f;
  CHECK (recipe_format (&r, text, sizeof text) > 0 && strstr (text, "curves rgb=0.000:0.000,0.500:0.700,1.000:1.000 red=0.000:0.100,1.000:0.900\n") != NULL,
         "text form: one curves line");
  recipe_parse (&back, text);
  CHECK (recipe_equal (&r, &back), "and back again");
  CHECK (!recipe_is_default (&r), "a curve is an edit");
  recipe_parse (&back, "curves rgb=0.5:0.5,0.2:0.9 green=bad\n");
  CHECK (recipe_curve_is_identity (&back.curve[CURVE_RGB]) && back.curve[CURVE_GREEN].n == 0, "nonsense ignored");
  /* Develop: mid grey brighter, black and white kept; the red curve only touches red. */
  recipe_default (&back);
  back.curve[CURVE_RGB] = r.curve[CURVE_RGB];
  develop_prepare (&d, &back);
  in[0] = in[1] = in[2] = 0;
  in[3] = in[4] = in[5] = 13996;             /* about sRGB 128 */
  in[6] = in[7] = in[8] = 65535;
  develop_rows (&d, in, 3, 0, 1, px, NULL);
  CHECK ((px[0] & 255) == 0 && (px[2] & 255) == 255 && (px[1] & 255) > 160, "RGB curve: the middle up, the ends kept");
  recipe_default (&back);
  back.curve[CURVE_RED] = r.curve[CURVE_RED];
  develop_prepare (&d, &back);
  develop_rows (&d, in, 3, 0, 1, px, NULL);
  CHECK ((px[0] & 255) > 20 && (px[0] >> 8 & 255) == 0 && (px[2] & 255) < 235 && (px[2] >> 16 & 255) == 255,
         "red curve: red lifted and lowered at the ends, green and blue untouched");
  /* A look carries curves. */
  recipe_default (&back);
  recipe_copy_look (&back, &r);
  CHECK (recipe_equal (&back, &r), "copy_look takes the curves");
}

/* Split toning and the built-in styles. */
static void test_toning (void)
{
  rgb_image img;
  recipe r;
  int k, x;
  unsigned char grey[3 * 3] = { 40, 40, 40, 128, 128, 128, 220, 220, 220 };
  img.w = 3; img.h = 1;
  img.rgb = malloc (9);
  /* Black and white, shadows toned warm (hue 30), highlights cool (210). */
  memcpy (img.rgb, grey, 9);
  recipe_default (&r);
  recipe_parse (&r, "enhance saturation=-100\ntone shadow_hue=30 shadows=80 highlight_hue=210 highlights=80\n");
  CHECK (r.v[P_TONE_SH_HUE] == 30 && r.v[P_TONE_SH] == 80 && r.v[P_TONE_HI_HUE] == 210, "tone line parsed");
  develop_image (&r, &img);
  CHECK (img.rgb[0] > img.rgb[2] + 4, "a dark grey toned warm: more red than blue");
  CHECK (img.rgb[8] > img.rgb[6] + 2, "a light grey toned cool: more blue than red");
  for (x = 0; x < 3; x++)
    {
      double y0 = grey[3 * x], y = 0.2126 * img.rgb[3 * x] + 0.7152 * img.rgb[3 * x + 1] + 0.0722 * img.rgb[3 * x + 2];
      CHECK (fabs (y - y0) < 12, "toning keeps the brightness");
    }
  /* None: unchanged. */
  memcpy (img.rgb, grey, 9);
  recipe_default (&r);
  recipe_parse (&r, "tone shadow_hue=30 highlight_hue=210\n");
  develop_image (&r, &img);
  CHECK (memcmp (img.rgb, grey, 9) == 0, "hues without strength: no change");
  free (img.rgb);
  /* Every style parses, says something, and comes back the same from its text. */
  CHECK (styles_count () >= 12, "a dozen or more styles");
  for (k = 0; k < styles_count (); k++)
    {
      const style *st = styles_get (k);
      recipe a, b;
      char t[2048];
      recipe_default (&a);
      recipe_parse (&a, st->recipe);
      recipe_format_look (&a, t, sizeof t);
      recipe_default (&b);
      recipe_parse (&b, t);
      CHECK (!recipe_is_default (&a) && recipe_equal (&a, &b) && recipe_geometry_is_default (&a) && st->about[0],
             st->name);
    }
}

/* Colour (HSL), vignette and noise reduction. */
static void test_hsl_vignette_noise (const char *work)
{
  recipe r;
  int o[3], a[3], k;
  char t[1024];
  /* HSL: blue's saturation down takes a blue's colour away and leaves red. */
  recipe_default (&r);
  recipe_parse (&r, "hsl blue_saturation=-100 red_hue=0\n");
  CHECK (r.v[HSL_PARAM (HSL_BLUE, HSL_SAT)] == -100, "hsl line parsed");
  recipe_format (&r, t, sizeof t);
  CHECK (strstr (t, "hsl blue_saturation=-100") != NULL && strstr (t, "red_hue") == NULL, "and written, defaults left out");
  dev1 (&r, 40, 90, 200, o);
  CHECK (abs (o[0] - o[2]) < 12 && abs (o[1] - o[2]) < 12, "blue, saturation -100: grey");
  dev1 (&r, 200, 40, 40, a);
  CHECK (a[0] == 200 && a[1] == 40 && a[2] == 40, "red: untouched");
  dev1 (&r, 128, 128, 128, a);
  CHECK (a[0] == 128 && a[1] == 128 && a[2] == 128, "grey: untouched");
  /* Hue: red turned towards orange (+), towards magenta (-). */
  recipe_default (&r);
  recipe_parse (&r, "hsl red_hue=100\n");
  dev1 (&r, 200, 30, 30, o);
  CHECK (o[1] > 50 && o[1] > o[2] + 15, "red hue +100: towards orange (more green than blue)");
  recipe_default (&r);
  recipe_parse (&r, "hsl red_hue=-100\n");
  dev1 (&r, 200, 30, 30, o);
  CHECK (o[2] > o[1] + 15, "red hue -100: towards magenta (more blue than green)");
  /* Luminance: green darker, a green's brightness falls; blue unaffected. */
  recipe_default (&r);
  recipe_parse (&r, "hsl green_luminance=-100\n");
  dev1 (&r, 60, 160, 60, o);
  CHECK (o[1] < 140 && o[1] > 60, "green luminance -100: darker green");
  dev1 (&r, 40, 60, 200, o);
  CHECK (abs (o[2] - 200) <= 2, "a blue isn't touched by green's slider");
  /* Neighbouring bands blend: halfway between orange (30) and yellow (60). */
  recipe_default (&r);
  recipe_parse (&r, "hsl yellow_saturation=-100\n");
  dev1 (&r, 230, 172, 30, o);          /* hue about 42: between orange and yellow */
  dev1 (&r, 230, 230, 30, a);          /* yellow itself */
  CHECK (abs (a[0] - a[2]) < 12, "yellow, -100: grey");
  CHECK (o[0] - o[2] > 40 && o[0] - o[2] < 190, "an orange-yellow between: partly taken");
  /* Copying a look carries the colour, noise and vignette settings. */
  {
    recipe dst, src;
    recipe_default (&src);
    recipe_parse (&src, "hsl aqua_hue=20\nnoise luminance=40 colour=60\nvignette amount=-50 midpoint=30\ncrop x0=0.1\n");
    recipe_default (&dst);
    recipe_copy_look (&dst, &src);
    CHECK (dst.v[HSL_PARAM (HSL_AQUA, HSL_HUE)] == 20 && dst.v[P_NOISE_COL] == 60 && dst.v[P_VIG_AMOUNT] == -50
           && dst.v[P_VIG_MID] == 30 && dst.v[P_CROP_X0] == 0, "a look: colour, noise, vignette (no crop)");
  }

  /* Vignette: corners darker than the middle, by about 2 stops at -100. */
  {
    rgb_image img;
    int x, y;
    img.w = 200; img.h = 100;
    img.rgb = malloc (200 * 100 * 3);
    memset (img.rgb, 160, 200 * 100 * 3);
    recipe_default (&r);
    recipe_parse (&r, "vignette amount=-100 midpoint=50\n");
    develop_image (&r, &img);
    {
      int mid = img.rgb[(50 * 200 + 100) * 3], corner = img.rgb[0], edge = img.rgb[(50 * 200 + 0) * 3];
      CHECK (mid >= 158 && mid <= 162, "vignette: the middle unchanged");
      CHECK (corner < 95 && corner > 60, "the corner about 2 stops darker");
      CHECK (edge < mid && edge > corner, "the side, in between");
    }
    memset (img.rgb, 100, 200 * 100 * 3);
    recipe_default (&r);
    recipe_parse (&r, "vignette amount=60\n");
    develop_image (&r, &img);
    CHECK (img.rgb[0] > 115, "a positive amount lightens the corners");
    /* The same through the rows a band at a time (export, the Viewer at 100%). */
    {
      static develop d;
      uint16_t *lin = malloc (200 * 10 * 6);
      uint32_t *px = malloc (200 * 10 * 4);
      unsigned char row[200 * 10 * 3];
      memset (row, 160, sizeof row);
      recipe_default (&r);
      recipe_parse (&r, "vignette amount=-100\n");
      develop_prepare (&d, &r);
      dev_to_linear (row, lin, 200 * 10);
      develop_region (&d, 200, 100, 0, 90);   /* the bottom 10 rows of 100 */
      develop_rows (&d, lin, 200, 0, 10, px, NULL);
      y = (int) (px[9 * 200] & 255);
      memset (img.rgb, 160, 200 * 100 * 3);
      develop_image (&r, &img);
      x = img.rgb[(99 * 200) * 3];
      CHECK (abs (x - y) <= 1, "a band of rows matches the whole (its place is known)");
      free (lin);
      free (px);
    }
    free (img.rgb);
  }

  /* Noise reduction: a noisy flat grey gets smoother; an edge stays. */
  {
    int w = 64, h = 64, x, y;
    uint32_t *px = malloc ((size_t) w * h * 4), *orig = malloc ((size_t) w * h * 4);
    double var0 = 0, var1 = 0, cvar0 = 0, cvar1 = 0;
    unsigned seed = 1;
    for (y = 0; y < h; y++)
      for (x = 0; x < w; x++)
        {
          int base = x < 32 ? 70 : 190, n[3], c;
          for (c = 0; c < 3; c++)
            {
              seed = seed * 1103515245u + 12345u;
              n[c] = base + (int) ((seed >> 16) % 13) - 6;
            }
          px[y * w + x] = (uint32_t) n[0] | (uint32_t) n[1] << 8 | (uint32_t) n[2] << 16;
        }
    memcpy (orig, px, (size_t) w * h * 4);
    CHECK (denoise32 (px, w, h, 0, 0, 1) == 0 && !memcmp (px, orig, (size_t) w * h * 4), "noise 0: unchanged");
    CHECK (denoise_margin (60, 80, 1) > 0 && denoise_margin (0, 0, 1) == 0, "a margin when on");
    denoise32 (px, w, h, 60, 80, 1);
    for (y = 8; y < 56; y++)
      for (x = 4; x < 24; x++)
        {
          int i = y * w + x, r0 = (int) (orig[i] & 255), g0 = (int) (orig[i] >> 8 & 255), b0 = (int) (orig[i] >> 16 & 255);
          int r1 = (int) (px[i] & 255), g1 = (int) (px[i] >> 8 & 255), b1 = (int) (px[i] >> 16 & 255);
          double l0 = (r0 + g0 + b0) / 3.0 - 70, l1 = (r1 + g1 + b1) / 3.0 - 70;
          var0 += l0 * l0;
          var1 += l1 * l1;
          cvar0 += (r0 - b0) * (r0 - b0);
          cvar1 += (r1 - b1) * (r1 - b1);
        }
    CHECK (var1 < var0 * 0.6, "luminance noise: smoother");
    CHECK (cvar1 < cvar0 * 0.4, "colour noise: much less");
    {
      int left = (int) (px[32 * w + 29] & 255), right = (int) (px[32 * w + 34] & 255);
      CHECK (left < 90 && right > 170, "the edge between 70 and 190 is kept");
    }
    free (px);
    free (orig);
  }
  /* Export with all three: runs, and the vignette is in the file. */
  {
    char src[700], dst[700], err[200];
    rgb_image img, back;
    export_opts o = { EXPORT_PNG, 95, 0, "", 0, 0, CS_SRGB, EXPORT_SHARP_NONE };
    exporter *e;
    img.w = 300; img.h = 200;
    img.rgb = malloc (300 * 200 * 3);
    memset (img.rgb, 150, 300 * 200 * 3);
    snprintf (src, sizeof src, "%s/vig-src.jpg", work);
    img_save_jpeg (&img, src, 98, err, sizeof err);
    recipe_default (&r);
    recipe_parse (&r, "vignette amount=-80\nnoise luminance=50 colour=50\nsharpen amount=40\nhsl blue_saturation=20\n");
    snprintf (dst, sizeof dst, "%s/vig-out.png", work);
    e = export_start (src, 300, 200, 1, &r, dst, &o, err, sizeof err);
    CHECK (e && run_export (e) >= 0, "export with colour, noise reduction and vignette");
    export_free (e);
    CHECK (read_back (dst, &back) == 0 && back.w == 300 && back.rgb[0] < 110 && back.rgb[(100 * 300 + 150) * 3] > 140,
           "exported: corners darker, middle as it was");
    if (back.rgb)
      free (back.rgb);
    free (img.rgb);
  }
  (void) k;
}

/* Export options: fit in a box, colour spaces, output sharpening; names. */
static void test_export_options (const char *work)
{
  char src[700], dst[700], err[200];
  rgb_image img, back, plain;
  recipe r;
  export_opts o = { EXPORT_JPEG, 95, 0, "", 0, 0, CS_SRGB, EXPORT_SHARP_NONE };
  exporter *e;
  int x, y, cs, k;
  FILE *f;
  img.w = 400; img.h = 300;
  img.rgb = malloc (400 * 300 * 3);
  for (y = 0; y < 300; y++)
    for (x = 0; x < 400; x++)
      {
        unsigned char *p = img.rgb + (y * 400 + x) * 3;
        int square = x >= 150 && x < 250 && y >= 100 && y < 200;
        p[0] = (unsigned char) (square ? 200 : 60);
        p[1] = (unsigned char) (square ? 120 : 110);
        p[2] = (unsigned char) (square ? 60 : 150);
      }
  snprintf (src, sizeof src, "%s/opts-src.jpg", work);
  img_save_jpeg (&img, src, 98, err, sizeof err);
  recipe_default (&r);

  /* A box: the largest that fits both ways. */
  o.box_w = 150; o.box_h = 150;
  {
    int ow, oh;
    export_plan_opts (400, 300, &r, &o, &ow, &oh);
    CHECK (ow == 150 && oh == 113, "box 150x150: 400x300 becomes 150x113");
    o.box_w = 1000; o.box_h = 120;
    export_plan_opts (400, 300, &r, &o, &ow, &oh);
    CHECK (ow == 160 && oh == 120, "box 1000x120: the height decides, 160x120");
    o.box_w = 2000; o.box_h = 2000;
    export_plan_opts (400, 300, &r, &o, &ow, &oh);
    CHECK (ow == 400 && oh == 300, "a box bigger than the photo: full size (never enlarged)");
    o.max_side = 200; o.box_w = 1000; o.box_h = 100;
    export_plan_opts (400, 300, &r, &o, &ow, &oh);
    CHECK (ow == 133 && oh == 100, "longer side 200 and a box 1000x100: both apply");
  }
  o.max_side = 0; o.box_w = 200; o.box_h = 200;
  snprintf (dst, sizeof dst, "%s/opts-box.jpg", work);
  e = export_start (src, 400, 300, 1, &r, dst, &o, err, sizeof err);
  CHECK (e && run_export (e) >= 0, "box export runs");
  export_free (e);
  CHECK (read_back (dst, &back) == 0 && back.w == 200 && back.h == 150, "written 200x150");
  free (back.rgb);
  o.box_w = o.box_h = 0;

  /* Caption and keywords: written as XMP in a JPEG and a PNG, read back. */
  {
    static const char *const ext[2] = { "jpg", "png" };
    int fmt;
    char xbuf[XMP_MAX];
    CHECK (xmp_packet ("", " , ", xbuf, sizeof xbuf) == 0, "no caption or keywords: no packet");
    CHECK (xmp_packet ("Tom & Jerry <at> \"home\"", "a, b", xbuf, 100) == 0, "too big for the space: none");
    for (fmt = 0; fmt < 2; fmt++)
      {
        unsigned char *buf = malloc (1 << 20);
        size_t got;
        char cap[512], kw[1024], what[80];
        o.format = fmt ? EXPORT_PNG : EXPORT_JPEG;
        snprintf (dst, sizeof dst, "%s/opts-tags.%s", work, ext[fmt]);
        e = export_start (src, 400, 300, 1, &r, dst, &o, err, sizeof err);
        export_set_tags (e, "Tom & Jerry <at> \"home\"", "Fells, Derwent Water, fells, rain; boats");
        run_export (e);
        export_free (e);
        f = fopen (dst, "rb");
        got = f && buf ? fread (buf, 1, 1 << 20, f) : 0;
        if (f)
          fclose (f);
        snprintf (what, sizeof what, "%s: the caption, its & < and \" kept", ext[fmt]);
        CHECK (got && xmp_read (buf, got, cap, sizeof cap, kw, sizeof kw) == 1 && !strcmp (cap, "Tom & Jerry <at> \"home\""), what);
        snprintf (what, sizeof what, "%s: the keywords, one of each", ext[fmt]);
        CHECK (!strcmp (kw, "Fells, Derwent Water, rain, boats"), what);
        if (fmt == 0)
          CHECK (got > 40 && memmem (buf, 4096, "http://ns.adobe.com/xap/1.0/", 29) != NULL, "jpg: in APP1 with the XMP namespace");
        else
          CHECK (memmem (buf, got, "iTXtXML:com.adobe.xmp", 21) != NULL, "png: in an iTXt chunk");
        free (buf);
      }
    o.format = EXPORT_JPEG;
    {
      /* As other programs write it: a simple description, attributes on the items. */
      static const char other[] =
        "junk<x:xmpmeta xmlns:x='adobe:ns:meta/'><rdf:RDF><rdf:Description>"
        "<dc:subject><rdf:Bag><rdf:li>Lake, District</rdf:li><rdf:li/><rdf:li xml:lang='en'> Swans </rdf:li></rdf:Bag></dc:subject>"
        "<dc:description>A   swan\n on&apos;the lake</dc:description></rdf:Description></rdf:RDF></x:xmpmeta>";
      char cap[512], kw[1024];
      CHECK (xmp_read ((const unsigned char *) other, sizeof other - 1, cap, sizeof cap, kw, sizeof kw) == 1
             && !strcmp (kw, "Lake  District, Swans") && !strcmp (cap, "A swan on'the lake"),
             "XMP as others write it: keywords (a comma inside one goes), a plain description");
      CHECK (xmp_read ((const unsigned char *) "no packet here", 14, cap, sizeof cap, kw, sizeof kw) == 0 && !cap[0] && !kw[0],
             "no packet: nothing");
    }
  }

  /* Colour spaces: the file says what it is, and Lumen reading it back
     (converting to sRGB) gets the colours it started with. */
  for (cs = CS_ADOBE; cs <= CS_P3; cs++)
    {
      unsigned char head[65536];
      size_t got;
      char name[64];
      unsigned char icc[COLOUR_ICC_MAX];
      size_t n = colour_icc_profile (cs, icc, sizeof icc);
      CHECK (n > 300 && colour_from_icc (icc, n, name, sizeof name) == cs, "our ICC profile is recognised as itself");
      o.colour = cs;
      snprintf (dst, sizeof dst, "%s/opts-cs%d.jpg", work, cs);
      e = export_start (src, 400, 300, 1, &r, dst, &o, err, sizeof err);
      run_export (e);
      export_free (e);
      f = fopen (dst, "rb");
      got = f ? fread (head, 1, sizeof head, f) : 0;
      if (f)
        fclose (f);
      CHECK (got > 0 && colour_from_jpeg (head, got, name, sizeof name) == cs,
             cs == CS_ADOBE ? "Adobe RGB export: tagged Adobe RGB" : "Display P3 export: tagged Display P3");
      CHECK (read_back (dst, &back) == 0, "read back");
      {
        int worst = 0, c;
        unsigned char *a = back.rgb + (150 * 400 + 200) * 3, *b = img.rgb + (150 * 400 + 200) * 3;
        unsigned char *a2 = back.rgb + (40 * 400 + 40) * 3, *b2 = img.rgb + (40 * 400 + 40) * 3;
        for (c = 0; c < 3; c++)
          {
            int d = abs (a[c] - b[c]), d2 = abs (a2[c] - b2[c]);
            worst = d > worst ? d : worst;
            worst = d2 > worst ? d2 : worst;
          }
        CHECK (worst <= 6, "and its colours come back as they were (within 6)");
      }
      free (back.rgb);
    }
  /* PNG too. */
  o.format = EXPORT_PNG;
  o.colour = CS_ADOBE;
  snprintf (dst, sizeof dst, "%s/opts-adobe.png", work);
  e = export_start (src, 400, 300, 1, &r, dst, &o, err, sizeof err);
  run_export (e);
  export_free (e);
  CHECK (read_back (dst, &back) == 0 && abs (back.rgb[(150 * 400 + 200) * 3] - 200) <= 4,
         "Adobe RGB PNG: read back right too");
  free (back.rgb);
  o.format = EXPORT_JPEG;
  o.colour = CS_SRGB;

  /* Output sharpening: the edge of the square gets crisper. */
  snprintf (dst, sizeof dst, "%s/opts-plain.png", work);
  o.format = EXPORT_PNG;
  e = export_start (src, 400, 300, 1, &r, dst, &o, err, sizeof err);
  run_export (e);
  export_free (e);
  read_back (dst, &plain);
  o.out_sharpen = EXPORT_SHARP_HIGH;
  snprintf (dst, sizeof dst, "%s/opts-sharp.png", work);
  e = export_start (src, 400, 300, 1, &r, dst, &o, err, sizeof err);
  run_export (e);
  export_free (e);
  read_back (dst, &back);
  {
    /* Just inside and just outside the square's left edge, red channel. */
    int in_p = plain.rgb[(150 * 400 + 150) * 3], out_p = plain.rgb[(150 * 400 + 149) * 3];
    int in_s = back.rgb[(150 * 400 + 150) * 3], out_s = back.rgb[(150 * 400 + 149) * 3];
    CHECK (in_s - out_s > in_p - out_p + 10, "output sharpening: more contrast at the edge");
    CHECK (abs (back.rgb[(40 * 400 + 40) * 3] - plain.rgb[(40 * 400 + 40) * 3]) <= 1, "and flat areas unchanged");
  }
  free (back.rgb);
  free (plain.rgb);
  free (img.rgb);

  /* Names from a pattern. */
  {
    char out[NAMING_MAX];
    naming_info ni = { "Friars_Crag", 7, "2022:08:14 06:12:30", "Lake District", "NIKON D850", 4 };
    naming_expand ("{date}_{name}_{nnn}", &ni, out);
    CHECK (!strcmp (out, "2022-08-14_Friars_Crag_007"), "pattern: {date}_{name}_{nnn}");
    naming_expand ("{project} {n} ({rating} stars) {camera}", &ni, out);
    CHECK (!strcmp (out, "Lake_District_7_(4_stars)_NIKON_D850"), "pattern: spaces become _");
    naming_expand ("{year}/{month}.{day}-{time}", &ni, out);
    CHECK (!strcmp (out, "2022/08/14-06-12-30"), "pattern: a full stop becomes /");
    naming_expand ("", &ni, out);
    CHECK (!strcmp (out, "Friars_Crag"), "empty pattern: the name");
    naming_expand ("{nope}{name", &ni, out);
    CHECK (!strcmp (out, "{nope}{name"), "unknown tokens kept as typed");
    ni.taken = "";
    naming_expand ("{date}", &ni, out);
    CHECK (!strcmp (out, "undated"), "no date: undated");
    CHECK (naming_varies ("{date}_{n}") && naming_varies ("x{name}") && !naming_varies ("{date}") && !naming_varies ("Holiday"),
           "which patterns give each photo its own name");
    for (k = 0; naming_tokens[k]; k++)
      ;
    CHECK (k >= 10, "the tokens for the menu");
  }
}

int main (int argc, char **argv)
{
  bus_trap_install ();
  if (argc < 2)
    return 2;
  fsys_mkdirs (argv[1]);
  test_recipe ();
  test_toning ();
  test_pixels ();
  test_histogram_and_speed ();
  test_edited_thumbnail (argv[1]);
  test_geometry ();
  test_resample ();
  test_sharpen ();
  test_curves ();
  test_highlights_shadows ();
  test_develop_image ();
  test_auto ();
  test_presets (argv[1]);
  test_export (argv[1]);
  test_export_options (argv[1]);
  test_hsl_vignette_noise (argv[1]);
  test_export_exif (argv[1]);
  printf ("test_develop: %d checks, %d failed\n", checks, fails);
  return fails != 0;
}
