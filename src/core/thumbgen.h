/* thumbgen: makes a thumbnail (a small JPEG) from a JPEG or PNG, a few
   rows at a time, so the desktop stays responsive while it works.

     thumbgen *t = thumbgen_start (src, dst, 256, orientation, err, sizeof err);
     while ((r = thumbgen_step (t, 16)) == 0) ... return to the Wimp ...
     thumbgen_free (t);

   JPEGs are decoded with libjpeg's DCT scaling (1/2, 1/4, 1/8) as close to
   the thumbnail size as it allows, then box-filtered down; PNGs are read
   row by row and box-filtered. The EXIF orientation is applied at the end. */
#ifndef LUMEN_THUMBGEN_H
#define LUMEN_THUMBGEN_H
#include <stddef.h>

typedef struct thumbgen thumbgen;

/* Opens src and reads its header. NULL on error, with a message in err.
   dst NULL: nothing is written; take the pixels with thumbgen_take. */
thumbgen *thumbgen_start (const char *src, const char *dst, int max_side,
                          int orientation, char *err, size_t errlen);
/* Decodes up to rows source rows. 0: more to do; 1: finished, dst written;
   -1: failed (message from thumbgen_error). */
int thumbgen_step (thumbgen *t, int rows);
const char *thumbgen_error (const thumbgen *t);
/* The thumbnail's size, once finished (after orientation). */
void thumbgen_size (const thumbgen *t, int *w, int *h);
void thumbgen_free (thumbgen *t);

typedef struct rgb_image rgb_image;
/* Once finished: hands over the image (free img->rgb). 0 on success. */
int thumbgen_take (thumbgen *t, rgb_image *img);

/* An 8-bit RGB image, rows top-down, 3 bytes a pixel, no padding. */
struct rgb_image { int w, h; unsigned char *rgb; };

/* Loads a (small) JPEG whole, e.g. a thumbnail for display. 0 on success;
   free img->rgb when done. */
int img_load_jpeg (const char *path, rgb_image *img, char *err, size_t errlen);
/* Writes img as a JPEG. 0 on success. */
int img_save_jpeg (const rgb_image *img, const char *path, int quality, char *err, size_t errlen);
/* Applies EXIF orientation 1-8 in place (may swap w and h). 0 on success. */
int img_orient (rgb_image *img, int orientation);

#endif
