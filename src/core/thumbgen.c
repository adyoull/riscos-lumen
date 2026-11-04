/* thumbgen.c: see thumbgen.h. libjpeg and libpng report errors by
   longjmp; each call into them sits under a setjmp in the same function. */
#include <setjmp.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <jpeglib.h>
#include <png.h>
#include "exif.h"
#include "thumbgen.h"
#include "colour.h"
#include "rawjob.h"

/* --- Box-filter downscaler: rows in at the source size, out at the target. */
typedef struct
{
  int sw, sh, tw, th;
  unsigned *sum;               /* tw * 3 sums for the target row being built */
  unsigned *cnt;               /* tw pixel counts */
  int *col;                    /* sw: the target column of each source column */
  int cur;                     /* target row being built */
  unsigned char *out;          /* tw * th * 3 */
} scaler;

static int scaler_init (scaler *s, int sw, int sh, int max_side)
{
  memset (s, 0, sizeof *s);
  s->sw = sw;
  s->sh = sh;
  if (sw >= sh)
    {
      s->tw = sw > max_side ? max_side : sw;
      s->th = (int) (((long long) sh * s->tw + sw / 2) / sw);
    }
  else
    {
      s->th = sh > max_side ? max_side : sh;
      s->tw = (int) (((long long) sw * s->th + sh / 2) / sh);
    }
  if (s->tw < 1) s->tw = 1;
  if (s->th < 1) s->th = 1;
  s->sum = calloc ((size_t) s->tw * 3, sizeof *s->sum);
  s->cnt = calloc ((size_t) s->tw, sizeof *s->cnt);
  s->out = malloc ((size_t) s->tw * s->th * 3);
  /* Which target column each source column goes into, worked out once: a
     64-bit division for every pixel is a library call on ARM, and was most
     of the time taken. */
  s->col = malloc ((size_t) sw * sizeof *s->col);
  if (!s->sum || !s->cnt || !s->out || !s->col)
    return -1;
  {
    int x;
    for (x = 0; x < sw; x++)
      s->col[x] = (int) ((long long) x * s->tw / s->sw);
  }
  return 0;
}

static void scaler_flush (scaler *s)
{
  int x;
  unsigned char *o;
  if (s->cur >= s->th)
    return;
  o = s->out + (size_t) s->cur * s->tw * 3;
  for (x = 0; x < s->tw; x++)
    {
      unsigned c = s->cnt[x] ? s->cnt[x] : 1;
      o[3 * x] = (unsigned char) ((s->sum[3 * x] + c / 2) / c);
      o[3 * x + 1] = (unsigned char) ((s->sum[3 * x + 1] + c / 2) / c);
      o[3 * x + 2] = (unsigned char) ((s->sum[3 * x + 2] + c / 2) / c);
    }
  memset (s->sum, 0, (size_t) s->tw * 3 * sizeof *s->sum);
  memset (s->cnt, 0, (size_t) s->tw * sizeof *s->cnt);
  s->cur++;
}

/* Adds source row y (RGB, sw pixels). */
static void scaler_row (scaler *s, int y, const unsigned char *row)
{
  int ty = (int) ((long long) y * s->th / s->sh), x;
  const int *col = s->col;
  unsigned *sum = s->sum, *cnt = s->cnt;
  while (s->cur < ty)
    scaler_flush (s);
  for (x = 0; x < s->sw; x++, row += 3)
    {
      unsigned *p = sum + 3 * col[x];
      p[0] += row[0];
      p[1] += row[1];
      p[2] += row[2];
      cnt[col[x]]++;
    }
}

static void scaler_finish (scaler *s)
{
  while (s->cur < s->th)
    scaler_flush (s);
}

static void scaler_free (scaler *s)
{
  free (s->sum);
  free (s->cnt);
  free (s->out);
  free (s->col);
  s->sum = NULL; s->cnt = NULL; s->out = NULL; s->col = NULL;
}

/* --- libjpeg error handling */
typedef struct
{
  struct jpeg_error_mgr pub;
  jmp_buf jb;
  char msg[JMSG_LENGTH_MAX];
} jerr;

static void jerr_exit (j_common_ptr c)
{
  jerr *e = (jerr *) c->err;
  (*c->err->format_message) (c, e->msg);
  longjmp (e->jb, 1);
}

static void jerr_silent (j_common_ptr c, int level)
{
  (void) c; (void) level;              /* warnings: corrupt data is common; carry on */
}

struct thumbgen
{
  int format;
  int orientation, max_side;
  char dst[1024];
  char err[200];
  FILE *f;
  scaler sc;
  int y;                               /* next source row */
  unsigned char *row;                  /* one decoded row */
  int cs;                              /* the file's colour space (colour.h): converted to sRGB */
  /* A camera raw file: the plug-in's job, then its picture read as a
     JPEG or a PPM. */
  rawjob *rj;
  int is_raw, raw_developed;
  img_info raw_info;
  int done;
  int out_w, out_h;
  /* JPEG */
  struct jpeg_decompress_struct jd;
  jerr je;
  int jd_live, cmyk;
  /* PNG */
  png_structp png;
  png_infop pinfo;
  int interlaced;
};

/* Decoding JPEGs at any eighth of their size (not only 1/2, 1/4, 1/8). */
static int eighths = -1;

void thumbgen_eighths (int on)
{
  eighths = on < 0 ? -1 : on != 0;
}

int thumbgen_eighths_on (void)
{
  if (eighths < 0)
    {
#ifdef __riscos
      const char *v = getenv ("Lumen$JPEGEighths");
#else
      const char *v = getenv ("LUMEN_JPEG_EIGHTHS");
#endif
      eighths = v && *v == '1';
    }
  return eighths;
}

/* The scale (m/8) to decode a JPEG whose longer side is longer at, so
   that it is still at least max_side: the smallest that is. */
int thumbgen_jpeg_eighths (int longer, int max_side)
{
  int m;
  for (m = 1; m < 8; m++)
    if ((thumbgen_eighths_on () || (m & (m - 1)) == 0)
        && ((long long) longer * m + 7) / 8 >= max_side)
      return m;
  return 8;
}

static void seterr (thumbgen *t, const char *m)
{
  snprintf (t->err, sizeof t->err, "%s", m);
}

static int start_jpeg (thumbgen *t)
{
  int m;
  t->jd.err = jpeg_std_error (&t->je.pub);
  t->je.pub.error_exit = jerr_exit;
  t->je.pub.emit_message = jerr_silent;
  if (setjmp (t->je.jb))
    {
      seterr (t, t->je.msg);
      return -1;
    }
  jpeg_create_decompress (&t->jd);
  t->jd_live = 1;
  jpeg_stdio_src (&t->jd, t->f);
  /* Keep the ICC profile (APP2) and EXIF (APP1): the colour space. */
  jpeg_save_markers (&t->jd, JPEG_APP0 + 2, 0xFFFF);
  jpeg_save_markers (&t->jd, JPEG_APP0 + 1, 0xFFFF);
  jpeg_read_header (&t->jd, TRUE);
  {
    JOCTET *icc = NULL;
    unsigned int icc_len = 0;
    jpeg_saved_marker_ptr m;
    t->cs = CS_SRGB;
    if (jpeg_read_icc_profile (&t->jd, &icc, &icc_len) && icc)
      {
        t->cs = colour_from_icc (icc, icc_len, NULL, 0);
        free (icc);
      }
    else
      for (m = t->jd.marker_list; m; m = m->next)
        if (m->marker == JPEG_APP0 + 1 && m->data_length > 6 && memcmp (m->data, "Exif\0\0", 6) == 0)
          t->cs = colour_from_exif (m->data + 6, m->data_length - 6);
  }
  /* The largest DCT reduction that still leaves the longer side >= max_side. */
  m = thumbgen_jpeg_eighths (t->jd.image_width > t->jd.image_height ? (int) t->jd.image_width
                             : (int) t->jd.image_height, t->max_side);
  t->jd.scale_num = (unsigned) m;
  t->jd.scale_denom = 8;
  t->jd.dct_method = JDCT_IFAST;
  t->jd.do_fancy_upsampling = FALSE;
  if (t->jd.jpeg_color_space == JCS_CMYK || t->jd.jpeg_color_space == JCS_YCCK)
    {
      t->jd.out_color_space = JCS_CMYK;
      t->cmyk = 1;
    }
  else
    t->jd.out_color_space = JCS_RGB;
  jpeg_start_decompress (&t->jd);
  t->row = malloc ((size_t) t->jd.output_width * (t->cmyk ? 4 : 3));
  if (!t->row || scaler_init (&t->sc, (int) t->jd.output_width, (int) t->jd.output_height, t->max_side))
    {
      seterr (t, "Not enough memory for the thumbnail");
      return -1;
    }
  return 0;
}

static int step_jpeg (thumbgen *t, int rows_in)
{
  volatile int rows = rows_in;
  if (setjmp (t->je.jb))
    {
      seterr (t, t->je.msg);
      return -1;
    }
  while (rows-- > 0 && t->jd.output_scanline < t->jd.output_height)
    {
      JSAMPROW r = t->row;
      jpeg_read_scanlines (&t->jd, &r, 1);
      if (t->cmyk)
        {
          /* Adobe CMYK JPEGs are stored inverted: R = C * K / 255. */
          unsigned x, w = t->jd.output_width;
          for (x = 0; x < w; x++)
            {
              unsigned c = t->row[4 * x], m = t->row[4 * x + 1], ye = t->row[4 * x + 2], k = t->row[4 * x + 3];
              t->row[3 * x] = (unsigned char) (c * k / 255);
              t->row[3 * x + 1] = (unsigned char) (m * k / 255);
              t->row[3 * x + 2] = (unsigned char) (ye * k / 255);
            }
        }
      else
        colour_row_to_srgb (t->cs, t->row, (int) t->jd.output_width);
      scaler_row (&t->sc, t->y++, t->row);
    }
  if (t->jd.output_scanline < t->jd.output_height)
    return 0;
  jpeg_finish_decompress (&t->jd);
  return 1;
}

static void png_err (png_structp p, png_const_charp msg)
{
  thumbgen *t = png_get_error_ptr (p);
  seterr (t, msg);
  png_longjmp (p, 1);
}

static void png_warn (png_structp p, png_const_charp msg)
{
  (void) p; (void) msg;
}

/* --- PPM (P6, 8 bits): what the raw plug-in develops to */
#define IMG_PPM 4

static int ppm_number (FILE *f)
{
  int c, n = 0;
  do
    {
      c = getc (f);
      if (c == '#')
        while (c != '\n' && c != EOF)
          c = getc (f);
    }
  while (c == ' ' || c == '\n' || c == '\r' || c == '\t');
  if (c < '0' || c > '9')
    return -1;
  while (c >= '0' && c <= '9')
    {
      n = n * 10 + (c - '0');
      if (n > 1 << 20)
        return -1;
      c = getc (f);
    }
  return n;                            /* the one whitespace after it is read too */
}

static int start_ppm (thumbgen *t)
{
  int w, h, maxval;
  if (getc (t->f) != 'P' || getc (t->f) != '6')
    {
      seterr (t, "Not a picture the raw plug-in made");
      return -1;
    }
  w = ppm_number (t->f);
  h = ppm_number (t->f);
  maxval = ppm_number (t->f);
  if (w <= 0 || h <= 0 || maxval != 255)
    {
      seterr (t, "Not a picture the raw plug-in made");
      return -1;
    }
  t->row = malloc ((size_t) w * 3);
  if (!t->row || scaler_init (&t->sc, w, h, t->max_side))
    {
      seterr (t, "Not enough memory for the thumbnail");
      return -1;
    }
  return 0;
}

static int step_ppm (thumbgen *t, int rows)
{
  while (rows-- > 0 && t->y < t->sc.sh)
    {
      if (fread (t->row, (size_t) t->sc.sw * 3, 1, t->f) != 1)
        {
          seterr (t, "The raw plug-in's picture is cut short");
          return -1;
        }
      scaler_row (&t->sc, t->y++, t->row);
    }
  return t->y >= t->sc.sh;
}

/* The raw job finished: its picture is read like any other file. */
static int start_raw_picture (thumbgen *t)
{
  unsigned char head[4];
  rawjob_details (t->rj, &t->raw_info);
  t->orientation = t->raw_info.orientation;   /* the plug-in's, as the catalogue will have it */
  t->f = fopen (rawjob_image (t->rj), "rb");
  if (t->f)
    setvbuf (t->f, NULL, _IOFBF, 64 * 1024);
  if (!t->f)
    {
      seterr (t, "Can't open the raw plug-in's picture");
      return -1;
    }
  if (fread (head, 1, sizeof head, t->f) != sizeof head)
    head[0] = 0;
  rewind (t->f);
  if (head[0] == 'P' && head[1] == '6')
    {
      t->format = IMG_PPM;
      return start_ppm (t);
    }
  t->format = img_probe (head, sizeof head);
  if (t->format != IMG_JPEG)
    {
      seterr (t, "The raw plug-in's picture isn't one Lumen reads");
      return -1;
    }
  return start_jpeg (t);
}

int thumbgen_raw_developed (const thumbgen *t) { return t->is_raw && t->raw_developed; }

int thumbgen_raw_details (const thumbgen *t, img_info *info)
{
  if (!t->is_raw || !t->raw_info.width)
    return 0;
  *info = t->raw_info;
  return 1;
}

static int start_png (thumbgen *t)
{
  png_uint_32 w, h;
  int depth, ctype, ilace;
  t->png = png_create_read_struct (PNG_LIBPNG_VER_STRING, t, png_err, png_warn);
  if (!t->png || !(t->pinfo = png_create_info_struct (t->png)))
    {
      seterr (t, "Not enough memory for the PNG reader");
      return -1;
    }
  if (setjmp (png_jmpbuf (t->png)))
    return -1;
  png_init_io (t->png, t->f);
  png_read_info (t->png, t->pinfo);
  png_get_IHDR (t->png, t->pinfo, &w, &h, &depth, &ctype, &ilace, NULL, NULL);
  t->cs = CS_SRGB;
#ifdef PNG_iCCP_SUPPORTED
  {
    png_charp name;
    int comp;
    png_bytep prof;
    png_uint_32 plen;
    if (png_get_iCCP (t->png, t->pinfo, &name, &comp, &prof, &plen) == PNG_INFO_iCCP)
      t->cs = colour_from_icc (prof, plen, NULL, 0);
  }
#endif
  png_set_expand (t->png);
  png_set_strip_16 (t->png);
  png_set_gray_to_rgb (t->png);
  if (!(ctype & PNG_COLOR_MASK_ALPHA) && !png_get_valid (t->png, t->pinfo, PNG_INFO_tRNS))
    png_set_filler (t->png, 0xFF, PNG_FILLER_AFTER);
  t->interlaced = ilace != PNG_INTERLACE_NONE;
  if (t->interlaced)
    png_set_interlace_handling (t->png);
  png_read_update_info (t->png, t->pinfo);
  t->row = malloc ((size_t) w * 4);
  if (!t->row || scaler_init (&t->sc, (int) w, (int) h, t->max_side))
    {
      seterr (t, "Not enough memory for the thumbnail");
      return -1;
    }
  return 0;
}

/* RGBA to RGB over the dark grey Lumen's browser draws behind thumbnails. */
static void flatten (unsigned char *row, int w)
{
  int x;
  for (x = 0; x < w; x++)
    {
      unsigned a = row[4 * x + 3], bg = 0x3C;
      row[3 * x] = (unsigned char) ((row[4 * x] * a + bg * (255 - a) + 127) / 255);
      row[3 * x + 1] = (unsigned char) ((row[4 * x + 1] * a + bg * (255 - a) + 127) / 255);
      row[3 * x + 2] = (unsigned char) ((row[4 * x + 2] * a + bg * (255 - a) + 127) / 255);
    }
}

static int step_png (thumbgen *t, int rows_in)
{
  volatile int rows = rows_in;
  if (setjmp (png_jmpbuf (t->png)))
    return -1;
  if (t->interlaced)
    {
      /* Interlaced: read it whole in one step (rare for photos). */
      size_t stride = (size_t) t->sc.sw * 4;
      unsigned char *all = malloc (stride * t->sc.sh);
      png_bytep *ptrs = malloc (sizeof *ptrs * t->sc.sh);
      int y;
      if (!all || !ptrs)
        {
          free (all); free (ptrs);
          seterr (t, "Not enough memory for an interlaced PNG");
          return -1;
        }
      for (y = 0; y < t->sc.sh; y++)
        ptrs[y] = all + stride * y;
      png_read_image (t->png, ptrs);
      for (y = 0; y < t->sc.sh; y++)
        {
          flatten (ptrs[y], t->sc.sw);
          colour_row_to_srgb (t->cs, ptrs[y], t->sc.sw);
          scaler_row (&t->sc, y, ptrs[y]);
        }
      free (all);
      free (ptrs);
      t->y = t->sc.sh;
    }
  else
    while (rows-- > 0 && t->y < t->sc.sh)
      {
        png_read_row (t->png, t->row, NULL);
        flatten (t->row, t->sc.sw);
        colour_row_to_srgb (t->cs, t->row, t->sc.sw);
        scaler_row (&t->sc, t->y++, t->row);
      }
  return t->y >= t->sc.sh;
}

thumbgen *thumbgen_start (const char *src, const char *dst, int max_side,
                          int orientation, char *err, size_t errlen)
{
  unsigned char head[16];
  size_t got;
  thumbgen *t = calloc (1, sizeof *t);
  if (!t)
    {
      snprintf (err, errlen, "Not enough memory");
      return NULL;
    }
  t->max_side = max_side;
  t->orientation = orientation;
  snprintf (t->dst, sizeof t->dst, "%s", dst ? dst : "");
  t->f = fopen (src, "rb");
  if (!t->f)
    {
      snprintf (err, errlen, "Can't open %s", src);
      free (t);
      return NULL;
    }
  /* libjpeg asks for 4 KB at a time; a bigger buffer means far fewer
     calls to the filing system. */
  setvbuf (t->f, NULL, _IOFBF, 64 * 1024);
  got = fread (head, 1, sizeof head, t->f);
  rewind (t->f);
  t->format = img_probe_named (head, got, src);
  if (t->format == IMG_RAW)
    {
      /* The plug-in does the work: the camera's preview for thumbnails,
         developed for anything bigger. */
      fclose (t->f);
      t->f = NULL;
      t->is_raw = 1;
      t->raw_developed = max_side > THUMBGEN_PREVIEW_SIDE;
      t->rj = rawjob_start (src, t->raw_developed, max_side, err, errlen);
      if (t->rj)
        return t;
      free (t);
      return NULL;
    }
  if (t->format == IMG_UNKNOWN)
    seterr (t, "Not a JPEG or PNG file");
  else if ((t->format == IMG_JPEG ? start_jpeg (t) : start_png (t)) == 0)
    return t;
  snprintf (err, errlen, "%s", t->err);
  thumbgen_free (t);
  return NULL;
}

int thumbgen_step (thumbgen *t, int rows)
{
  int r;
  rgb_image img;
  if (t->done)
    return t->done;
  if (t->format == IMG_RAW)
    {
      int j = rawjob_poll (t->rj);
      if (j == 0)
        return 0;
      if (j < 0)
        {
          seterr (t, rawjob_error (t->rj));
          return t->done = -1;
        }
      if (start_raw_picture (t))
        return t->done = -1;
      return 0;
    }
  r = t->format == IMG_JPEG ? step_jpeg (t, rows) : t->format == IMG_PPM ? step_ppm (t, rows) : step_png (t, rows);
  if (r == 0)
    return 0;
  if (r < 0)
    return t->done = -1;
  scaler_finish (&t->sc);
  img.w = t->sc.tw;
  img.h = t->sc.th;
  img.rgb = t->sc.out;
  if (img_orient (&img, t->orientation))
    {
      seterr (t, "Not enough memory to rotate the thumbnail");
      return t->done = -1;
    }
  t->sc.out = img.rgb;                 /* img_orient may have replaced the buffer */
  t->out_w = img.w;
  t->out_h = img.h;
  if (t->dst[0] && img_save_jpeg (&img, t->dst, 85, t->err, sizeof t->err))
    return t->done = -1;
  return t->done = 1;
}

int thumbgen_colour (const thumbgen *t) { return t->cs; }

const char *thumbgen_error (const thumbgen *t)
{
  return t->err;
}

int thumbgen_take (thumbgen *t, rgb_image *img)
{
  if (t->done != 1 || !t->sc.out)
    return -1;
  img->w = t->out_w;
  img->h = t->out_h;
  img->rgb = t->sc.out;
  t->sc.out = NULL;                    /* the caller frees it now */
  return 0;
}

void thumbgen_size (const thumbgen *t, int *w, int *h)
{
  *w = t->out_w;
  *h = t->out_h;
}

void thumbgen_free (thumbgen *t)
{
  if (!t)
    return;
  if (t->jd_live)
    {
      if (setjmp (t->je.jb) == 0)
        jpeg_destroy_decompress (&t->jd);
    }
  if (t->png)
    png_destroy_read_struct (&t->png, t->pinfo ? &t->pinfo : NULL, NULL);
  if (t->f)
    fclose (t->f);
  rawjob_free (t->rj);
  scaler_free (&t->sc);
  free (t->row);
  free (t);
}

int img_orient (rgb_image *img, int o)
{
  int w = img->w, h = img->h, x, y, nw, nh;
  unsigned char *out;
  if (o <= 1 || o > 8)
    return 0;
  nw = (o >= 5) ? h : w;
  nh = (o >= 5) ? w : h;
  out = malloc ((size_t) w * h * 3);
  if (!out)
    return -1;
  for (y = 0; y < h; y++)
    for (x = 0; x < w; x++)
      {
        int dx, dy;
        /* EXIF orientation: where stored pixel (x, y) is shown. */
        switch (o)
          {
          case 2: dx = w - 1 - x; dy = y; break;           /* mirrored */
          case 3: dx = w - 1 - x; dy = h - 1 - y; break;   /* 180 */
          case 4: dx = x; dy = h - 1 - y; break;           /* flipped */
          case 5: dx = y; dy = x; break;                   /* transposed */
          case 6: dx = h - 1 - y; dy = x; break;           /* 90 clockwise */
          case 7: dx = h - 1 - y; dy = w - 1 - x; break;   /* transverse */
          default: dx = y; dy = w - 1 - x; break;          /* 8: 90 anticlockwise */
          }
        memcpy (out + ((size_t) dy * nw + dx) * 3, img->rgb + ((size_t) y * w + x) * 3, 3);
      }
  free (img->rgb);
  img->rgb = out;
  img->w = nw;
  img->h = nh;
  return 0;
}

int img_save_jpeg (const rgb_image *img, const char *path, int quality, char *err, size_t errlen)
{
  struct jpeg_compress_struct c;
  jerr e;
  FILE *volatile f = fopen (path, "wb");
  int y;
  if (!f)
    {
      snprintf (err, errlen, "Can't write %s", path);
      return -1;
    }
  c.err = jpeg_std_error (&e.pub);
  e.pub.error_exit = jerr_exit;
  if (setjmp (e.jb))
    {
      snprintf (err, errlen, "%s", e.msg);
      jpeg_destroy_compress (&c);
      fclose (f);
      return -1;
    }
  jpeg_create_compress (&c);
  jpeg_stdio_dest (&c, f);
  c.image_width = (JDIMENSION) img->w;
  c.image_height = (JDIMENSION) img->h;
  c.input_components = 3;
  c.in_color_space = JCS_RGB;
  jpeg_set_defaults (&c);
  jpeg_set_quality (&c, quality, TRUE);
  jpeg_start_compress (&c, TRUE);
  for (y = 0; y < img->h; y++)
    {
      JSAMPROW r = img->rgb + (size_t) y * img->w * 3;
      jpeg_write_scanlines (&c, &r, 1);
    }
  jpeg_finish_compress (&c);
  jpeg_destroy_compress (&c);
  if (fclose (f) != 0)
    {
      snprintf (err, errlen, "Can't write %s", path);
      return -1;
    }
  return 0;
}

int img_load_jpeg (const char *path, rgb_image *img, char *err, size_t errlen)
{
  struct jpeg_decompress_struct d;
  jerr e;
  FILE *volatile f = fopen (path, "rb");
  unsigned char *volatile buf = NULL;
  img->rgb = NULL;
  if (!f)
    {
      snprintf (err, errlen, "Can't open %s", path);
      return -1;
    }
  d.err = jpeg_std_error (&e.pub);
  e.pub.error_exit = jerr_exit;
  e.pub.emit_message = jerr_silent;
  if (setjmp (e.jb))
    {
      snprintf (err, errlen, "%s", e.msg);
      jpeg_destroy_decompress (&d);
      fclose (f);
      free (buf);
      return -1;
    }
  jpeg_create_decompress (&d);
  jpeg_stdio_src (&d, f);
  jpeg_read_header (&d, TRUE);
  d.out_color_space = JCS_RGB;
  jpeg_start_decompress (&d);
  buf = malloc ((size_t) d.output_width * d.output_height * 3);
  if (!buf)
    {
      snprintf (err, errlen, "Not enough memory");
      jpeg_destroy_decompress (&d);
      fclose (f);
      return -1;
    }
  while (d.output_scanline < d.output_height)
    {
      JSAMPROW r = buf + (size_t) d.output_scanline * d.output_width * 3;
      jpeg_read_scanlines (&d, &r, 1);
    }
  img->w = (int) d.output_width;
  img->h = (int) d.output_height;
  jpeg_finish_decompress (&d);
  jpeg_destroy_decompress (&d);
  fclose (f);
  img->rgb = buf;
  return 0;
}

int thumbgen_progress (const thumbgen *t)
{
  if (t->done)
    return 100;
  return t->sc.sh > 0 ? (int) ((long long) t->y * 100 / t->sc.sh) : 0;
}
