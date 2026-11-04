/* LumenBench: times the editing pipeline on the machine it runs on, for
   milestone 1's gate (a slider's redraw within 50 ms).

   On RISC OS: in a task window (Ctrl-F12),  *Run <Lumen$Dir>.Bench
   It writes a 6000 x 4000 test JPEG to <Wimp$ScrapDir> (removed after),
   and prints:
     - opening it: decoding the 1600-pixel proxy (what the viewer does),
       and where that time goes: reading the file, libjpeg-turbo's decoding
       (at 1/2, and at 3/8 as Lumen$JPEGEighths allows), and the rest;
     - developing the proxy, and the half-size copy used while dragging,
       with tone only and with saturation and vibrancy;
     - smoothing, enlarging and sharpening, plain and NEON;
     - a frame while dragging (develop + enlarge + plot) against 50 ms. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include "develop.h"
#include "resample.h"
#include "sharpen.h"
#include "simd.h"
#include "recipe.h"
#include "thumbgen.h"
#include <jpeglib.h>
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

/* Opening the test JPEG at 1600 as the viewer does; eighths: allowing
   any eighth (thumbgen_eighths). The proxy is kept when keep isn't NULL. */
static double open_proxy (int eighths, rgb_image *keep)
{
  char err[200];
  rgb_image img;
  thumbgen *tg;
  int rc;
  clock_t t0 = clock ();
  thumbgen_eighths (eighths);
  tg = thumbgen_start (SCRAP, NULL, 1600, 1, err, sizeof err);
  if (!tg)
    {
      printf ("%s\n", err);
      return -1;
    }
  while ((rc = thumbgen_step (tg, 64)) == 0)
    ;
  if (rc < 0 || thumbgen_take (tg, &img))
    {
      printf ("%s\n", thumbgen_error (tg));
      thumbgen_free (tg);
      return -1;
    }
  thumbgen_free (tg);
  thumbgen_eighths (-1);
  if (keep)
    *keep = img;
  else
    free (img.rgb);
  return ms_since (t0);
}

/* Reading the file chunk bytes at a time; its contents in *buf if asked. */
static double read_file (size_t chunk, unsigned char **buf, long *len)
{
  FILE *f;
  unsigned char *b;
  long n = 0;
  size_t got;
  clock_t t0 = clock ();
  f = fopen (SCRAP, "rb");
  if (!f)
    return -1;
  setvbuf (f, NULL, _IONBF, 0);       /* every read goes to the filing system */
  fseek (f, 0, SEEK_END);
  n = ftell (f);
  rewind (f);
  b = malloc ((size_t) n + 1);
  if (!b)
    {
      fclose (f);
      return -1;
    }
  for (got = 0; got < (size_t) n;)
    {
      size_t want = (size_t) n - got < chunk ? (size_t) n - got : chunk;
      size_t k = fread (b + got, 1, want, f);
      if (k == 0)
        break;
      got += k;
    }
  fclose (f);
  if (buf)
    {
      *buf = b;
      *len = n;
    }
  else
    free (b);
  return ms_since (t0);
}

/* libjpeg-turbo alone, from memory, at m/8 (rows thrown away). */
static double decode_only (const unsigned char *buf, long len, int m, int *w, int *h)
{
  struct jpeg_decompress_struct jd;
  struct jpeg_error_mgr je;
  unsigned char *row;
  clock_t t0 = clock ();
  jd.err = jpeg_std_error (&je);
  jpeg_create_decompress (&jd);
  jpeg_mem_src (&jd, buf, (unsigned long) len);
  jpeg_read_header (&jd, TRUE);
  jd.scale_num = (unsigned) m;
  jd.scale_denom = 8;
  jd.dct_method = JDCT_IFAST;
  jd.do_fancy_upsampling = FALSE;
  jd.out_color_space = JCS_RGB;
  jpeg_start_decompress (&jd);
  *w = (int) jd.output_width;
  *h = (int) jd.output_height;
  row = malloc ((size_t) jd.output_width * 3);
  while (row && jd.output_scanline < jd.output_height)
    jpeg_read_scanlines (&jd, &row, 1);
  jpeg_finish_decompress (&jd);
  jpeg_destroy_decompress (&jd);
  free (row);
  return ms_since (t0);
}

/* libjpeg-turbo's own (jsimd.h): 1 if its NEON inverse DCT is in use. */
extern int jsimd_can_idct_islow (void);

int main (void)
{
  char err[200];
  rgb_image big, proxy;
  develop *d = malloc (sizeof *d);
  recipe r;
  uint16_t *src;
  uint32_t *dst;
  clock_t t0;
  int x, y;
  double tone, full, half_tone, half_full, open_ms;

  printf ("LumenBench %s\n", LUMEN_VERSION);
  /* libjpeg-turbo picks NEON or plain C when it starts (VFPSupport says
     whether there's NEON; *Set JSIMD_FORCENONE 1 forces plain C). */
  printf ("JPEG code: %s\n", jsimd_can_idct_islow () ? "NEON" : "plain C");
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
  t0 = clock ();
  if (img_save_jpeg (&big, SCRAP, 90, err, sizeof err))
    {
      printf ("%s\n", err);
      return 1;
    }
  printf ("Writing it (JPEG, quality 90): %6.0f ms\n", ms_since (t0));
  free (big.rgb);

  /* Opening, and where its time goes: reading the file, libjpeg-turbo's
     decoding, and the rest (colour, scaling down to 1600). */
  open_ms = open_proxy (0, &proxy);
  if (open_ms < 0)
    return 1;
  {
    unsigned char *buf = NULL;
    long len = 0;
    int m2 = thumbgen_jpeg_eighths (6000, 1600), m8, dw2, dh2, dw8, dh8;
    double r4 = read_file (4096, NULL, NULL), r64 = read_file (65536, &buf, &len);
    double dec2, dec8, open8;
    thumbgen_eighths (1);
    m8 = thumbgen_jpeg_eighths (6000, 1600);
    thumbgen_eighths (-1);
    if (!buf)
      {
        printf ("Not enough memory.\n");
        return 1;
      }
    dec2 = decode_only (buf, len, m2, &dw2, &dh2);
    dec8 = decode_only (buf, len, m8, &dw8, &dh8);
    free (buf);
    open8 = open_proxy (1, NULL);
    printf ("Opening (decode to %d x %d): %6.0f ms\n", proxy.w, proxy.h, open_ms);
    printf ("  reading the file (%ld KB), 4 KB / 64 KB at a time: %.0f / %.0f ms\n", len / 1024, r4, r64);
    printf ("  decoding at %d/8 (%d x %d) alone:       %6.0f ms\n", m2, dw2, dh2, dec2);
    printf ("  the rest (colour, scaling to 1600):     %6.0f ms\n", open_ms - dec2 - r64);
    printf ("Opening at %d/8 (Lumen$JPEGEighths 1):  %6.0f ms\n", m8, open8);
    printf ("  decoding at %d/8 (%d x %d) alone:       %6.0f ms\n", m8, dw8, dh8, dec8);
  }
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

  printf ("Proxy, tone only:              %6.1f ms\n", tone);
  printf ("Proxy, with saturation:        %6.1f ms\n", full);
  printf ("Half size (while dragging):    %6.1f ms tone, %.1f ms with saturation\n", half_tone, half_full);
  /* Smoothing the renders to the size shown (a 1080p desktop), and
     sharpening the proxy: with the plain code and, where the processor
     has it, with NEON. */
  printf ("NEON: %s\n", !simd_built () ? "not in this build" : simd_neon () ? "yes, in use" : "no (plain code)");
  {
    int dw = proxy.w * 1150 / (proxy.w > proxy.h ? proxy.w : proxy.h);
    int dh = proxy.h * 1150 / (proxy.w > proxy.h ? proxy.w : proxy.h);
    uint32_t *sm = malloc ((size_t) dw * dh * 4), *cp = malloc ((size_t) proxy.w * proxy.h * 4);
    int pass, had = simd_neon ();
    double enlarge = 0;
    if (sm && cp)
      for (pass = 0; pass < (had ? 2 : 1); pass++)
        {
          int k;
          const char *which = pass ? "NEON " : "plain";
          simd_force (pass);
          t0 = clock ();
          for (k = 0; k < 5; k++)
            resample32 (dst, proxy.w, proxy.h, sm, dw, dh);
          printf ("Smoothing the proxy for the screen (%s): %5.1f ms\n", which, ms_since (t0) / 5);
          t0 = clock ();
          for (k = 0; k < 5; k++)
            resample32 (dst, proxy.w / 2, proxy.h / 2, sm, dw, dh);
          enlarge = ms_since (t0) / 5;   /* the last pass is the one in use */
          printf ("Enlarging the half size (%s):         %5.1f ms\n", which, enlarge);
          t0 = clock ();
          for (k = 0; k < 3; k++)
            {
              memcpy (cp, dst, (size_t) proxy.w * proxy.h * 4);
              sharpen32 (cp, proxy.w, proxy.h, 80, 1.0);
            }
          printf ("Sharpening the proxy (%s):            %5.1f ms\n", which, ms_since (t0) / 3);
        }
    simd_force (-1);
    free (sm);
    free (cp);
    /* A frame while dragging: the half size developed, then enlarged to
       the screen (with the code in use), and a few ms to plot it. */
    printf ("A frame while dragging: %.0f ms develop + %.0f ms enlarge + 5 ms plot = %.0f ms\n",
            half_full, enlarge, half_full + enlarge + 5);
    printf ("Gate (slider redraw <= 50 ms while dragging): %s\n",
            half_full + enlarge + 5 <= 50 ? "met" : "NOT met");
  }
  return 0;
}
