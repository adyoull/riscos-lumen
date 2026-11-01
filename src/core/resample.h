/* resample: photo-quality scaling of 32bpp pixels (&00BBGGRR or any
   order: the three low bytes are treated alike).

   Shrinking averages the source pixels around each destination pixel
   with a tent filter two destination pixels wide, so fine detail and
   edges stay smooth instead of being sampled away (no jaggies, no
   sparkle, no moire); enlarging is bilinear.
   Each axis is chosen separately. Integer arithmetic only, one row of
   accumulators: memory is a few kilobytes whatever the size. */
#ifndef LUMEN_RESAMPLE_H
#define LUMEN_RESAMPLE_H
#include <stdint.h>

/* src is sw x sh, dst dw x dh, rows packed. 0, or -1 if out of memory. */
int resample32 (const uint32_t *src, int sw, int sh, uint32_t *dst, int dw, int dh);

#endif
