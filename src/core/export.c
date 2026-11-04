/* export.c: see export.h. */
#include <math.h>
#include <setjmp.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <jpeglib.h>
#include <png.h>
#include "colour.h"
#include "develop.h"
#include "export.h"
#include "exifout.h"
#include "xmp.h"
#include "fsys.h"
#include "geometry.h"
#include "sharpen.h"
#include "retouch.h"
#include "denoise.h"
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
  int osharp;                          /* output sharpening amount, 0 for none */
  int nlum, ncol;                      /* noise reduction */
  double nscale;                       /* the output's scale against the full size */
  double osigma;
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
  /* EXIF */
  unsigned char *src_tiff;             /* the original's, copied; NULL if none */
  size_t src_tiff_len;
  img_info known;
  int have_known;
  char *xmp;                           /* the caption and keywords as XMP, or NULL */
  size_t xmp_len;
};

#define HEAD_READ (256 * 1024)          /* enough of the original for its EXIF (a NEF's too) */

/* The original's EXIF, kept until the output is opened. */
static void read_source_exif (exporter *e, const char *src)
{
  FILE *f = fopen (src, "rb");
  unsigned char *buf;
  size_t got, off, n;
  if (!f)
    return;
  buf = malloc (HEAD_READ);
  if (!buf)
    {
      fclose (f);
      return;
    }
  got = fread (buf, 1, HEAD_READ, f);
  fclose (f);
  if (exif_find_tiff (buf, got, &off, &n) == 0 && n >= 8)
    {
      if (off)
        memmove (buf, buf + off, n);
      e->src_tiff = buf;
      e->src_tiff_len = n;
      return;
    }
  free (buf);
}

void export_set_known (exporter *e, const img_info *known)
{
  e->known = *known;
  e->have_known = 1;
}

void export_set_tags (exporter *e, const char *caption, const char *keywords)
{
  char *x = malloc (XMP_MAX);
  free (e->xmp);
  e->xmp = NULL;
  e->xmp_len = 0;
  if (!x)
    return;
  e->xmp_len = xmp_packet (caption, keywords, x, XMP_MAX);
  if (e->xmp_len)
    e->xmp = x;
  else
    free (x);
}

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

void export_plan_opts (int w, int h, const recipe *r, const export_opts *o, int *ow, int *oh)
{
  export_plan_size (w, h, r, o->max_side, ow, oh);
  if (o->box_w > 0 && o->box_h > 0 && (*ow > o->box_w || *oh > o->box_h))
    {
      /* The largest that fits both ways, the shape kept. */
      double s = (double) o->box_w / *ow, t = (double) o->box_h / *oh;
      if (t < s)
        s = t;
      *ow = (int) lround (*ow * s);
      *oh = (int) lround (*oh * s);
      if (*ow < 1) *ow = 1;
      if (*oh < 1) *oh = 1;
      if (*ow > o->box_w) *ow = o->box_w;
      if (*oh > o->box_h) *oh = o->box_h;
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
  if ((e->o.max_side > 0 || (e->o.box_w > 0 && e->o.box_h > 0)) && full_w > 0 && full_h > 0)
    {
      int fw, fh;
      export_plan_size (full_w, full_h, r, 0, &fw, &fh);
      export_plan_opts (full_w, full_h, r, &e->o, &ow, &oh);
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
  read_source_exif (e, src);
  return e;
}

/* The EXIF block for the output, into exif (EXIFOUT_MAX bytes); its length, 0 for none. */
static size_t make_exif (exporter *e, unsigned char *exif)
{
  return exif_for_export (e->src_tiff, e->src_tiff_len, e->have_known ? &e->known : NULL, e->g.ow, e->g.oh,
                          e->o.software, e->o.colour == CS_ADOBE || e->o.colour == CS_P3 ? 0xFFFF : 1, exif,
                          EXIFOUT_MAX);
}

static int open_output (exporter *e)
{
  static unsigned char exif[6 + EXIFOUT_MAX];
  size_t exif_len = make_exif (e, exif + 6);
  memcpy (exif, "Exif\0\0", 6);
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
      if (exif_len)
        e->jc.write_JFIF_header = FALSE;   /* EXIF's APP1 comes straight after SOI */
      jpeg_start_compress (&e->jc, TRUE);
      if (exif_len)
        jpeg_write_marker (&e->jc, JPEG_APP0 + 1, exif, (unsigned) (exif_len + 6));
      if (e->o.colour == CS_ADOBE || e->o.colour == CS_P3)
        {
          /* The profile, in APP2 ICC_PROFILE segments (libjpeg-turbo writes them). */
          static unsigned char icc[COLOUR_ICC_MAX];
          size_t n = colour_icc_profile (e->o.colour, icc, sizeof icc);
          if (n)
            jpeg_write_icc_profile (&e->jc, icc, (unsigned) n);
        }
      if (e->xmp)
        {
          /* The caption and keywords: APP1, the XMP namespace and the packet. */
          static unsigned char app1[sizeof XMP_NAMESPACE + XMP_MAX];
          memcpy (app1, XMP_NAMESPACE, sizeof XMP_NAMESPACE);
          memcpy (app1 + sizeof XMP_NAMESPACE, e->xmp, e->xmp_len);
          jpeg_write_marker (&e->jc, JPEG_APP0 + 1, app1, (unsigned) (sizeof XMP_NAMESPACE + e->xmp_len));
        }
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
      if (e->o.colour == CS_ADOBE || e->o.colour == CS_P3)
        {
          static unsigned char icc[COLOUR_ICC_MAX];
          size_t n = colour_icc_profile (e->o.colour, icc, sizeof icc);
          if (n)
            png_set_iCCP (e->png, e->pinfo, e->o.colour == CS_ADOBE ? "Adobe RGB (1998)" : "Display P3",
                          PNG_COMPRESSION_TYPE_BASE, icc, (png_uint_32) n);
        }
      else
        png_set_sRGB_gAMA_and_cHRM (e->png, e->pinfo, PNG_sRGB_INTENT_PERCEPTUAL);
      png_set_compression_level (e->png, 6);
      if (exif_len)
        png_set_eXIf_1 (e->png, e->pinfo, (png_uint_32) exif_len, exif + 6);
      if (e->xmp)
        {
          png_text t;
          memset (&t, 0, sizeof t);
          t.compression = PNG_ITXT_COMPRESSION_NONE;
          t.key = (char *) "XML:com.adobe.xmp";
          t.text = e->xmp;
          t.itxt_length = e->xmp_len;
          t.lang = (char *) "";
          t.lang_key = (char *) "";
          png_set_text (e->png, e->pinfo, &t, 1);
        }
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
      colour_row_from_srgb (e->o.colour, e->row, e->g.ow);
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
      retouch_rgb8 (e->img.rgb, e->img.w, e->img.h, &e->r);   /* spots, on the photo upright */
      if (e->full_w <= 0 || e->full_h <= 0)
        {
          /* The original's size wasn't known: decoded whole, so this is it
             (sharpening's and noise reduction's radii are scaled by it). */
          e->full_w = e->img.w;
          e->full_h = e->img.h;
        }
      thumbgen_free (e->tg);
      e->tg = NULL;
      {
        int ow, oh, fw, fh;
        export_plan_opts (e->img.w, e->img.h, &e->r, &e->o, &ow, &oh);
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
        /* Noise reduction: before sharpening, its radii in full-size pixels. */
        e->nlum = (int) e->r.v[P_NOISE_LUM];
        e->ncol = (int) e->r.v[P_NOISE_COL];
        if (e->nlum > 0 || e->ncol > 0)
          {
            int m;
            export_plan_size (e->full_w, e->full_h, &e->r, 0, &fw, &fh);
            e->nscale = fw > 0 ? (double) (ow > oh ? ow : oh) / (fw > fh ? fw : fh) : 1;
            m = denoise_margin (e->nlum, e->ncol, e->nscale);
            e->margin += m;            /* sharpening reads the denoised rows round it */
            e->band = 48;
          }
        /* Output sharpening: in output pixels, whatever the size. */
        {
          static const int amounts[4] = { 0, 35, 70, 110 };
          int k = e->o.out_sharpen;
          e->osharp = k > 0 && k < 4 ? amounts[k] : 0;
          if (e->osharp)
            {
              /* Each step reads the rows round it as the one before left them:
                 the margins add up (denoise, then sharpen, then this). */
              e->osigma = 0.7;
              e->margin += sharpen_margin (e->osigma);
              e->band = 48;
            }
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
      if ((e->nlum > 0 || e->ncol > 0) && denoise32 (e->px, e->g.ow, b - a, e->nlum, e->ncol, e->nscale))
        {
          snprintf (e->err, sizeof e->err, "Not enough memory");
          return e->done = -1;
        }
      if (e->sharp > 0)
        sharpen32 (e->px, e->g.ow, b - a, e->sharp, e->sigma);
      if (e->osharp > 0)
        sharpen32 (e->px, e->g.ow, b - a, e->osharp, e->osigma);
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
  free (e->src_tiff);
  free (e->xmp);
  free (e->dev);
  free (e);
}
