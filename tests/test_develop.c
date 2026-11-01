/* The develop pipeline and recipes on Linux (x86-64, and ARM under
   qemu-arm-aligntrap). Usage: test_develop WORKDIR */
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <jpeglib.h>
#include "../src/core/autoadj.h"
#include "../src/core/develop.h"
#include "../src/core/geometry.h"
#include "../src/core/resample.h"
#include "../src/core/fsys.h"
#include "../src/core/library.h"
#include "../src/core/recipe.h"
#include "../src/core/thumbgen.h"

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
  cat_source all = { SRC_ALL, 0, 0 };
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
    cat_source all = { SRC_ALL, 0, 0 };
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

int main (int argc, char **argv)
{
  if (argc < 2)
    return 2;
  fsys_mkdirs (argv[1]);
  test_recipe ();
  test_pixels ();
  test_histogram_and_speed ();
  test_edited_thumbnail (argv[1]);
  test_geometry ();
  test_resample ();
  test_develop_image ();
  test_auto ();
  test_presets (argv[1]);
  printf ("test_develop: %d checks, %d failed\n", checks, fails);
  return fails != 0;
}
