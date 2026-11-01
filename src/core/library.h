/* library: a Lumen library on disc, and the background work on it.

   A library is a directory:
     Catalogue   the SQLite catalogue (catalog.h)
     Thumbs      256-pixel JPEG thumbnails, in 256 subdirectories by id

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
} lib_progress;
void lib_get_progress (library *l, lib_progress *p);

void lib_thumb_path (library *l, cat_id version, char *out, size_t n);

/* Checking that the originals are still where they were (they are marked
   missing if not, and found again if they come back). Starts on opening;
   lib_verify_start starts again (a disc put back in). Each step checks
   files until now() >= deadline; 1 if any changed. */
void lib_verify_start (library *l);
int lib_verifying (library *l);
int lib_verify_step (library *l, unsigned (*now) (void), unsigned deadline);

/* Thumbnails that failed are tried again (the raw plug-in is installed now). */
void lib_retry_failed_thumbs (library *l);

/* Removes versions from the library (cat_remove_versions) and their
   thumbnails. The photos' files are not touched. 0 on success. */
int lib_remove (library *l, const cat_id *versions, int n);

/* Thumbnails were marked to be made again (cat_set_recipe): count again. */
void lib_thumbs_changed (library *l);

/* The last problem met while working (a file that couldn't be read), or "". */
const char *lib_last_error (library *l);

#endif
