/* catalog: the library's SQLite catalogue. Originals are files on disc,
   never written; a version is one recipe applied to an original (milestone
   0 makes one version per original and leaves the recipe empty). Every
   version belongs to one project; albums gather versions across projects. */
#ifndef LUMEN_CATALOG_H
#define LUMEN_CATALOG_H
#include <stddef.h>
#include "exif.h"

typedef long long cat_id;
typedef struct cat cat;

#define RATING_REJECT (-1)

int cat_open (cat **out, const char *path, char *err, size_t errlen);
void cat_close (cat *c);
const char *cat_error (cat *c);

/* Wraps many changes in one transaction (much faster on SD cards). */
int cat_begin (cat *c);
int cat_commit (cat *c);

cat_id cat_project_ensure (cat *c, const char *name);
cat_id cat_album_create (cat *c, const char *name);
int cat_album_add (cat *c, cat_id album, const cat_id *versions, int n);
int cat_album_remove (cat *c, cat_id album, const cat_id *versions, int n);

typedef struct
{
  const char *path;
  long long size, mtime;
  unsigned long long hash;
  int filetype;
  long long batch;            /* the import it came in with */
  const img_info *info;
} cat_original;

/* Adds an original and its first version. Returns the version's id, or 0
   if this file (same path, or same size and hash) is already in the
   library (then *existing is set to that version), or -1 on error. */
cat_id cat_add_original (cat *c, cat_id project, const cat_original *o, cat_id *existing);

int cat_set_rating (cat *c, const cat_id *versions, int n, int rating);  /* 0-5 or RATING_REJECT */
int cat_set_flag (cat *c, const cat_id *versions, int n, int flag);      /* 0, 1, or -1 to toggle */

/* What the browser shows. */
enum { SRC_ALL, SRC_FLAGGED, SRC_REJECTED, SRC_LAST_IMPORT, SRC_PROJECT, SRC_ALBUM };
typedef struct
{
  int kind;                   /* SRC_* */
  cat_id id;                  /* project or album */
  int min_rating;             /* 0: everything but rejects (except SRC_REJECTED) */
} cat_source;

typedef struct
{
  cat_id id;                  /* version */
  char name[64];              /* the original's leaf name */
  int rating, flag;
  int thumb_state;            /* THUMB_* */
  int filetype;
  int edited;                 /* has a recipe */
} cat_item;

enum { THUMB_NONE = 0, THUMB_READY = 1, THUMB_FAILED = 2 };

/* Fills a malloc'd array (free it). Sorted by capture time, then name. */
int cat_list (cat *c, const cat_source *src, cat_item **items, int *count);

/* The next version without a thumbnail: its id, the original's path,
   EXIF orientation and recipe. 1 if found, 0 if none, -1 on error. */
int cat_next_thumb (cat *c, cat_id *version, char *path, size_t n, int *orientation,
                    char *recipe, size_t rn);

/* A version's recipe text ("" for none). 0 on success. */
int cat_get_recipe (cat *c, cat_id version, char *out, size_t n);
/* Stores a recipe and marks the thumbnail to be made again. */
int cat_set_recipe (cat *c, cat_id version, const char *recipe);
int cat_set_thumb (cat *c, cat_id version, int state);
int cat_count_thumbs_pending (cat *c);

typedef struct { cat_id id; char name[64]; int count; } cat_group;
/* Projects (kind SRC_PROJECT) or albums (SRC_ALBUM), sorted by name. */
int cat_groups (cat *c, int kind, cat_group **out, int *count);
/* Name of a project or album, or "" */
void cat_group_name (cat *c, int kind, cat_id id, char *out, size_t n);

/* Details for the info line and tests. */
typedef struct
{
  char path[1024];
  int width, height, orientation, iso;
  char taken[20], make[32], model[48];
  double exposure, fnumber, focal;
  cat_id project;
} cat_details;
int cat_get_details (cat *c, cat_id version, cat_details *d);

#endif
