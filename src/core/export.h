/* export: writes a version (an original through its recipe) as a new
   JPEG or PNG, at full size or smaller, a few rows at a time so the
   desktop stays usable.

   The original is decoded at the size needed (libjpeg's DCT scaling and
   area averaging, as for thumbnails) into 8-bit RGB - the only whole
   picture held, 3 bytes a pixel - and then each band of output rows goes
   through the geometry (in linear light), the develop pipeline and
   straight into the encoder. The file gets its RISC OS filetype at the
   end; a failed or abandoned export leaves no file behind. */
#ifndef LUMEN_EXPORT_H
#define LUMEN_EXPORT_H
#include <stddef.h>
#include "recipe.h"

enum { EXPORT_JPEG, EXPORT_PNG };

typedef struct
{
  int format;                          /* EXPORT_JPEG or EXPORT_PNG */
  int quality;                         /* JPEG, 1-100 */
  int max_side;                        /* longer side in pixels; 0 = full size */
} export_opts;

typedef struct exporter exporter;

/* src_w x src_h is the original's stored size (for working out how far
   it may be shrunk while decoding). NULL on error, message in err. */
exporter *export_start (const char *src, int src_w, int src_h, int orientation,
                        const recipe *r, const char *dst, const export_opts *o,
                        char *err, size_t errlen);
/* Does up to rows rows of work. 0: more to do; 1: written; -1: failed. */
int export_step (exporter *e, int rows);
int export_progress (const exporter *e);   /* 0-100 */
const char *export_error (const exporter *e);
void export_size (const exporter *e, int *w, int *h);  /* once decoding is done */
/* Frees it; if it hadn't finished, its partial file is removed. */
void export_free (exporter *e);

/* The output size for an original of w x h (as oriented) with recipe r. */
void export_plan_size (int w, int h, const recipe *r, int max_side, int *ow, int *oh);

#endif
