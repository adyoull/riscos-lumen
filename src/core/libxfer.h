/* Copyright (c) 2026 Andrew Youll
   Released under the MIT licence: see the LICENSE file.
   SPDX-License-Identifier: MIT */
/* libxfer: moving photos between libraries.

   Export writes some or all of a library's photos to a new library (a
   directory, as library.h): their versions with every adjustment, ratings,
   flags, labels, captions and keywords, the projects and albums they are
   in, stacks, presets, and the thumbnails. The originals can be copied in
   too (into Originals, a directory for each project, the folders inside
   the project's folder kept); otherwise the new library refers to them
   where they are.

   Import merges another library into this one. Photos already here (the
   same file's content, wherever it is) aren't added twice; a version of
   one with different adjustments comes in as another version. Projects,
   albums, keywords and presets are matched by name; smart albums come in
   if there isn't one of that name. Folder albums and watching stay with
   the library they came from. The photos can be copied into this library
   (Originals, as export) or left where they are.

   Both plan at the start (quick), then copy files a few at a time:
   libx_work() until libx_finished(). The catalogue is written in one
   transaction, all or nothing. */
#ifndef LUMEN_LIBXFER_H
#define LUMEN_LIBXFER_H
#include <stddef.h>
#include "library.h"

typedef struct libxfer libxfer;

/* A directory holding a library (it has a Catalogue)? */
int libx_is_library (const char *dir);

/* Starts exporting versions (n < 0: the whole library) to dst, which
   mustn't exist yet. originals: copy the original files in. */
int libx_export (libxfer **out, library *l, const char *dst, const cat_id *versions, int n,
                 int originals, char *err, size_t errlen);

/* What an import would bring, worked out before starting. */
typedef struct
{
  int photos;                  /* originals in the other library */
  int known;                   /* already in this one */
  int versions;                /* versions in the other library */
  int new_versions;            /* versions that would be added */
  int projects, new_projects;
  int albums, new_albums;      /* albums that come across (not folder albums) and new here */
  int keywords, presets;
  int missing;                 /* new photos whose files can't be found */
  long long bytes;             /* to copy if the photos are copied */
  char name[256];              /* the other library's directory leaf */
} libx_survey;

/* Plans an import of the library at src: the survey is filled, nothing
   is changed until libx_import_go. */
int libx_import_plan (libxfer **out, library *l, const char *src, libx_survey *s,
                      char *err, size_t errlen);
/* copy: copy the new photos into this library's Originals. */
void libx_import_go (libxfer *x, int copy);

/* Works until now() >= deadline (always one step). Returns LIB_LIST_CHANGED
   when the catalogue has been merged, LIB_THUMBS_CHANGED as thumbnails
   arrive. */
int libx_work (libxfer *x, unsigned (*now) (void), unsigned deadline);
int libx_finished (libxfer *x);
int libx_started (libxfer *x);             /* export, or an import after libx_import_go */
int libx_is_export (libxfer *x);
/* Files copied and to copy (percentage), and what it is doing. */
void libx_progress (libxfer *x, int *done, int *total, char *what, size_t n);
/* When finished: what happened, a sentence; failed: 1 if it didn't work. */
void libx_result (libxfer *x, char *out, size_t n, int *failed);
/* What was added (import) or written (export): photos and versions. */
void libx_counts (libxfer *x, int *photos, int *versions);
void libx_free (libxfer *x);

/* The photos (originals) behind versions (n < 0: all of them) and the
   size of their files, for the Export library box. */
int libx_measure (library *l, const cat_id *versions, int n, int *photos, long long *bytes);

/* Opening a library that has moved (a copy on another disc): the originals
   inside it (exported with it) are found in their new place. Called by
   lib_open; 1 if paths changed. */
int libx_rehome (cat *c, const char *root);

#endif
