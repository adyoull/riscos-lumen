/* The develop pipeline and recipes on Linux (x86-64, and ARM under
   qemu-arm-aligntrap). Usage: test_develop WORKDIR */
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <jpeglib.h>
#include "../src/core/develop.h"
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
  unsigned hist[64];
  unsigned total = 0;
  develop *d = malloc (sizeof *d);
  recipe r;
  clock_t t0;
  double ms_plain, ms_full;
  for (i = 0; i < w * h * 3; i++)
    src[i] = (uint16_t) ((i * 2654435761u) >> 16);
  recipe_default (&r);
  recipe_set (&r, P_TEMP, 12); recipe_set (&r, P_EV, 0.35f); recipe_set (&r, P_CONTRAST, 15);
  develop_prepare (d, &r);
  memset (hist, 0, sizeof hist);
  t0 = clock ();
  for (y = 0; y < h; y += 64)
    develop_rows (d, src, w, y, y + 64 < h ? y + 64 : h, dst, hist);
  ms_plain = (double) (clock () - t0) * 1000 / CLOCKS_PER_SEC;
  for (i = 0; i < 64; i++)
    total += hist[i];
  CHECK (total == (unsigned) (w * h), "the histogram counts every pixel");
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

int main (int argc, char **argv)
{
  if (argc < 2)
    return 2;
  fsys_mkdirs (argv[1]);
  test_recipe ();
  test_pixels ();
  test_histogram_and_speed ();
  test_edited_thumbnail (argv[1]);
  printf ("test_develop: %d checks, %d failed\n", checks, fails);
  return fails != 0;
}
