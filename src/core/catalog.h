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
enum { SRC_ALL, SRC_FLAGGED, SRC_REJECTED, SRC_LAST_IMPORT, SRC_PROJECT, SRC_ALBUM, SRC_MISSING };
typedef struct
{
  int kind;                   /* SRC_* */
  cat_id id;                  /* project or album */
  int min_rating;             /* 0: everything but rejects (except SRC_REJECTED) */
  char search[64];            /* words that must all match the name, camera, date or project ("" none) */
} cat_source;

typedef struct
{
  cat_id id;                  /* version */
  char name[64];              /* the original's leaf name */
  int rating, flag;
  int thumb_state;            /* THUMB_* */
  int filetype;
  int edited;                 /* has a recipe */
  int missing;                /* its file can't be found */
  cat_id stack;               /* its stack, 0 for none */
  int stack_count;            /* how many are in that stack */
  int stack_pick;             /* it is the stack's pick (shown when the stack is closed) */
  int stack_open;             /* the stack is open: all shown */
  int label;                  /* colour label, CAT_LABEL_* (0 none) */
} cat_item;

/* Colour labels, as Aperture's: search and smart albums find them by name. */
enum { CAT_LABEL_NONE, CAT_LABEL_RED, CAT_LABEL_ORANGE, CAT_LABEL_YELLOW, CAT_LABEL_GREEN, CAT_LABEL_BLUE,
       CAT_LABEL_PURPLE, CAT_LABEL_GREY, CAT_LABELS };
extern const char *const cat_label_names[CAT_LABELS];
int cat_set_label (cat *c, const cat_id *versions, int n, int label);

enum { THUMB_NONE = 0, THUMB_READY = 1, THUMB_FAILED = 2 };

/* Fills a malloc'd array (free it). Sorted by capture time, then name; a
   stack's photos together, its pick first. In All photos, a project and
   the last import, without a search, a closed stack shows only its pick. */
int cat_list (cat *c, const cat_source *src, cat_item **items, int *count);

/* The next version without a thumbnail: its id, the original's path,
   EXIF orientation and recipe. 1 if found, 0 if none, -1 on error. */
int cat_next_thumb (cat *c, cat_id *version, char *path, size_t n, int *orientation,
                    char *recipe, size_t rn);

/* The same for one version, if it still needs its thumbnail: 1 if so. */
int cat_thumb_source (cat *c, cat_id version, char *path, size_t n, int *orientation, char *recipe, size_t rn);
/* A version's recipe text ("" for none). 0 on success. */
int cat_get_recipe (cat *c, cat_id version, char *out, size_t n);
/* Stores a recipe and marks the thumbnail to be made again. */
int cat_set_recipe (cat *c, cat_id version, const char *recipe);
int cat_set_thumb (cat *c, cat_id version, int state);
int cat_count_thumbs_pending (cat *c);

/* Presets: named looks (recipes without geometry), for the whole library. */
typedef struct { cat_id id; char name[48]; char recipe[2048]; } cat_preset;
/* Saves (or replaces, by name) a preset. Its id, or -1. */
cat_id cat_preset_save (cat *c, const char *name, const char *look);
int cat_presets (cat *c, cat_preset **out, int *count);   /* sorted by name; free it */
int cat_preset_delete (cat *c, cat_id id);
/* Gives each version the look in text (a preset or copied adjustments),
   keeping its own geometry; thumbnails are made again. */
int cat_apply_look (cat *c, const cat_id *versions, int n, const char *look);

/* Originals whose files have gone (moved, renamed, a disc not there).
   cat_original_next walks them in id order (after 0 for the first): 1 and
   the next, 0 at the end. */
int cat_original_next (cat *c, cat_id after, cat_id *id, char *path, size_t n, int *missing);
int cat_set_missing (cat *c, cat_id original, int missing);
/* A version's original is at path now: no longer missing. */
int cat_relink (cat *c, cat_id version, const char *path, long long mtime);
int cat_count_missing (cat *c);        /* versions */
int cat_tidy (cat *c);                 /* projects with no photos go */
int cat_retry_failed_thumbs (cat *c);  /* failed thumbnails: to be made again */
/* A raw file's details from the plug-in (size, orientation, camera...). */
int cat_set_original_info (cat *c, cat_id version, const img_info *info);

/* Takes versions out of the library: their album places, their originals'
   records when no other version uses them, and projects left empty. The
   files on disc are never touched. 0 on success. */
int cat_remove_versions (cat *c, const cat_id *versions, int n);
/* The versions in a project or album (malloc'd). */
int cat_group_versions (cat *c, int kind, cat_id id, cat_id **out, int *count);
/* An album goes (its photos stay in the library). */
int cat_album_delete (cat *c, cat_id album);
/* A project's or album's new name; project names are unique (-1, with a
   message in cat_error, if taken or empty). */
int cat_group_rename (cat *c, int kind, cat_id id, const char *name);

typedef struct { cat_id id; char name[64]; int count; int smart; } cat_group;
/* Projects (kind SRC_PROJECT) or albums (SRC_ALBUM, your own, not folder
   albums; smart ones with their count of photos now), sorted by name. */
int cat_groups (cat *c, int kind, cat_group **out, int *count);
/* Name of a project or album, or "" */
void cat_group_name (cat *c, int kind, cat_id id, char *out, size_t n);

/* --- A project's folder (the directory it was imported from), watching it
   for new photos, and albums made from the folders inside it. */
/* Records the folder a project came from (if it has none yet). */
int cat_project_set_root (cat *c, cat_id project, const char *root);
/* A project's folder: the one recorded, or else the directory all its
   photos are in. 0 if there is one. */
int cat_project_root (cat *c, cat_id project, char *out, size_t n);
int cat_project_watching (cat *c, cat_id project, int *watch, int *folder_albums);
/* Sets both (the folder is recorded first); albums turned off go at once. */
int cat_project_set_watching (cat *c, cat_id project, int watch, int folder_albums);
typedef struct { cat_id project; char root[1024]; } cat_watch;
int cat_watched (cat *c, cat_watch **out, int *count);   /* the watched projects (malloc'd) */
/* A file already in the library, unchanged (same path, size and date):
   1, its original and whether it was marked missing. */
int cat_known_file (cat *c, const char *path, long long size, long long mtime, cat_id *original, int *missing);
/* The project's folder albums made again from where its files are: one
   album for each folder inside the project's folder that has photos in it
   or below it, holding all of those. With folder albums off, they go. */
int cat_sync_folder_albums (cat *c, cat_id project);
int cat_sync_all_folder_albums (cat *c);
/* A project's folder albums, in folder order (nested ones after their
   parent), with their depth (1 for a folder in the project's own). */
typedef struct { cat_id id; char name[64]; int count, depth; } cat_folder;
int cat_folder_albums (cat *c, cat_id project, cat_folder **out, int *count);
int cat_album_is_folder (cat *c, cat_id album);   /* made from a folder (not edited by hand) */

/* --- Captions and keywords (schema 4). A caption is a line of text about
   the photo; keywords are words or short phrases (any case kept, matched
   without it), given as a list separated by commas. */
#define CAT_CAPTION_MAX 512
#define CAT_KEYWORDS_MAX 1024
int cat_set_caption (cat *c, const cat_id *versions, int n, const char *caption);
/* Adds or takes away the keywords in list (commas between) for each version. */
int cat_add_keywords (cat *c, const cat_id *versions, int n, const char *list);
int cat_remove_keywords (cat *c, const cat_id *versions, int n, const char *list);
/* A version's caption and keywords ("a, b, c", sorted). 0 on success. */
int cat_get_tags (cat *c, cat_id version, char *caption, size_t cn, char *keywords, size_t kn);
/* The keywords every one of the versions has, and the caption if all have
   the same one (*same 1; else "" and 0). */
int cat_common_tags (cat *c, const cat_id *versions, int n, char *caption, size_t cn, int *same,
                     char *keywords, size_t kn);
/* All the keywords in use, with how many photos have each (malloc'd), by name. */
typedef struct { char name[64]; int count; } cat_keyword;
int cat_keywords (cat *c, cat_keyword **out, int *count);
/* Splits a keyword list: calls fn for each keyword, trimmed, without
   duplicates (ignoring case), at most 63 characters each. */
void cat_keyword_split (const char *list, void (*fn) (void *ctx, const char *kw), void *ctx);

/* --- Smart albums: an album whose photos are the ones that match its rule
   now, kept up to date as photos change. */
typedef struct
{
  char words[64];              /* all must match name, camera, date, project, caption or a keyword */
  int min_rating;              /* 0-5 */
  int flagged;                 /* only flagged photos */
  int edited;                  /* only adjusted ones */
  char from[11], to[11];       /* taken from / to, YYYY-MM-DD ("" for no limit) */
} cat_rule;
cat_id cat_smart_create (cat *c, const char *name, const cat_rule *r);
/* 1 and its rule if the album is smart, 0 if not, -1 on error. */
int cat_smart_get (cat *c, cat_id album, cat_rule *r);
int cat_smart_set (cat *c, cat_id album, const cat_rule *r);
/* The rule as text, and back (unknown lines ignored). */
void cat_rule_format (const cat_rule *r, char *out, size_t n);
void cat_rule_parse (const char *text, cat_rule *r);
/* A date as YYYY-MM-DD from what was typed (2026-5-3, 3/5/2026, 2026): 0 if
   it makes one ("" stays ""); with end set, a bare year or month means its end. */
int cat_date_normal (const char *in, int end, char out[11]);

/* --- Versions and stacks (schema 5). A version is one way of developing an
   original; an original can have several. A stack gathers versions (of one
   original, or a burst or bracket of several) under one, the pick, which
   stands for them all while the stack is closed. */
/* A new version of the original version v is of: with its adjustments
   (keep_look) or as the original; its caption, keywords and albums too.
   It is stacked with v (open, so it shows). Its id, or -1. */
cat_id cat_new_version (cat *c, cat_id v, int keep_look);
/* The versions into one stack (theirs before are merged): pick is its pick
   if it is among them, else the first. The stack's id, or -1. */
cat_id cat_stack (cat *c, const cat_id *v, int n, cat_id pick);
/* Takes the versions out of their stacks (a stack left with one goes). */
int cat_unstack (cat *c, const cat_id *v, int n);
/* Takes their stacks apart: every version in them, wherever it is shown. */
int cat_unstack_all (cat *c, const cat_id *v, int n);
int cat_stack_pick (cat *c, cat_id v);                 /* v becomes its stack's pick */
int cat_stack_open (cat *c, cat_id v, int open);       /* v's stack: 1 open, 0 closed, -1 the other */
/* Stacks the versions taken within seconds of the one before (bursts,
   brackets); those with no date are left. How many stacks were made. */
int cat_auto_stack (cat *c, const cat_id *v, int n, int seconds);
int cat_count_versions (cat *c, cat_id original);      /* versions of an original */

/* Details for the info line and tests. */
typedef struct
{
  char path[1024];
  int width, height, orientation, iso;
  char taken[20], make[32], model[48];
  double exposure, fnumber, focal;
  cat_id project;
  int missing;                /* the file can't be found (cat_set_missing) */
  cat_id original;
  int rating;                 /* 0-5, or RATING_REJECT */
  char project_name[64];
  int versions;               /* of its original (1 if just this) */
  int version_no;             /* this one's place among them, from 1 */
} cat_details;
int cat_get_details (cat *c, cat_id version, cat_details *d);

#endif
