/* Copyright (c) 2026 Andrew Youll
   Released under the MIT licence: see the LICENSE file.
   SPDX-License-Identifier: MIT */
/* xmp: a photo's caption and keywords as an XMP packet, the form photo
   programs read and write them in (Lightroom, digiKam, darktable, Photos):
   dc:description for the caption, dc:subject for the keywords.

   An exported JPEG carries the packet in an APP1 segment after the
   namespace "http://ns.adobe.com/xap/1.0/"; a PNG in an iTXt chunk named
   "XML:com.adobe.xmp". On import the packet is looked for in the start
   of the file, so photos keep the keywords they were given elsewhere. */
#ifndef LUMEN_XMP_H
#define LUMEN_XMP_H
#include <stddef.h>

#define XMP_NAMESPACE "http://ns.adobe.com/xap/1.0/"
#define XMP_MAX 8192

/* The packet for a caption and keywords ("a, b"), with the same rules
   for splitting as the catalogue. Its length (no NUL counted; out is NUL
   terminated), or 0 if both are empty or it doesn't fit. */
size_t xmp_packet (const char *caption, const char *keywords, char *out, size_t cap);

/* Finds a packet in len bytes and reads the caption (x-default, or the
   first) and the keywords, as "a, b". 1 if a packet was found. */
int xmp_read (const unsigned char *buf, size_t len, char *caption, size_t cn, char *keywords, size_t kn);

#endif
