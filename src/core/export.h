/* export: writes a version (an original through its recipe) as a new
   JPEG or PNG, at full size or smaller, a few rows at a time so the
   desktop stays usable.

   The original is decoded at the size needed (libjpeg's DCT scaling and
   area averaging, as for thumbnails) into 8-bit RGB - the only whole
   picture held, 3 bytes a pixel - and then each band of output rows goes
   through the geometry (in linear light), the develop pipeline and
   straight into the encoder. The original's EXIF goes with it (see
   exifout.h), in a JPEG's APP1 segment or a PNG's eXIf chunk. The file gets its RISC OS filetype at the
   end; a failed or abandoned export leaves no file behind. */
#ifndef LUMEN_EXPORT_H
#define LUMEN_EXPORT_H
#include <stddef.h>
#include "recipe.h"
#include "exif.h"

enum { EXPORT_JPEG, EXPORT_PNG };

/* Sharpening for the output, after any in the recipe: none, then for
   screens (low), for most uses (standard) and for print (high). */
enum { EXPORT_SHARP_NONE, EXPORT_SHARP_LOW, EXPORT_SHARP_STANDARD, EXPORT_SHARP_HIGH };

typedef struct
{
  int format;                          /* EXPORT_JPEG or EXPORT_PNG */
  int quality;                         /* JPEG, 1-100 */
  int max_side;                        /* longer side in pixels; 0 = full size */
  char software[32];                   /* EXIF Software, "" for none */
  int box_w, box_h;                    /* fit inside this box (0 x 0: no box); with max_side, both apply */
  int colour;                          /* CS_SRGB, CS_ADOBE or CS_P3 (colour.h): with a profile unless sRGB */
  int out_sharpen;                     /* EXPORT_SHARP_* */
} export_opts;

typedef struct exporter exporter;

/* src_w x src_h is the original's stored size (for working out how far
   it may be shrunk while decoding). NULL on error, message in err. */
exporter *export_start (const char *src, int src_w, int src_h, int orientation,
                        const recipe *r, const char *dst, const export_opts *o,
                        char *err, size_t errlen);
/* What the library knows of the photo (camera, date, exposure): the EXIF
   written when the original has none of its own to copy (exifout.h). */
void export_set_known (exporter *e, const img_info *known);
/* The photo's caption and keywords ("a, b"), written as XMP (xmp.h). */
void export_set_tags (exporter *e, const char *caption, const char *keywords);
/* Does up to rows rows of work. 0: more to do; 1: written; -1: failed. */
int export_step (exporter *e, int rows);
int export_progress (const exporter *e);   /* 0-100 */
const char *export_error (const exporter *e);
void export_size (const exporter *e, int *w, int *h);  /* once decoding is done */
/* Frees it; if it hadn't finished, its partial file is removed. */
void export_free (exporter *e);

/* The output size for an original of w x h (as oriented) with recipe r. */
void export_plan_size (int w, int h, const recipe *r, int max_side, int *ow, int *oh);
/* The same with all of o's limits (longer side, and the box). */
void export_plan_opts (int w, int h, const recipe *r, const export_opts *o, int *ow, int *oh);

#endif
