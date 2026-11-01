/* colour: photos in other colour spaces, shown and exported as sRGB.

   Lumen works in sRGB. A JPEG or PNG that says it is Adobe RGB (1998) or
   Display P3 (an iPhone's) is converted to sRGB as it is decoded: its own
   transfer curve to linear light, a 3x3 matrix to sRGB's primaries (both
   D65), clipped, and back to the sRGB curve. Colours outside sRGB are
   clipped; inside, they look as they should, rather than dull (Adobe RGB
   read as sRGB) or slightly off.

   The space comes from the embedded ICC profile's description ("Adobe RGB
   (1998)", "Display P3", "sRGB IEC61966-2.1") or, without one, from EXIF:
   ColorSpace "uncalibrated" with interoperability index "R03" is the
   camera's Adobe RGB (the DCF rule). Any other profile is shown as sRGB. */
#ifndef LUMEN_COLOUR_H
#define LUMEN_COLOUR_H
#include <stddef.h>

enum { CS_SRGB, CS_ADOBE, CS_P3, CS_OTHER };

/* From an ICC profile: its space, and its description in name (if not
   NULL; Latin-1, "" if it has none). */
int colour_from_icc (const unsigned char *icc, size_t len, char *name, size_t n);
/* From an EXIF block (the TIFF data after "Exif\0\0"). */
int colour_from_exif (const unsigned char *tiff, size_t len);
/* From the start of a JPEG file (the Info tab): the ICC profile if there
   is one (its first part is enough for the description), else EXIF. */
int colour_from_jpeg (const unsigned char *buf, size_t len, char *name, size_t n);
const char *colour_name (int cs);      /* "sRGB", "Adobe RGB (1998)", ... */

/* Converts w pixels of 8-bit RGB in place from cs to sRGB (nothing for
   CS_SRGB and CS_OTHER). */
void colour_row_to_srgb (int cs, unsigned char *rgb, int w);
/* The other way, for exports: w pixels of 8-bit sRGB to cs. */
void colour_row_from_srgb (int cs, unsigned char *rgb, int w);
/* An ICC profile for cs (CS_SRGB, CS_ADOBE or CS_P3) in out; its length,
   or 0 if cap is too small. COLOUR_ICC_MAX is always enough. */
#define COLOUR_ICC_MAX 3072
size_t colour_icc_profile (int cs, unsigned char *out, size_t cap);

#endif
