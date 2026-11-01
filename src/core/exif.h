/* exif: reads what the library shows from the start of a JPEG or PNG file:
   pixel size, and for JPEG the EXIF orientation, capture time, camera and
   exposure. Byte-at-a-time reads only (no unaligned access), bounds checked
   against the buffer it is given. */
#ifndef LUMEN_EXIF_H
#define LUMEN_EXIF_H
#include <stddef.h>

enum { IMG_UNKNOWN = 0, IMG_JPEG = 1, IMG_PNG = 2 };

typedef struct
{
  int format;                 /* IMG_* */
  int width, height;          /* stored pixels (before orientation) */
  int orientation;            /* EXIF 1-8; 1 when absent */
  char taken[20];             /* "YYYY:MM:DD HH:MM:SS" or "" */
  char make[32], model[48];
  double exposure;            /* seconds, 0 if unknown */
  double fnumber, focal;      /* 0 if unknown */
  int iso;                    /* 0 if unknown */
  long thumb_offset;          /* embedded JPEG thumbnail, from the file start; 0 if none */
  long thumb_length;
} img_info;

/* What kind of image the first bytes are. */
int img_probe (const unsigned char *buf, size_t len);

/* Fills *info from buf, the first len bytes of the file (64 KB or more is
   enough for nearly every camera). Returns 0 if the format was recognised
   and its pixel size found, -1 otherwise. */
int img_read_info (const unsigned char *buf, size_t len, img_info *info);

#endif
