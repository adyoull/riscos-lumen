/* Copyright (c) 2026 Andrew Youll
   Released under the MIT licence: see the LICENSE file.
   SPDX-License-Identifier: MIT */
/* prefs: Lumen's Choices, a small text file of "key value" lines in
   <Choices$Write>.Lumen.Choices. Unknown keys are ignored and missing ones
   keep their defaults, so older and newer Lumens can share the file. */
#ifndef LUMEN_PREFS_H
#define LUMEN_PREFS_H

typedef struct
{
  char library[512];                   /* "" = the usual place; used from the next start */
  int export_format;                   /* EXPORT_JPEG, EXPORT_PNG or EXPORT_TIFF */
  int export_quality;                  /* 1-100 */
  int export_size;                     /* longer side, 0 = full size, -1 = fit in the box below */
  int export_box_w, export_box_h;      /* the box */
  int export_colour;                   /* CS_SRGB, CS_ADOBE, CS_P3 */
  int export_sharpen;                  /* EXPORT_SHARP_* */
  char export_names[128];              /* name pattern for several photos */
  int thumb_size;                      /* 0 small, 1 medium, 2 large */
  int verify;                          /* check originals are still there at start */
  int watch_new;                       /* folders imported are watched for new photos */
  int albums_new;                      /* and get albums from the folders inside them */
  int watch_minutes;                   /* how often watched folders are looked through (0: only at start) */
  int previews;                        /* PREVIEWS_* (library.h): 0 off, 1 raw files, 2 all photos */
  /* File types (the icon bar menu): which double-clicks open in Lumen. */
  int open_mode;                       /* OPEN_NEVER, OPEN_RUNNING, OPEN_ALWAYS; -1 not chosen yet */
  char open_types[160];                /* the ticked filetypes, "C85 B60 ..."; "-" none yet */
  int open_data;                       /* Data files named like a photo (while running); -1 not chosen */
} prefs;

enum { OPEN_NEVER, OPEN_RUNNING, OPEN_ALWAYS };

void prefs_default (prefs *p);
/* Reads the file into *p (defaults first). 0 if it was there. */
int prefs_load (const char *path, prefs *p);
/* Writes it (making its directory if need be). 0 on success. */
int prefs_save (const char *path, const prefs *p);

#endif
