/* Copyright (c) 2026 Andrew Youll
   Released under the MIT licence: see the LICENSE file.
   SPDX-License-Identifier: MIT */
/* library: a Lumen library on disc, and the background work on it.

   A library is a directory:
     Catalogue   the SQLite catalogue (catalog.h)
     Thumbs      256-pixel JPEG thumbnails, in 256 subdirectories by id
     Previews    working-size copies of the originals (below), the same way

   Imports and thumbnail-making run as small steps: lib_work() does steps
   until the clock passes a deadline, then returns so the desktop stays
   responsive. Each call commits what it did. */
#ifndef LUMEN_LIBRARY_H
#define LUMEN_LIBRARY_H
#include <stddef.h>
#include "catalog.h"

#define THUMB_SIDE 256

typedef struct library library;

int lib_open (library **out, const char *root, char *err, size_t errlen);
void lib_close (library *l);
cat *lib_cat (library *l);
const char *lib_root (library *l);

/* Queues a file or a directory (searched recursively; application
   directories are skipped) for import. 0 if queued. */
int lib_import (library *l, const char *path);

/* Folders imported from now on are watched, and/or get albums from the
   folders inside them (Choices). */
void lib_set_new_folders (library *l, int watch, int folder_albums);
/* Looks through the watched folders again (queued as quiet imports into
   their own projects; unchanged files aren't read). The number watched. */
int lib_watch_start (library *l);

/* Thumbnails on screen first: the ones asked for with lib_want_thumb (the
   photos on screen) are made first, the latest first, and then the rest.
   The front end decides when there is time for them. */
void lib_set_lazy_thumbs (library *l, int lazy);
void lib_want_thumb (library *l, cat_id version);
/* All that is left to do is thumbnails no one is looking at yet. */
int lib_background_only (library *l);

/* Anything left to do? */
int lib_busy (library *l);

/* What lib_work changed. */
enum { LIB_LIST_CHANGED = 1, LIB_THUMBS_CHANGED = 2 };
/* Called for each thumbnail finished. */
typedef void lib_thumb_fn (void *ctx, cat_id version);
/* Works until now() >= deadline (always at least one step). now() is in
   centiseconds. Returns LIB_* flags. */
int lib_work (library *l, unsigned (*now) (void), unsigned deadline,
              lib_thumb_fn *done, void *ctx);

/* Progress for the title bar. */
typedef struct
{
  int importing;               /* an import is running */
  int found, added, skipped;   /* this import: images found, new, already there or unreadable */
  int relinked;                /* already known, but missing from their old place: found again here */
  int thumbs_pending;
  int watched_only;            /* this import is only of watched folders (a routine look) */
  char what[200];              /* this import: the first file or folder's name */
} lib_progress;
void lib_get_progress (library *l, lib_progress *p);
/* 1 while an import is running or waiting to start. */
int lib_importing (library *l);

void lib_thumb_path (library *l, cat_id version, char *out, size_t n);

/* Checking that the originals are still where they were (they are marked
   missing if not, and found again if they come back). Starts on opening;
   lib_verify_start starts again (a disc put back in). Each step checks
   files until now() >= deadline; 1 if any changed. */
void lib_verify_start (library *l);
int lib_verifying (library *l);
int lib_verify_step (library *l, unsigned (*now) (void), unsigned deadline);
/* The last (or current) check: originals looked at, gone since the last
   check, and back again. */
void lib_verify_counts (library *l, int *checked, int *gone, int *back);

/* Thumbnails that failed are tried again (the raw plug-in is installed now). */
void lib_retry_failed_thumbs (library *l);

/* Removes versions from the library (cat_remove_versions) and their
   thumbnails. The photos' files are not touched. 0 on success. */
int lib_remove (library *l, const cat_id *versions, int n);

/* Thumbnails were marked to be made again (cat_set_recipe): count again. */
void lib_thumbs_changed (library *l);

/* --- Previews: each original at the size the Viewer works on
   (PREVIEW_SIDE), as it comes from the file (turned upright, in sRGB, no
   adjustments), kept as a JPEG in Previews.xx.<original>_<key>. The key
   is worked out from what the picture depends on (the file's size and
   date, its orientation, and for a raw the plug-in), so a preview that is
   out of date is simply not found, and is made again.

   They are made in the background once the thumbnails are done and the
   originals checked, newest first; the Viewer also keeps the picture it
   opened as one. A raw file opens from its preview in a moment rather
   than being developed again by the plug-in. */
#define PREVIEW_SIDE 1600
struct rgb_image;
enum { PREVIEWS_OFF, PREVIEWS_RAW, PREVIEWS_ALL };
void lib_set_previews (library *l, int mode);
int lib_previews (library *l);
/* Is one made for a photo of this format (IMG_*) with the mode set? */
int lib_preview_wanted (library *l, int format, const char *path);
/* Anything to look at or make (lib_preview_work), and not held. */
int lib_previews_busy (library *l);
/* Held while the Viewer, an export or a print is busy: the raw plug-in
   runs one job at a time, and the photo being opened comes first. */
void lib_hold_previews (library *l, int hold);
/* Looks through the originals again (the plug-in has changed). */
void lib_previews_changed (library *l);
/* Works until now() >= deadline: how many previews were made. */
int lib_preview_work (library *l, unsigned (*now) (void), unsigned deadline);
/* Previews still to look at or make (held or not). */
int lib_previews_making (library *l);
/* One being made: 1 and its photo's leaf name. */
int lib_preview_now (library *l, char *leaf, size_t n);
/* The preview being made in the background, if it is of this original:
   handed over (the caller frees it; the picture is the one the preview
   would be). NULL if another or none. */
struct thumbgen *lib_preview_adopt (library *l, cat_id original);
/* The up-to-date preview of an original: 1 and its path, or 0. */
int lib_preview_find (library *l, cat_id original, char *out, size_t n);
/* Keeps img (at most PREVIEW_SIDE, upright) as the original's preview, if
   one is wanted for it: 1 if kept, 0 if not wanted, -1 on error. */
int lib_preview_save (library *l, cat_id original, const struct rgb_image *img);
/* The leaf name an original's preview has now (for Check library). 1 if
   the original is there. */
int lib_preview_leaf (library *l, cat_id original, char *leaf, size_t n);
/* Removes every preview: how many, and their size in bytes. */
int lib_discard_previews (library *l, long long *bytes);

/* The last problem met while working (a file that couldn't be read), or "". */
const char *lib_last_error (library *l);

#endif
