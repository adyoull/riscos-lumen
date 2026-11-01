/* LumenBench: times the editing pipeline on the machine it runs on, for
   milestone 1's gate (a slider's redraw within 50 ms).

   On RISC OS: in a task window (Ctrl-F12),  *Run <Lumen$Dir>.Bench
   It writes a 6000 x 4000 test JPEG to <Wimp$ScrapDir> (removed after),
   and prints:
     - opening it: decoding the 1600-pixel proxy (what the viewer does);
     - developing the proxy, and the half-size copy used while dragging,
       with tone only and with saturation and vibrancy. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include "develop.h"
#include "recipe.h"
#include "thumbgen.h"
#ifdef __riscos
#include <unixlib/local.h>
int __riscosify_control = __RISCOSIFY_NO_PROCESS;
#define SCRAP "<Wimp$ScrapDir>.LumenBench"
#else
#define SCRAP "/tmp/LumenBench.jpg"
#endif

static double ms_since (clock_t t0)
{
  return (double) (clock () - t0) * 1000.0 / CLOCKS_PER_SEC;
}

static double time_develop (const develop *d, const uint16_t *src, int w, int h, uint32_t *dst, int reps)
{
  clock_t t0 = clock ();
  int k;
  for (k = 0; k < reps; k++)
    develop_rows (d, src, w, 0, h, dst, NULL);
  return ms_since (t0) / reps;
}

int main (void)
{
  char err[200];
  rgb_image big, proxy;
  thumbgen *tg;
  develop *d = malloc (sizeof *d);
  recipe r;
  uint16_t *src;
  uint32_t *dst;
  clock_t t0;
  int x, y, rc;
  double tone, full, half_tone, half_full, open_ms;

  printf ("LumenBench %s\n", LUMEN_VERSION);
  /* A 6000 x 4000 test picture: smooth gradients with some texture. */
  big.w = 6000; big.h = 4000;
  big.rgb = malloc ((size_t) big.w * big.h * 3);
  if (!big.rgb || !d)
    {
      printf ("Not enough memory.\n");
      return 1;
    }
  for (y = 0; y < big.h; y++)
    for (x = 0; x < big.w; x++)
      {
        unsigned char *p = big.rgb + ((size_t) y * big.w + x) * 3;
        p[0] = (unsigned char) (x * 255 / big.w);
        p[1] = (unsigned char) (y * 255 / big.h);
        p[2] = (unsigned char) ((x ^ y) & 255);
      }
  printf ("Writing a 6000 x 4000 test JPEG...\n");
  if (img_save_jpeg (&big, SCRAP, 90, err, sizeof err))
    {
      printf ("%s\n", err);
      return 1;
    }
  free (big.rgb);

  t0 = clock ();
  tg = thumbgen_start (SCRAP, NULL, 1600, 1, err, sizeof err);
  if (!tg)
    {
      printf ("%s\n", err);
      return 1;
    }
  while ((rc = thumbgen_step (tg, 64)) == 0)
    ;
  if (rc < 0 || thumbgen_take (tg, &proxy))
    {
      printf ("%s\n", thumbgen_error (tg));
      return 1;
    }
  open_ms = ms_since (t0);
  thumbgen_free (tg);
  remove (SCRAP);

  src = malloc ((size_t) proxy.w * proxy.h * 6);
  dst = malloc ((size_t) proxy.w * proxy.h * 4);
  if (!src || !dst)
    {
      printf ("Not enough memory.\n");
      return 1;
    }
  dev_to_linear (proxy.rgb, src, (size_t) proxy.w * proxy.h);

  recipe_default (&r);
  recipe_set (&r, P_TEMP, 12); recipe_set (&r, P_EV, 0.35f); recipe_set (&r, P_CONTRAST, 15);
  develop_prepare (d, &r);
  tone = time_develop (d, src, proxy.w, proxy.h, dst, 10);
  half_tone = time_develop (d, src, proxy.w / 2, proxy.h / 2, dst, 20);
  recipe_set (&r, P_SATURATION, 20); recipe_set (&r, P_VIBRANCE, 20);
  t0 = clock ();
  for (x = 0; x < 10; x++)
    develop_prepare (d, &r);
  printf ("Preparing a recipe:            %6.1f ms\n", ms_since (t0) / 10);
  full = time_develop (d, src, proxy.w, proxy.h, dst, 10);
  half_full = time_develop (d, src, proxy.w / 2, proxy.h / 2, dst, 20);

  printf ("Opening (decode to %d x %d): %6.0f ms\n", proxy.w, proxy.h, open_ms);
  printf ("Proxy, tone only:              %6.1f ms\n", tone);
  printf ("Proxy, with saturation:        %6.1f ms\n", full);
  printf ("Half size (while dragging):    %6.1f ms tone, %.1f ms with saturation\n", half_tone, half_full);
  printf ("Gate (slider redraw <= 50 ms while dragging): %s\n",
          half_full + 5 <= 50 ? "met" : "NOT met");
  return 0;
}
