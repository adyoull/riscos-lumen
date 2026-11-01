/* export.c: see export.h. */
#include <math.h>
#include <setjmp.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <jpeglib.h>
#include <png.h>
#include "develop.h"
#include "export.h"
#include "fsys.h"
#include "geometry.h"
#include "sharpen.h"
#include "thumbgen.h"

#define BAND 16                        /* rows rendered at a time */

typedef struct
{
  struct jpeg_error_mgr pub;
  jmp_buf jb;
  char msg[JMSG_LENGTH_MAX];
} jerr;

struct exporter
{
  export_opts o;
  recipe r;
  char dst[1024], err[200];
  thumbgen *tg;
  rgb_image img;
  geom_plan g;
  develop *dev;
  int y;                               /* next output row */
  int band, margin, sharp;             /* rows written a step; extra rows each side for sharpening */
  double sigma;
  int done, file_open, created;
  int full_w, full_h;                  /* the original as shown, full size */
  uint16_t *lin;                       /* BAND rows after geometry */
  uint32_t *px;                        /* BAND rows developed */
  unsigned char *row;                  /* one RGB row for the encoder */
  FILE *f;
  /* JPEG */
  struct jpeg_compress_struct jc;
  jerr je;
  int jc_live;
  /* PNG */
  png_structp png;
  png_infop pinfo;
};

static void jerr_exit (j_common_ptr c)
{
  jerr *e = (jerr *) c->err;
  (*c->err->format_message) (c, e->msg);
  longjmp (e->jb, 1);
}

static void png_err (png_structp p, png_const_charp msg)
{
  exporter *e = png_get_error_ptr (p);
  snprintf (e->err, sizeof e->err, "PNG: %s", msg);
  png_longjmp (p, 1);
}

static void png_warn (png_structp p, png_const_charp msg)
{
  (void) p; (void) msg;
}

static int longer (int w, int h) { return w > h ? w : h; }

void export_plan_size (int w, int h, const recipe *r, int max_side, int *ow, int *oh)
{
  geom_plan g;
  geom_plan_make (&g, r, w, h, 1, 0, 0);
  *ow = g.ow;
  *oh = g.oh;
  if (max_side > 0 && longer (g.ow, g.oh) > max_side)
    {
      if (g.ow >= g.oh)
        {
          *ow = max_side;
          *oh = (int) lround ((double) g.oh * max_side / g.ow);
        }
      else
        {
          *oh = max_side;
          *ow = (int) lround ((double) g.ow * max_side / g.oh);
        }
      if (*ow < 1) *ow = 1;
      if (*oh < 1) *oh = 1;
    }
}

exporter *export_start (const char *src, int src_w, int src_h, int orientation,
                        const recipe *r, const char *dst, const export_opts *o,
                        char *err, size_t errlen)
{
  exporter *e = calloc (1, sizeof *e);
  int decode_side, ow, oh, full_w = src_w, full_h = src_h;
  if (!e || !(e->dev = malloc (sizeof *e->dev)))
    {
      free (e);
      snprintf (err, errlen, "Not enough memory");
      return NULL;
    }
  e->o = *o;
  if (e->o.quality < 1) e->o.quality = 1;
  if (e->o.quality > 100) e->o.quality = 100;
  e->r = *r;
  snprintf (e->dst, sizeof e->dst, "%s", dst);
  if (orientation >= 5)                /* turned a quarter: as shown */
    {
      full_w = src_h;
      full_h = src_w;
    }
  e->full_w = full_w;
  e->full_h = full_h;
  /* Decode only as large as the output needs: the output's scale relative
     to the full-size picture, applied to the original's longer side. */
  decode_side = longer (src_w, src_h);
  if (e->o.max_side > 0 && full_w > 0 && full_h > 0)
    {
      int fw, fh;
      export_plan_size (full_w, full_h, r, 0, &fw, &fh);
      export_plan_size (full_w, full_h, r, e->o.max_side, &ow, &oh);
      if (longer (ow, oh) < longer (fw, fh))
        decode_side = (int) ceil ((double) longer (src_w, src_h) * longer (ow, oh) / longer (fw, fh)) + 2;
    }
  if (decode_side < 1)
    decode_side = 1 << 30;
  e->tg = thumbgen_start (src, NULL, decode_side, orientation, err, errlen);
  if (!e->tg)
    {
      free (e->dev);
      free (e);
      return NULL;
    }
  develop_prepare (e->dev, r);
  return e;
}

static int open_output (exporter *e)
{
  e->f = fopen (e->dst, "wb");
  if (!e->f)
    {
      snprintf (e->err, sizeof e->err, "Can't write %s", e->dst);
      return -1;
    }
  e->file_open = 1;
  e->created = 1;
  if (e->o.format == EXPORT_JPEG)
    {
      e->jc.err = jpeg_std_error (&e->je.pub);
      e->je.pub.error_exit = jerr_exit;
      if (setjmp (e->je.jb))
        {
          snprintf (e->err, sizeof e->err, "%s", e->je.msg);
          return -1;
        }
      jpeg_create_compress (&e->jc);
      e->jc_live = 1;
      jpeg_stdio_dest (&e->jc, e->f);
      e->jc.image_width = (JDIMENSION) e->g.ow;
      e->jc.image_height = (JDIMENSION) e->g.oh;
      e->jc.input_components = 3;
      e->jc.in_color_space = JCS_RGB;
      jpeg_set_defaults (&e->jc);
      jpeg_set_quality (&e->jc, e->o.quality, TRUE);
      if (e->o.quality >= 90)          /* no colour subsampling at high quality */
        {
          e->jc.comp_info[0].h_samp_factor = 1;
          e->jc.comp_info[0].v_samp_factor = 1;
        }
      e->jc.density_unit = 1;          /* 72 dpi, as cameras write */
      e->jc.X_density = 72;
      e->jc.Y_density = 72;
      jpeg_start_compress (&e->jc, TRUE);
    }
  else
    {
      e->png = png_create_write_struct (PNG_LIBPNG_VER_STRING, e, png_err, png_warn);
      e->pinfo = e->png ? png_create_info_struct (e->png) : NULL;
      if (!e->pinfo)
        {
          snprintf (e->err, sizeof e->err, "Not enough memory");
          return -1;
        }
      if (setjmp (png_jmpbuf (e->png)))
        return -1;
      png_init_io (e->png, e->f);
      png_set_IHDR (e->png, e->pinfo, (png_uint_32) e->g.ow, (png_uint_32) e->g.oh, 8, PNG_COLOR_TYPE_RGB,
                    PNG_INTERLACE_NONE, PNG_COMPRESSION_TYPE_DEFAULT, PNG_FILTER_TYPE_DEFAULT);
      png_set_sRGB_gAMA_and_cHRM (e->png, e->pinfo, PNG_sRGB_INTENT_PERCEPTUAL);
      png_set_compression_level (e->png, 6);
      png_write_info (e->png, e->pinfo);
    }
  return 0;
}

static int write_rows (exporter *e, int y0, int y1, const uint32_t *band)
{
  volatile int y;
  int x;
  for (y = y0; y < y1; y++)
    {
      const uint32_t *p = band + (size_t) (y - y0) * e->g.ow;
      for (x = 0; x < e->g.ow; x++)
        {
          e->row[3 * x] = (unsigned char) p[x];
          e->row[3 * x + 1] = (unsigned char) (p[x] >> 8);
          e->row[3 * x + 2] = (unsigned char) (p[x] >> 16);
        }
      if (e->o.format == EXPORT_JPEG)
        {
          JSAMPROW r = e->row;
          if (setjmp (e->je.jb))
            {
              snprintf (e->err, sizeof e->err, "%s", e->je.msg);
              return -1;
            }
          jpeg_write_scanlines (&e->jc, &r, 1);
        }
      else
        {
          if (setjmp (png_jmpbuf (e->png)))
            return -1;
          png_write_row (e->png, e->row);
        }
    }
  return 0;
}

static int finish_output (exporter *e)
{
  int bad = 0;
  if (e->o.format == EXPORT_JPEG)
    {
      if (setjmp (e->je.jb))
        {
          snprintf (e->err, sizeof e->err, "%s", e->je.msg);
          return -1;
        }
      jpeg_finish_compress (&e->jc);
      jpeg_destroy_compress (&e->jc);
      e->jc_live = 0;
    }
  else
    {
      if (setjmp (png_jmpbuf (e->png)))
        return -1;
      png_write_end (e->png, e->pinfo);
      png_destroy_write_struct (&e->png, &e->pinfo);
    }
  bad = fclose (e->f) != 0;
  e->f = NULL;
  e->file_open = 0;
  if (bad)
    {
      snprintf (e->err, sizeof e->err, "Can't write %s (is the disc full?)", e->dst);
      remove (e->dst);
      return -1;
    }
  fsys_settype (e->dst, e->o.format == EXPORT_JPEG ? FT_JPEG : FT_PNG);
  return 0;
}

int export_step (exporter *e, int rows)
{
  if (e->done)
    return e->done;
  if (e->tg)
    {
      int r = thumbgen_step (e->tg, rows);
      if (r == 0)
        return 0;
      if (r < 0 || thumbgen_take (e->tg, &e->img))
        {
          snprintf (e->err, sizeof e->err, "%s", thumbgen_error (e->tg));
          thumbgen_free (e->tg);
          e->tg = NULL;
          return e->done = -1;
        }
      thumbgen_free (e->tg);
      e->tg = NULL;
      {
        int ow, oh, fw, fh;
        export_plan_size (e->img.w, e->img.h, &e->r, e->o.max_side, &ow, &oh);
        geom_plan_make (&e->g, &e->r, e->img.w, e->img.h, 1, ow, oh);
        /* Highlights & Shadows: the lightness map of the whole picture. */
        if (develop_needs_mask (e->dev))
          {
            dev_lummap *m = malloc (sizeof *m);
            if (!m)
              {
                snprintf (e->err, sizeof e->err, "Not enough memory");
                return e->done = -1;
              }
            dev_lummap_rgb8 (m, e->img.rgb, e->img.w, e->img.h);
            develop_mask (e->dev, m, &e->g, e->img.w, e->img.h);
            free (m);
          }
        /* Sharpening: its radius is in full-size pixels; the output's
           scale is its size over the full-size output's. */
        e->sharp = (int) e->r.v[P_SHARPEN];
        e->band = BAND;
        e->margin = 0;
        if (e->sharp > 0)
          {
            export_plan_size (e->full_w, e->full_h, &e->r, 0, &fw, &fh);
            e->sigma = sharpen_sigma (&e->r, fw > 0 ? (double) (ow > oh ? ow : oh) / (fw > fh ? fw : fh) : 1);
            e->margin = sharpen_margin (e->sigma);
            e->band = 48;              /* so the margins cost less */
          }
      }
      e->lin = malloc ((size_t) e->g.ow * (e->band + 2 * e->margin) * 6);
      e->px = malloc ((size_t) e->g.ow * (e->band + 2 * e->margin) * 4);
      e->row = malloc ((size_t) e->g.ow * 3);
      if (!e->lin || !e->px || !e->row)
        {
          snprintf (e->err, sizeof e->err, "Not enough memory");
          return e->done = -1;
        }
      if (open_output (e))
        return e->done = -1;
      return 0;
    }
  while (rows > 0 && e->y < e->g.oh)
    {
      int y1 = e->y + e->band < e->g.oh ? e->y + e->band : e->g.oh;
      /* With sharpening, the band is rendered with margin rows above and
         below (as far as the picture goes), sharpened, and its middle written. */
      int a = e->y - e->margin < 0 ? 0 : e->y - e->margin;
      int b = y1 + e->margin > e->g.oh ? e->g.oh : y1 + e->margin;
      geom_rows_rgb8 (&e->g, e->img.rgb, dev_srgb_to_linear (), e->lin, a, b, a);
      develop_region (e->dev, e->g.ow, e->g.oh, 0, a);
      develop_rows (e->dev, e->lin, e->g.ow, 0, b - a, e->px, NULL);
      if (e->sharp > 0)
        sharpen32 (e->px, e->g.ow, b - a, e->sharp, e->sigma);
      if (write_rows (e, e->y, y1, e->px + (size_t) (e->y - a) * e->g.ow))
        return e->done = -1;
      rows -= y1 - e->y;
      e->y = y1;
    }
  if (e->y >= e->g.oh)
    {
      if (finish_output (e))
        return e->done = -1;
      return e->done = 1;
    }
  return 0;
}

int export_progress (const exporter *e)
{
  if (e->done == 1)
    return 100;
  if (e->tg)
    return thumbgen_progress (e->tg) * 40 / 100;   /* decoding: the first 40% */
  return e->g.oh > 0 ? 40 + (int) ((long long) e->y * 60 / e->g.oh) : 40;
}

const char *export_error (const exporter *e)
{
  return e->err;
}

void export_size (const exporter *e, int *w, int *h)
{
  *w = e->g.ow;
  *h = e->g.oh;
}

void export_free (exporter *e)
{
  if (!e)
    return;
  thumbgen_free (e->tg);
  if (e->jc_live)
    {
      if (setjmp (e->je.jb) == 0)
        jpeg_destroy_compress (&e->jc);
    }
  if (e->png)
    png_destroy_write_struct (&e->png, &e->pinfo);
  if (e->f)
    fclose (e->f);
  if (e->done != 1 && e->created)
    remove (e->dst);
  free (e->img.rgb);
  free (e->lin);
  free (e->px);
  free (e->row);
  free (e->dev);
  free (e);
}
