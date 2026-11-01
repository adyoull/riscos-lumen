/* exif.c: see exif.h. TIFF/EXIF layout from the EXIF 2.32 specification:
   a TIFF header ("II" or "MM", 42, offset of IFD0), IFDs of 12-byte
   entries (tag, type, count, value or offset), IFD0 linking to the EXIF
   IFD (tag &8769) and to IFD1, which holds the thumbnail's offset and
   length (tags &201, &202). */
#include <string.h>
#include "exif.h"

typedef struct
{
  const unsigned char *p;    /* TIFF header */
  size_t len;                /* bytes from p to the end of the buffer */
  int big;                   /* "MM": big-endian */
} tiff;

static unsigned rd16 (const tiff *t, size_t off)
{
  if (off + 2 > t->len)
    return 0;
  return t->big ? (unsigned) (t->p[off] << 8 | t->p[off + 1])
                : (unsigned) (t->p[off] | t->p[off + 1] << 8);
}

static unsigned long rd32 (const tiff *t, size_t off)
{
  if (off + 4 > t->len)
    return 0;
  if (t->big)
    return (unsigned long) t->p[off] << 24 | (unsigned long) t->p[off + 1] << 16
         | (unsigned long) t->p[off + 2] << 8 | t->p[off + 3];
  return (unsigned long) t->p[off] | (unsigned long) t->p[off + 1] << 8
       | (unsigned long) t->p[off + 2] << 16 | (unsigned long) t->p[off + 3] << 24;
}

static const size_t type_size[] = { 0, 1, 1, 2, 4, 8, 1, 1, 2, 4, 8, 4, 8 };

/* Where an entry's value starts: inline if it fits in 4 bytes. */
static size_t value_at (const tiff *t, size_t entry, unsigned type, unsigned long count)
{
  size_t sz = (type < sizeof type_size / sizeof type_size[0]) ? type_size[type] : 0;
  if (sz == 0 || count > 0x10000)
    return (size_t) -1;
  if (sz * count <= 4)
    return entry + 8;
  return (size_t) rd32 (t, entry + 8);
}

static unsigned long int_value (const tiff *t, size_t entry, unsigned type, unsigned long count)
{
  size_t at = value_at (t, entry, type, count);
  if (at == (size_t) -1)
    return 0;
  return type == 3 ? rd16 (t, at) : (type == 4 || type == 9) ? rd32 (t, at) : 0;
}

static double rational (const tiff *t, size_t entry, unsigned type, unsigned long count)
{
  size_t at = value_at (t, entry, type, count);
  unsigned long n, d;
  if (at == (size_t) -1 || (type != 5 && type != 10))
    return type == 3 || type == 4 ? (double) int_value (t, entry, type, count) : 0;
  n = rd32 (t, at);
  d = rd32 (t, at + 4);
  if (d == 0)
    return 0;
  if (type == 10)
    return (double) (long) n / (double) (long) d;
  return (double) n / (double) d;
}

static void ascii (const tiff *t, size_t entry, unsigned type, unsigned long count, char *out, size_t n)
{
  size_t at = value_at (t, entry, type, count), i;
  out[0] = '\0';
  if (type != 2 || at == (size_t) -1 || at >= t->len || n == 0)
    return;
  for (i = 0; i < count && i + 1 < n && at + i < t->len && t->p[at + i]; i++)
    out[i] = (char) t->p[at + i];
  out[i] = '\0';
  while (i > 0 && out[i - 1] == ' ')           /* cameras pad with spaces */
    out[--i] = '\0';
}

/* Walks one IFD. Returns the offset of the next IFD (0 if none). */
static unsigned long walk (const tiff *t, size_t ifd, int which, img_info *info,
                           unsigned long *exif_ifd, long base)
{
  unsigned n, i;
  if (ifd < 8 || ifd + 2 > t->len)
    return 0;
  n = rd16 (t, ifd);
  if (n > 500 || ifd + 2 + 12 * (size_t) n + 4 > t->len)
    return 0;
  for (i = 0; i < n; i++)
    {
      size_t e = ifd + 2 + 12 * (size_t) i;
      unsigned tag = rd16 (t, e), type = rd16 (t, e + 2);
      unsigned long count = rd32 (t, e + 4);
      if (which == 0)                           /* IFD0 */
        switch (tag)
          {
          case 0x0112: info->orientation = (int) int_value (t, e, type, count); break;
          case 0x010F: ascii (t, e, type, count, info->make, sizeof info->make); break;
          case 0x0110: ascii (t, e, type, count, info->model, sizeof info->model); break;
          case 0x8769: *exif_ifd = int_value (t, e, type, count); break;
          case 0x0132: if (!info->taken[0]) ascii (t, e, type, count, info->taken, sizeof info->taken); break;
          }
      else if (which == 1)                      /* IFD1: the thumbnail */
        switch (tag)
          {
          case 0x0201: info->thumb_offset = (long) int_value (t, e, type, count) + base; break;
          case 0x0202: info->thumb_length = (long) int_value (t, e, type, count); break;
          }
      else                                      /* the EXIF IFD */
        switch (tag)
          {
          case 0x9003: ascii (t, e, type, count, info->taken, sizeof info->taken); break;
          case 0x829A: info->exposure = rational (t, e, type, count); break;
          case 0x829D: info->fnumber = rational (t, e, type, count); break;
          case 0x8827: info->iso = (int) int_value (t, e, type, count); break;
          case 0x920A: info->focal = rational (t, e, type, count); break;
          }
    }
  return rd32 (t, ifd + 2 + 12 * (size_t) n);
}

static void parse_exif (const unsigned char *p, size_t len, long base, img_info *info)
{
  tiff t;
  unsigned long ifd0, ifd1, exif_ifd = 0;
  char taken0[20];
  if (len < 8)
    return;
  t.p = p;
  t.len = len;
  if (p[0] == 'I' && p[1] == 'I')
    t.big = 0;
  else if (p[0] == 'M' && p[1] == 'M')
    t.big = 1;
  else
    return;
  if (rd16 (&t, 2) != 42)
    return;
  ifd0 = rd32 (&t, 4);
  ifd1 = walk (&t, ifd0, 0, info, &exif_ifd, base);
  /* DateTimeOriginal (EXIF IFD) wins over DateTime (IFD0). */
  memcpy (taken0, info->taken, sizeof taken0);
  info->taken[0] = '\0';
  if (exif_ifd)
    walk (&t, exif_ifd, 2, info, &exif_ifd, base);
  if (!info->taken[0])
    memcpy (info->taken, taken0, sizeof taken0);
  if (ifd1)
    walk (&t, ifd1, 1, info, &exif_ifd, base);
  if (info->orientation < 1 || info->orientation > 8)
    info->orientation = 1;
  if (info->thumb_offset <= 0 || info->thumb_length <= 0)
    info->thumb_offset = info->thumb_length = 0;
}

int img_probe (const unsigned char *buf, size_t len)
{
  static const unsigned char png_sig[8] = { 0x89, 'P', 'N', 'G', 0x0D, 0x0A, 0x1A, 0x0A };
  if (len >= 3 && buf[0] == 0xFF && buf[1] == 0xD8 && buf[2] == 0xFF)
    return IMG_JPEG;
  if (len >= 8 && memcmp (buf, png_sig, 8) == 0)
    return IMG_PNG;
  return IMG_UNKNOWN;
}

static int read_jpeg (const unsigned char *buf, size_t len, img_info *info)
{
  size_t pos = 2;
  while (pos + 4 <= len)
    {
      unsigned marker, seglen;
      if (buf[pos] != 0xFF)
        return -1;
      marker = buf[pos + 1];
      if (marker == 0xFF)                       /* fill byte */
        {
          pos++;
          continue;
        }
      if (marker == 0xD8 || (marker >= 0xD0 && marker <= 0xD7) || marker == 0x01)
        {
          pos += 2;
          continue;
        }
      seglen = (unsigned) (buf[pos + 2] << 8 | buf[pos + 3]);
      if (seglen < 2)
        return -1;
      if (marker == 0xE1 && pos + 10 <= len && memcmp (buf + pos + 4, "Exif\0\0", 6) == 0)
        {
          size_t start = pos + 10, end = pos + 2 + seglen;
          if (end > len)
            end = len;
          parse_exif (buf + start, end - start, (long) start, info);
        }
      /* Start of frame: SOF0-3, 5-7, 9-11, 13-15 (not C4 DHT, C8 JPG, CC DAC). */
      if (marker >= 0xC0 && marker <= 0xCF && marker != 0xC4 && marker != 0xC8 && marker != 0xCC)
        {
          if (pos + 9 > len)
            return -1;
          info->height = buf[pos + 5] << 8 | buf[pos + 6];
          info->width = buf[pos + 7] << 8 | buf[pos + 8];
          return (info->width > 0 && info->height > 0) ? 0 : -1;
        }
      if (marker == 0xDA || marker == 0xD9)     /* scan data before a frame header: give up */
        return -1;
      pos += 2 + seglen;
    }
  return -1;
}

int img_read_info (const unsigned char *buf, size_t len, img_info *info)
{
  memset (info, 0, sizeof *info);
  info->orientation = 1;
  info->format = img_probe (buf, len);
  if (info->format == IMG_JPEG)
    return read_jpeg (buf, len, info);
  if (info->format == IMG_PNG)
    {
      /* IHDR is the first chunk: length(4) "IHDR" width(4) height(4), big-endian. */
      if (len < 24 || memcmp (buf + 12, "IHDR", 4) != 0)
        return -1;
      info->width = (int) ((unsigned long) buf[16] << 24 | (unsigned long) buf[17] << 16 | buf[18] << 8 | buf[19]);
      info->height = (int) ((unsigned long) buf[20] << 24 | (unsigned long) buf[21] << 16 | buf[22] << 8 | buf[23]);
      return (info->width > 0 && info->height > 0) ? 0 : -1;
    }
  return -1;
}

/* --- every tag, for the Info tab */

typedef struct { unsigned tag; const char *name; } tag_name;

/* Names as the EXIF 2.32 and TIFF 6 specifications give them, spaced out. */
static const tag_name names[] = {
  { 0x0100, "Image width" }, { 0x0101, "Image height" }, { 0x0102, "Bits per sample" },
  { 0x0103, "Compression" }, { 0x0106, "Photometric interpretation" }, { 0x010E, "Description" },
  { 0x010F, "Make" }, { 0x0110, "Model" }, { 0x0111, "Strip offsets" }, { 0x0112, "Orientation" },
  { 0x0115, "Samples per pixel" }, { 0x0116, "Rows per strip" }, { 0x0117, "Strip byte counts" },
  { 0x011A, "X resolution" }, { 0x011B, "Y resolution" }, { 0x011C, "Planar configuration" },
  { 0x0128, "Resolution unit" }, { 0x012D, "Transfer function" }, { 0x0131, "Software" },
  { 0x0132, "Date and time" }, { 0x013B, "Artist" }, { 0x013C, "Host computer" },
  { 0x013E, "White point" }, { 0x013F, "Primary chromaticities" }, { 0x0201, "Thumbnail offset" },
  { 0x0202, "Thumbnail length" }, { 0x0211, "YCbCr coefficients" }, { 0x0212, "YCbCr subsampling" },
  { 0x0213, "YCbCr positioning" }, { 0x0214, "Reference black white" }, { 0x4746, "Rating" },
  { 0x4749, "Rating percent" }, { 0x8298, "Copyright" }, { 0x829A, "Exposure time" },
  { 0x829D, "F number" }, { 0x8822, "Exposure program" }, { 0x8824, "Spectral sensitivity" },
  { 0x8827, "ISO" }, { 0x8828, "OECF" }, { 0x8830, "Sensitivity type" },
  { 0x8831, "Standard output sensitivity" }, { 0x8832, "Recommended exposure index" },
  { 0x8833, "ISO speed" }, { 0x9000, "Exif version" }, { 0x9003, "Date taken" },
  { 0x9004, "Date digitised" }, { 0x9010, "Time zone" }, { 0x9011, "Time zone (taken)" },
  { 0x9012, "Time zone (digitised)" }, { 0x9101, "Components configuration" },
  { 0x9102, "Compressed bits per pixel" }, { 0x9201, "Shutter speed value" },
  { 0x9202, "Aperture value" }, { 0x9203, "Brightness value" }, { 0x9204, "Exposure bias" },
  { 0x9205, "Max aperture value" }, { 0x9206, "Subject distance" }, { 0x9207, "Metering mode" },
  { 0x9208, "Light source" }, { 0x9209, "Flash" }, { 0x920A, "Focal length" },
  { 0x9214, "Subject area" }, { 0x927C, "Maker notes" }, { 0x9286, "User comment" },
  { 0x9290, "Subseconds" }, { 0x9291, "Subseconds (taken)" }, { 0x9292, "Subseconds (digitised)" },
  { 0x9400, "Temperature" }, { 0x9401, "Humidity" }, { 0x9402, "Pressure" },
  { 0x9403, "Water depth" }, { 0x9404, "Acceleration" }, { 0x9405, "Camera elevation angle" },
  { 0xA000, "FlashPix version" }, { 0xA001, "Colour space" }, { 0xA002, "Pixel width" },
  { 0xA003, "Pixel height" }, { 0xA004, "Related sound file" }, { 0xA20B, "Flash energy" },
  { 0xA20E, "Focal plane X resolution" }, { 0xA20F, "Focal plane Y resolution" },
  { 0xA210, "Focal plane resolution unit" }, { 0xA214, "Subject location" },
  { 0xA215, "Exposure index" }, { 0xA217, "Sensing method" }, { 0xA300, "File source" },
  { 0xA301, "Scene type" }, { 0xA302, "CFA pattern" }, { 0xA401, "Custom rendered" },
  { 0xA402, "Exposure mode" }, { 0xA403, "White balance" }, { 0xA404, "Digital zoom ratio" },
  { 0xA405, "Focal length (35 mm)" }, { 0xA406, "Scene capture type" }, { 0xA407, "Gain control" },
  { 0xA408, "Contrast" }, { 0xA409, "Saturation" }, { 0xA40A, "Sharpness" },
  { 0xA40B, "Device setting description" }, { 0xA40C, "Subject distance range" },
  { 0xA420, "Image unique ID" }, { 0xA430, "Camera owner" }, { 0xA431, "Body serial number" },
  { 0xA432, "Lens specification" }, { 0xA433, "Lens make" }, { 0xA434, "Lens model" },
  { 0xA435, "Lens serial number" }, { 0xA460, "Composite image" }, { 0xA500, "Gamma" },
  { 0xC4A5, "Print image matching" }, { 0xEA1C, "Padding" }, { 0xEA1D, "Offset schema" }
};

static const tag_name gps_names[] = {
  { 0x00, "GPS version" }, { 0x01, "Latitude ref" }, { 0x02, "Latitude" }, { 0x03, "Longitude ref" },
  { 0x04, "Longitude" }, { 0x05, "Altitude ref" }, { 0x06, "Altitude" }, { 0x07, "Time (UTC)" },
  { 0x08, "Satellites" }, { 0x09, "Status" }, { 0x0A, "Measure mode" }, { 0x0B, "Precision (DOP)" },
  { 0x0C, "Speed unit" }, { 0x0D, "Speed" }, { 0x0E, "Track ref" }, { 0x0F, "Track" },
  { 0x10, "Image direction ref" }, { 0x11, "Image direction" }, { 0x12, "Map datum" },
  { 0x13, "Destination latitude ref" }, { 0x14, "Destination latitude" },
  { 0x15, "Destination longitude ref" }, { 0x16, "Destination longitude" },
  { 0x17, "Destination bearing ref" }, { 0x18, "Destination bearing" },
  { 0x19, "Destination distance ref" }, { 0x1A, "Destination distance" },
  { 0x1B, "Processing method" }, { 0x1C, "Area information" }, { 0x1D, "Date (UTC)" },
  { 0x1E, "Differential" }, { 0x1F, "Horizontal error" }
};

static const tag_name interop_names[] = {
  { 0x0001, "Interoperability index" }, { 0x0002, "Interoperability version" },
  { 0x1000, "Related image file format" }, { 0x1001, "Related image width" },
  { 0x1002, "Related image length" }
};

/* Values with meanings: tag, value, words. */
typedef struct { unsigned tag; unsigned v; const char *s; } tag_enum;
static const tag_enum enums[] = {
  { 0x0112, 1, "Normal" }, { 0x0112, 2, "Mirrored" }, { 0x0112, 3, "Upside down" },
  { 0x0112, 4, "Upside down, mirrored" }, { 0x0112, 5, "Turned left, mirrored" },
  { 0x0112, 6, "Turned right" }, { 0x0112, 7, "Turned right, mirrored" }, { 0x0112, 8, "Turned left" },
  { 0x0128, 1, "None" }, { 0x0128, 2, "Inches" }, { 0x0128, 3, "Centimetres" },
  { 0xA210, 1, "None" }, { 0xA210, 2, "Inches" }, { 0xA210, 3, "Centimetres" },
  { 0x0103, 1, "Uncompressed" }, { 0x0103, 6, "JPEG" }, { 0x0103, 7, "JPEG" },
  { 0x0213, 1, "Centred" }, { 0x0213, 2, "Co-sited" },
  { 0x8822, 0, "Not defined" }, { 0x8822, 1, "Manual" }, { 0x8822, 2, "Program AE" },
  { 0x8822, 3, "Aperture priority" }, { 0x8822, 4, "Shutter priority" }, { 0x8822, 5, "Creative" },
  { 0x8822, 6, "Action" }, { 0x8822, 7, "Portrait" }, { 0x8822, 8, "Landscape" },
  { 0x8830, 1, "Standard output sensitivity" }, { 0x8830, 2, "Recommended exposure index" },
  { 0x8830, 3, "ISO speed" },
  { 0x9207, 0, "Unknown" }, { 0x9207, 1, "Average" }, { 0x9207, 2, "Centre-weighted average" },
  { 0x9207, 3, "Spot" }, { 0x9207, 4, "Multi-spot" }, { 0x9207, 5, "Pattern" }, { 0x9207, 6, "Partial" },
  { 0x9207, 255, "Other" },
  { 0x9208, 0, "Unknown" }, { 0x9208, 1, "Daylight" }, { 0x9208, 2, "Fluorescent" },
  { 0x9208, 3, "Tungsten" }, { 0x9208, 4, "Flash" }, { 0x9208, 9, "Fine weather" },
  { 0x9208, 10, "Cloudy" }, { 0x9208, 11, "Shade" }, { 0x9208, 12, "Daylight fluorescent" },
  { 0x9208, 13, "Day white fluorescent" }, { 0x9208, 14, "Cool white fluorescent" },
  { 0x9208, 15, "White fluorescent" }, { 0x9208, 17, "Standard light A" },
  { 0x9208, 18, "Standard light B" }, { 0x9208, 19, "Standard light C" }, { 0x9208, 20, "D55" },
  { 0x9208, 21, "D65" }, { 0x9208, 22, "D75" }, { 0x9208, 23, "D50" },
  { 0x9208, 24, "ISO studio tungsten" }, { 0x9208, 255, "Other" },
  { 0xA001, 1, "sRGB" }, { 0xA001, 0xFFFF, "Uncalibrated" },
  { 0xA217, 1, "Not defined" }, { 0xA217, 2, "One-chip colour area" },
  { 0xA217, 3, "Two-chip colour area" }, { 0xA217, 4, "Three-chip colour area" },
  { 0xA217, 5, "Colour sequential area" }, { 0xA217, 7, "Trilinear" },
  { 0xA217, 8, "Colour sequential linear" },
  { 0xA401, 0, "Normal" }, { 0xA401, 1, "Custom" },
  { 0xA402, 0, "Auto" }, { 0xA402, 1, "Manual" }, { 0xA402, 2, "Auto bracket" },
  { 0xA403, 0, "Auto" }, { 0xA403, 1, "Manual" },
  { 0xA406, 0, "Standard" }, { 0xA406, 1, "Landscape" }, { 0xA406, 2, "Portrait" },
  { 0xA406, 3, "Night" },
  { 0xA407, 0, "None" }, { 0xA407, 1, "Low gain up" }, { 0xA407, 2, "High gain up" },
  { 0xA407, 3, "Low gain down" }, { 0xA407, 4, "High gain down" },
  { 0xA408, 0, "Normal" }, { 0xA408, 1, "Soft" }, { 0xA408, 2, "Hard" },
  { 0xA409, 0, "Normal" }, { 0xA409, 1, "Low" }, { 0xA409, 2, "High" },
  { 0xA40A, 0, "Normal" }, { 0xA40A, 1, "Soft" }, { 0xA40A, 2, "Hard" },
  { 0xA40C, 0, "Unknown" }, { 0xA40C, 1, "Macro" }, { 0xA40C, 2, "Close" }, { 0xA40C, 3, "Distant" },
  { 0xA460, 0, "Unknown" }, { 0xA460, 1, "Not composite" }, { 0xA460, 2, "Composite" },
  { 0xA460, 3, "Composite, taken alone" }
};

static const char *find_name (const tag_name *t, size_t n, unsigned tag)
{
  size_t i;
  for (i = 0; i < n; i++)
    if (t[i].tag == tag)
      return t[i].name;
  return NULL;
}

static const char *find_enum (unsigned tag, unsigned v)
{
  size_t i;
  for (i = 0; i < sizeof enums / sizeof enums[0]; i++)
    if (enums[i].tag == tag && enums[i].v == v)
      return enums[i].s;
  return NULL;
}

#include <stdio.h>
#include <math.h>

/* APEX aperture value to an f number: 2^(Av/2). */
static double pow2_half (double av) { return pow (2.0, av / 2.0); }

static void trim_number (char *s)
{
  /* "2.50" -> "2.5", "3.00" -> "3" */
  char *dot = strchr (s, '.'), *e;
  if (!dot)
    return;
  e = s + strlen (s) - 1;
  while (e > dot && *e == '0')
    *e-- = '\0';
  if (e == dot)
    *e = '\0';
}

/* The n'th value of an entry, as a double (rationals divided out). */
static double nth (const tiff *t, size_t at, unsigned type, unsigned long k)
{
  switch (type)
    {
    case 1: case 7: return at + k < t->len ? t->p[at + k] : 0;
    case 6: return at + k < t->len ? (signed char) t->p[at + k] : 0;
    case 3: return rd16 (t, at + 2 * k);
    case 8: return (short) rd16 (t, at + 2 * k);
    case 4: return (double) rd32 (t, at + 4 * k);
    case 9: return (double) (long) (int) rd32 (t, at + 4 * k);
    case 5: case 10:
      {
        unsigned long n = rd32 (t, at + 8 * k), d = rd32 (t, at + 8 * k + 4);
        if (d == 0)
          return 0;
        if (type == 10)
          return (double) (int) n / (double) (int) d;
        return (double) n / (double) d;
      }
    }
  return 0;
}

static void degrees (const tiff *t, size_t at, unsigned type, char *out, size_t n)
{
  double d = nth (t, at, type, 0), m = nth (t, at, type, 1), s = nth (t, at, type, 2);
  char sec[32];
  snprintf (sec, sizeof sec, "%.2f", s);
  trim_number (sec);
  snprintf (out, n, "%.0f\xB0 %.0f' %s\"", d, m, sec);
}

/* Formats entry e's value for people. */
static void format_value (const tiff *t, size_t e, int group, unsigned tag, unsigned type,
                          unsigned long count, char *out, size_t n)
{
  size_t at = value_at (t, e, type, count);
  double v;
  char num[48];
  unsigned long k;
  out[0] = '\0';
  if (at == (size_t) -1 || at > t->len)
    {
      snprintf (out, n, "(%lu values)", count);
      return;
    }
  if (type == 2)
    {
      ascii (t, e, type, count, out, n);
      return;
    }
  /* Undefined bytes: versions, comments, and blobs. */
  if (type == 7)
    {
      if ((tag == 0x9000 || tag == 0xA000 || (group == 3 && tag == 0x0002)) && count == 4 && at + 4 <= t->len)
        {
          const unsigned char *p = t->p + at;
          snprintf (out, n, "%c%c.%c%c", p[0] == '0' ? ' ' : p[0], p[1], p[2], p[3]);
          if (out[0] == ' ')
            memmove (out, out + 1, strlen (out));
          return;
        }
      if (tag == 0x9101 && count == 4 && at + 4 <= t->len)
        {
          static const char *const c[7] = { "", "Y", "Cb", "Cr", "R", "G", "B" };
          for (k = 0; k < 4; k++)
            if (t->p[at + k] > 0 && t->p[at + k] < 7)
              snprintf (out + strlen (out), n - strlen (out), "%s", c[t->p[at + k]]);
          return;
        }
      if ((tag == 0x9286 || (group == 2 && (tag == 0x1B || tag == 0x1C))) && count > 8 && at + count <= t->len)
        {
          /* An 8-byte character code, then the text: ASCII shown, others not. */
          size_t i, o = 0;
          if (memcmp (t->p + at, "ASCII\0\0\0", 8) == 0 || memcmp (t->p + at, "\0\0\0\0\0\0\0\0", 8) == 0)
            {
              for (i = 8; i < count && o + 1 < n && t->p[at + i]; i++)
                if (t->p[at + i] >= 32 && t->p[at + i] < 127)
                  out[o++] = (char) t->p[at + i];
              out[o] = '\0';
              while (o > 0 && out[o - 1] == ' ')
                out[--o] = '\0';
              if (o)
                return;
            }
          snprintf (out, n, "(%lu bytes)", count);
          return;
        }
      if (tag == 0xA300 && count >= 1)
        {
          snprintf (out, n, "%s", t->p[at] == 3 ? "Digital camera" : t->p[at] == 2 ? "Reflection scanner"
                                  : t->p[at] == 1 ? "Transparency scanner" : "Other");
          return;
        }
      if (tag == 0xA301 && count >= 1)
        {
          snprintf (out, n, "%s", t->p[at] == 1 ? "Directly photographed" : "Other");
          return;
        }
      if (count > 8)
        {
          snprintf (out, n, "(%lu bytes)", count);
          return;
        }
    }
  if (count == 0)
    return;
  v = nth (t, at, type, 0);
  if (count == 1 && group != 2)
    {
      const char *s = find_enum (tag, (unsigned) v);
      if (s && (type == 3 || type == 4 || type == 1))
        {
          snprintf (out, n, "%s", s);
          return;
        }
      switch (tag)
        {
        case 0x829A:                   /* exposure time */
          if (v > 0 && v < 0.5)
            snprintf (out, n, "1/%.0f s", 1 / v);
          else
            {
              snprintf (num, sizeof num, "%.1f", v);
              trim_number (num);
              snprintf (out, n, "%s s", num);
            }
          return;
        case 0x829D: case 0x9202: case 0x9205:
          if (tag != 0x829D)
            v = v > 0 ? pow2_half (v) : v;   /* APEX to an f number */
          snprintf (num, sizeof num, "%.1f", v);
          trim_number (num);
          snprintf (out, n, "f/%s", num);
          return;
        case 0x920A: case 0xA405:
          snprintf (num, sizeof num, "%.1f", v);
          trim_number (num);
          snprintf (out, n, "%s mm", num);
          return;
        case 0x9204:
          snprintf (num, sizeof num, "%+.2f", v);
          trim_number (num);
          snprintf (out, n, "%s EV", strcmp (num, "+0") == 0 || strcmp (num, "-0") == 0 ? "0" : num);
          return;
        case 0x9206:
          snprintf (num, sizeof num, "%.2f", v);
          trim_number (num);
          snprintf (out, n, "%s m", num);
          return;
        case 0x9209:
          {
            unsigned f = (unsigned) v;
            if (f == 0x20)
              snprintf (out, n, "No flash function");
            else
              snprintf (out, n, "%s%s%s%s", f & 1 ? "Fired" : "Did not fire",
                        (f & 0x18) == 0x18 ? ", auto" : (f & 0x18) == 0x08 ? ", compulsory" : (f & 0x18) == 0x10 ? ", off" : "",
                        f & 0x40 ? ", red-eye reduction" : "",
                        (f & 6) == 6 ? ", return detected" : (f & 6) == 4 ? ", no return" : "");
            return;
          }
        }
    }
  if (tag == 0xA432 && count == 4 && (type == 5 || type == 10))
    {
      /* Lens specification: shortest and longest focal length, the widest
         aperture at each: "16-55 mm f/2.8", or "23 mm f/2". */
      char a[4][24];
      for (k = 0; k < 4; k++)
        {
          snprintf (a[k], sizeof a[k], "%.1f", nth (t, at, type, k));
          trim_number (a[k]);
        }
      if (strcmp (a[0], a[1]) == 0)
        snprintf (out, n, "%s mm", a[0]);
      else
        snprintf (out, n, "%s-%s mm", a[0], a[1]);
      if (nth (t, at, type, 2) > 0)
        {
          snprintf (out + strlen (out), n - strlen (out), " f/%s", a[2]);
          if (strcmp (a[2], a[3]) != 0 && nth (t, at, type, 3) > 0)
            snprintf (out + strlen (out), n - strlen (out), "-%s", a[3]);
        }
      return;
    }
  if (group == 2)
    switch (tag)
      {
      case 0x00:                       /* version: 2.3.0.0 */
        for (k = 0; k < count && k < 4; k++)
          snprintf (out + strlen (out), n - strlen (out), "%s%.0f", k ? "." : "", nth (t, at, type, k));
        return;
      case 0x02: case 0x04: case 0x14: case 0x16:
        if (count == 3 && (type == 5 || type == 10))
          {
            degrees (t, at, type, out, n);
            return;
          }
        break;
      case 0x05:
        snprintf (out, n, "%s", v == 1 ? "Below sea level" : "Above sea level");
        return;
      case 0x06:
        snprintf (num, sizeof num, "%.1f", v);
        trim_number (num);
        snprintf (out, n, "%s m", num);
        return;
      case 0x07:
        if (count == 3)
          {
            snprintf (num, sizeof num, "%05.2f", nth (t, at, type, 2));
            trim_number (num);
            snprintf (out, n, "%02.0f:%02.0f:%s", v, nth (t, at, type, 1), num);
            return;
          }
        break;
      case 0x1E:
        snprintf (out, n, "%s", v == 1 ? "Corrected" : "Not corrected");
        return;
      }
  /* Plain numbers, up to eight of them. */
  if (count > 8)
    {
      snprintf (out, n, "(%lu values)", count);
      return;
    }
  for (k = 0; k < count; k++)
    {
      double x = nth (t, at, type, k);
      if (type == 5 || type == 10 || type == 11 || type == 12)
        snprintf (num, sizeof num, "%.4f", x);
      else
        snprintf (num, sizeof num, "%.0f", x);
      trim_number (num);
      snprintf (out + strlen (out), n - strlen (out), "%s%s", k ? ", " : "", num);
    }
}

typedef struct { exif_tag_fn fn; void *ctx; int count; } lister;

static void list_ifd (const tiff *t, size_t ifd, int group, lister *ls, int depth,
                      unsigned long *exif_ifd, unsigned long *gps_ifd, unsigned long *interop_ifd)
{
  static const char *const groups[4] = { "Camera", "Exif", "GPS", "Interoperability" };
  unsigned n, i;
  if (depth > 2 || ifd < 8 || ifd + 2 > t->len)
    return;
  n = rd16 (t, ifd);
  if (n > 500 || ifd + 2 + 12 * (size_t) n > t->len)
    return;
  for (i = 0; i < n; i++)
    {
      size_t e = ifd + 2 + 12 * (size_t) i;
      unsigned tag = rd16 (t, e), type = rd16 (t, e + 2);
      unsigned long count = rd32 (t, e + 4);
      const char *name;
      char nbuf[24], value[256];
      /* Pointers to other IFDs, and the thumbnail's own: not for people. */
      if (tag == 0x8769 && group == 0) { *exif_ifd = int_value (t, e, type, count); continue; }
      if (tag == 0x8825 && group == 0) { *gps_ifd = int_value (t, e, type, count); continue; }
      if (tag == 0xA005 && group == 1) { *interop_ifd = int_value (t, e, type, count); continue; }
      if (tag == 0x927C || tag == 0xC4A5 || tag == 0xEA1C || tag == 0x0201 || tag == 0x0202
          || tag == 0x0111 || tag == 0x0117)
        {
          if (tag != 0x927C)
            continue;
        }
      name = group == 2 ? find_name (gps_names, sizeof gps_names / sizeof gps_names[0], tag)
           : group == 3 ? find_name (interop_names, sizeof interop_names / sizeof interop_names[0], tag)
           : find_name (names, sizeof names / sizeof names[0], tag);
      if (!name)
        {
          snprintf (nbuf, sizeof nbuf, "Tag &%04X", tag);
          name = nbuf;
        }
      if (tag == 0x927C)
        snprintf (value, sizeof value, "(%lu bytes)", count);
      else
        format_value (t, e, group, tag, type, count, value, sizeof value);
      if (!value[0])
        continue;
      ls->fn (ls->ctx, groups[group], name, value);
      ls->count++;
    }
}

static int list_tiff (const unsigned char *p, size_t len, lister *ls)
{
  tiff t;
  unsigned long ifd0, exif_ifd = 0, gps_ifd = 0, interop_ifd = 0, none = 0;
  if (len < 8)
    return -1;
  t.p = p;
  t.len = len;
  if (p[0] == 'I' && p[1] == 'I')
    t.big = 0;
  else if (p[0] == 'M' && p[1] == 'M')
    t.big = 1;
  else
    return -1;
  if (rd16 (&t, 2) != 42)
    return -1;
  ifd0 = rd32 (&t, 4);
  list_ifd (&t, ifd0, 0, ls, 0, &exif_ifd, &gps_ifd, &none);
  if (exif_ifd)
    list_ifd (&t, exif_ifd, 1, ls, 1, &none, &none, &interop_ifd);
  if (gps_ifd)
    list_ifd (&t, gps_ifd, 2, ls, 1, &none, &none, &none);
  if (interop_ifd)
    list_ifd (&t, interop_ifd, 3, ls, 2, &none, &none, &none);
  return 0;
}

int exif_list_tiff (const unsigned char *tiff, size_t len, exif_tag_fn fn, void *ctx)
{
  lister ls;
  ls.fn = fn;
  ls.ctx = ctx;
  ls.count = 0;
  return list_tiff (tiff, len, &ls) ? -1 : ls.count;
}

int exif_list (const unsigned char *buf, size_t len, exif_tag_fn fn, void *ctx)
{
  size_t pos = 2;
  lister ls;
  ls.fn = fn;
  ls.ctx = ctx;
  ls.count = 0;
  if (img_probe (buf, len) != IMG_JPEG)
    return -1;
  while (pos + 4 <= len && buf[pos] == 0xFF)
    {
      unsigned marker = buf[pos + 1], seglen;
      if (marker == 0xFF)
        {
          pos++;
          continue;
        }
      if (marker == 0xD8 || (marker >= 0xD0 && marker <= 0xD7) || marker == 0x01)
        {
          pos += 2;
          continue;
        }
      if (marker == 0xDA || marker == 0xD9)
        break;
      seglen = (unsigned) (buf[pos + 2] << 8 | buf[pos + 3]);
      if (seglen < 2)
        break;
      if (marker == 0xE1 && pos + 10 <= len && memcmp (buf + pos + 4, "Exif\0\0", 6) == 0)
        {
          size_t start = pos + 10, end = pos + 2 + seglen;
          if (end > len)
            end = len;
          return list_tiff (buf + start, end - start, &ls) ? -1 : ls.count;
        }
      pos += 2 + seglen;
    }
  return -1;
}
