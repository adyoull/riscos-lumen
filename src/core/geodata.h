/* Copyright (c) 2026 Andrew Youll
   Released under the MIT licence: see the LICENSE file.
   SPDX-License-Identifier: MIT */
/* geodata: the Wimp messages of the GeoData protocol (Sine Nomine's
   "RISC OS geodata messaging system", as RiscOSM uses it), made and read
   without the Wimp so they can be tested anywhere. src/ui/geomap.c sends
   and answers them.

   Positions are WGS84 in ten-millionths of a degree, altitudes in mm,
   bearings in thousandths of a degree, times 5-byte RISC OS times
   (centiseconds since 1900), and text UTF-8. A message is at most 256
   bytes: block[0] its length, [4] its number, then the message's own
   words from +20 (block[5]). */
#ifndef LUMEN_GEODATA_H
#define LUMEN_GEODATA_H
#include <stddef.h>
#include "catalog.h"

#define GeoData_OpenMap          0x546D0
#define GeoData_DescribeMaps     0x546D1
#define GeoData_MapInfo          0x546D2
#define GeoData_DescribeDatasets 0x546D3
#define GeoData_DatasetDetails   0x546D4
#define GeoData_FetchData        0x546D5
#define GeoData_TransmitData     0x546D6
#define GeoData_FetchFile        0x546D7
#define GeoData_DisplayInfo      0x546D8
#define GeoData_Status           0x546D9
#define GeoData_Filetypes        0x546DA

/* Object types (TransmitData +32, DatasetDetails +40, FetchData +52). */
#define GD_PHOTOS          (1 << 5)
#define GD_TYPES           0xFFFF
/* What can be searched by (DatasetDetails +40) or was given (FetchData +52). */
#define GD_BOX             (1 << 16)
#define GD_RADIUS          (1 << 17)
#define GD_TEXT            (1 << 18)
#define GD_PAGED           (1 << 19)
#define GD_PLUGIN          (1 << 20)
#define GD_BEARING         (1 << 21)
/* DatasetDetails +44. */
#define GD_INTERSECTS      (1 << 0)
#define GD_FETCH_ALL       (1 << 2)
#define GD_FETCH_BOX       (1 << 3)
#define GD_FETCH_RADIUS    (1 << 4)
#define GD_SEARCH_ALL      (1 << 5)
#define GD_SEARCH_BOX      (1 << 6)
#define GD_SEARCH_RADIUS   (1 << 7)
#define GD_SUPPORTS_PAGES  (1 << 9)
#define GD_BY_ID           (1 << 10)
/* TransmitData +36. */
#define GD_DRAW_MAP        (1 << 0)
#define GD_FILE            (1 << 2)
#define GD_MORE_INFO       (1 << 3)
#define GD_LATLON          (1 << 16)
#define GD_ALTITUDE        (1 << 17)
#define GD_TIME            (1 << 18)
#define GD_HAS_BEARING     (1 << 21)
#define GD_TITLE           (1 << 24)
#define GD_DESCRIPTION     (1 << 25)
#define GD_TIME_DAY        (1 << 26)
/* FetchData +32. */
#define GD_SEND_UPDATES    (1 << 0)
#define GD_CANCEL_UPDATES  (1 << 1)
/* Status +20. */
enum { GDS_SEARCHING = 1, GDS_FOUND = 2, GDS_FAILED = 5, GDS_NO_RESULTS = 6 };

#define GEODATA_WORDS 64                /* 256 bytes */

/* Text for the messages: UTF-8 kept as it is, anything else taken as
   Latin-1 (what RISC OS writes) and made UTF-8; cut at a whole character
   to fit n bytes with its terminator. Its length. */
size_t geodata_utf8 (const char *in, char *out, size_t n);
/* UTF-8 from a map program as Latin-1 for Lumen ('?' for what Latin-1
   hasn't; bytes that aren't UTF-8 kept). */
void geodata_latin1 (const char *in, char *out, size_t n);
/* "YYYY:MM:DD HH:MM:SS" as a RISC OS time: 0 on success. */
int geodata_time (const char *taken, unsigned char out[5]);

/* Message_GeoData_TransmitData for a photo (object id = its version).
   flags are the +36 bits to add to those for what it has (GD_DRAW_MAP,
   GD_FILE, GD_MORE_INFO). The block's length is set. */
void geodata_transmit (int *block, int map, int dataset, const cat_geo_hit *h, int flags);

/* Message_GeoData_DatasetDetails for the library: n photos with a place
   within the box (n 0: undeclared, every field but the id 0). */
void geodata_details (int *block, int dataset, int n, int x0, int y0, int x1, int y1,
                      const char *title, const char *description);

/* Message_GeoData_Status: code, how many (or -1), and a message. */
void geodata_status (int *block, int code, int quantity, const char *message);

/* Message_GeoData_FetchData, read. */
typedef struct
{
  int map, dataset, object, flags, what;
  int a, b, c, d;                       /* +36..+48: a box, or a point, radius and bearing */
  int text_at, text_len;                /* the search text: where it is in the sender, its length */
  int page_size, page;
} geodata_fetch;
void geodata_read_fetch (const int *block, geodata_fetch *f);
/* The query it asks (the words are given separately, read from the
   sender). 0 if it can be answered: it wants photos, by a box, a radius,
   an id, or none of them. */
int geodata_query (const geodata_fetch *f, const char *words, cat_geo_query *q);

#endif
