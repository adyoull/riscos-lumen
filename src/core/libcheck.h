/* libcheck: checking a library and putting right what is wrong.

   The check reads; nothing is changed:
     - the catalogue file itself (SQLite's integrity check), which a power
       cut or a failing card can damage;
     - rows left pointing at nothing: versions whose photo or project has
       gone, photos with no versions, album entries and keywords of
       versions that have gone, stacks of one or whose pick isn't in them;
     - thumbnails: ones the catalogue says are made but whose files are
       gone, ones that failed, and files for versions no longer there;
     - previews: files that are no original's preview now (its photo has
       gone or changed); repair removes them;
     - originals marked missing (the check of the files themselves is
       lib_verify's, and goes on in the background as ever).

   It works a little at a time (libcheck_step) as the thumbnails are many
   files. Repair keeps a copy of the catalogue first (Catalogue-backup,
   written by SQLite with VACUUM INTO), then mends everything in one
   transaction; damaged indexes are rebuilt (REINDEX). */
#ifndef LUMEN_LIBCHECK_H
#define LUMEN_LIBCHECK_H
#include <stddef.h>
#include "library.h"

typedef struct
{
  int damaged;                 /* the integrity check found problems */
  char damage[200];            /* its first message */
  int versions_no_photo;       /* versions whose original has gone from the catalogue */
  int versions_no_project;     /* versions whose project has gone */
  int lone_originals;          /* photos with no versions (never shown) */
  int bad_album_items;
  int bad_keyword_links;
  int bad_stacks;              /* of one, empty, or the pick elsewhere */
  int thumbs_missing;          /* made, but the file has gone */
  int thumbs_failed;           /* couldn't be made: tried again on repair */
  int thumbs_unused;           /* files for versions that aren't there */
  long long thumbs_unused_bytes;
  int previews, previews_unused; /* previews (library.h), and of those no original uses as it is now */
  long long previews_bytes, previews_unused_bytes;
  int originals_missing;       /* marked missing (not changed by repair) */
  int versions, originals;     /* how many were looked at */
} libcheck_report;

typedef struct libcheck libcheck;

libcheck *libcheck_start (library *l);
/* Works until now() >= deadline (always one step); 1 when finished. */
int libcheck_step (libcheck *k, unsigned (*now) (void), unsigned deadline);
int libcheck_percent (libcheck *k);
const libcheck_report *libcheck_result (libcheck *k);
/* Anything repair would change. */
int libcheck_problems (const libcheck_report *r);
/* The report in a few sentences. */
void libcheck_describe (const libcheck_report *r, char *out, size_t n);
void libcheck_free (libcheck *k);

/* Puts right what a finished check found: 0 done, -1 (err says why). The
   catalogue is copied to Catalogue-backup first. Versions whose project
   has gone, and photos with no versions, go into a project called
   "Recovered" rather than being lost; versions whose photo has gone are
   removed. fixed: what was done, a sentence. */
int libcheck_repair (libcheck *k, char *fixed, size_t fn, char *err, size_t en);

#endif
