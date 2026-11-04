/* Copyright (c) 2026 Andrew Youll
   Released under the MIT licence: see the LICENSE file.
   SPDX-License-Identifier: MIT */
/* library.c: see library.h. */
#include <stdio.h>
#include <stdlib.h>
#include <ctype.h>
#include <string.h>
#include <strings.h>
#include <time.h>
#include "exif.h"
#include "fsys.h"
#include "library.h"
#include "libxfer.h"
#include "xmp.h"
#include "thumbgen.h"
#include "develop.h"
#include "rawjob.h"

#define HEAD_BYTES (128 * 1024)       /* enough for EXIF in nearly every camera's files */
#define HASH_BYTES (64 * 1024)
#define ROWS_PER_STEP 16
#define LIB_WANT_MAX 256

typedef struct root_req
{
  struct root_req *next;
  char path[1024];
  cat_id project;                      /* a watched project's folder; 0: a project named after it */
} root_req;

struct library
{
  char root[1024];
  cat *db;
  /* import */
  root_req *roots, *roots_tail;
  char **stack;                       /* directories still to read */
  int nstack, capstack;
  fsys_dir *dir;
  char dirpath[1024];
  cat_id project;
  long long batch;
  int found, added, skipped, importing, relinked;
  int watch_new, albums_new;           /* folders imported from now on are watched / get folder albums */
  /* checking that originals are still there */
  int verifying;
  cat_id verify_after;
  unsigned char *head;
  /* thumbnails */
  thumbgen *tg;
  cat_id tg_version;
  char tg_recipe[RECIPE_TEXT_MAX];               /* applied when the thumbnail is finished */
  char tg_dst[1100];
  int thumbs_pending;                 /* cached count; -1 to recount */
  /* Thumbnails on screen first: the ones asked for (the photos on screen),
     newest request first, then the rest. The front end decides when (and
     how much) to call lib_work. */
  int lazy;
  int tg_wanted;
  int watched_only;
  char what[200];
  int checked, gone_now, back_now;                      /* the one being made was asked for */
  cat_id want[LIB_WANT_MAX];
  int nwant;
  unsigned char shard_made[256];
  /* previews */
  int pv_mode, pv_hold, pv_done;
  cat_id pv_below;                     /* looked through down to (not including) this original */
  thumbgen *pv_tg;
  cat_id pv_original;
  char pv_leaf[64];                    /* its file's leaf name, for the status bar */
  unsigned char pv_shard_made[256];
  char err[256];
};

int lib_open (library **out, const char *root, char *err, size_t errlen)
{
  library *l = calloc (1, sizeof *l);
  char path[1100];
  *out = NULL;
  if (!l || !(l->head = malloc (HEAD_BYTES)))
    {
      free (l);
      snprintf (err, errlen, "Not enough memory");
      return -1;
    }
  if (fsys_canonical (root, l->root, sizeof l->root) || !l->root[0])
    snprintf (l->root, sizeof l->root, "%s", root);
  if (!l->root[0] || l->root[0] == '.')
    {
      snprintf (err, errlen, "Lumen doesn't know where to keep its library (\"%s\"). Set Lumen$Library to a directory, e.g. *Set Lumen$Library SDFS::RISCOSpi.$.Photos.Library", root);
      lib_close (l);
      return -1;
    }
  fsys_join (path, sizeof path, l->root, "Thumbs");
  if (fsys_mkdirs (path))
    {
      snprintf (err, errlen, "Can't create the library at %s", l->root);
      lib_close (l);
      return -1;
    }
  fsys_join (path, sizeof path, l->root, "Catalogue");
  if (cat_open (&l->db, path, err, errlen))
    {
      char e2[300];
      snprintf (e2, sizeof e2, "%s (the library is %s)", err, l->root);
      snprintf (err, errlen, "%s", e2);
      lib_close (l);
      return -1;
    }
  libx_rehome (l->db, l->root);      /* moved, with originals inside it */
  l->thumbs_pending = -1;
  lib_verify_start (l);
  *out = l;
  return 0;
}

/* --- checking the originals are there ---------------------------------- */

void lib_verify_start (library *l)
{
  cat_id id;
  char path[1024];
  int missing;
  l->verify_after = 0;
  l->checked = l->gone_now = l->back_now = 0;
  /* Nothing to check in an empty library: no idle time wanted. */
  l->verifying = cat_original_next (l->db, 0, &id, path, sizeof path, &missing) == 1;
}

int lib_verifying (library *l) { return l->verifying; }

void lib_verify_counts (library *l, int *checked, int *gone, int *back)
{
  *checked = l->checked;
  *gone = l->gone_now;
  *back = l->back_now;
}

int lib_verify_step (library *l, unsigned (*now) (void), unsigned deadline)
{
  int changed = 0;
  do
    {
      cat_id id;
      char path[1024];
      int missing, r = cat_original_next (l->db, l->verify_after, &id, path, sizeof path, &missing);
      fsys_entry e;
      int gone;
      if (r != 1)
        {
          l->verifying = 0;
          break;
        }
      l->verify_after = id;
      l->checked++;
      gone = fsys_stat (path, &e) != 0;
      if (gone != missing)
        {
          if (gone)
            l->gone_now++;
          else
            l->back_now++;
          cat_set_missing (l->db, id, gone);
          changed = 1;
        }
    }
  while ((int) (now () - deadline) < 0);
  return changed;
}

void lib_close (library *l)
{
  if (!l)
    return;
  while (l->roots)
    {
      root_req *r = l->roots;
      l->roots = r->next;
      free (r);
    }
  while (l->nstack > 0)
    free (l->stack[--l->nstack]);
  free (l->stack);
  fsys_closedir (l->dir);
  thumbgen_free (l->tg);
  thumbgen_free (l->pv_tg);
  cat_close (l->db);
  free (l->head);
  free (l);
}

cat *lib_cat (library *l) { return l->db; }
const char *lib_root (library *l) { return l->root; }
const char *lib_last_error (library *l) { return l->err; }

static int queue_root (library *l, const char *path, cat_id project)
{
  root_req *r = calloc (1, sizeof *r);
  if (!r)
    return -1;
  snprintf (r->path, sizeof r->path, "%s", path);
  r->project = project;
  if (l->roots_tail)
    l->roots_tail->next = r;
  else
    l->roots = r;
  l->roots_tail = r;
  return 0;
}

int lib_import (library *l, const char *path)
{
  return queue_root (l, path, 0);
}

void lib_set_new_folders (library *l, int watch, int folder_albums)
{
  l->watch_new = watch;
  l->albums_new = folder_albums;
}

int lib_watch_start (library *l)
{
  cat_watch *w;
  int n = 0, k;
  root_req *r;
  if (cat_watched (l->db, &w, &n))
    return 0;
  for (k = 0; k < n; k++)
    {
      /* Not if it is waiting already (a slow disc, a long import). */
      for (r = l->roots; r; r = r->next)
        if (r->project == w[k].project)
          break;
      if (!r)
        queue_root (l, w[k].root, w[k].project);
    }
  free (w);
  return n;
}

void lib_set_lazy_thumbs (library *l, int lazy)
{
  l->lazy = lazy;
  l->thumbs_pending = -1;
}

void lib_want_thumb (library *l, cat_id v)
{
  int k;
  if (!l->lazy || v <= 0 || (l->tg && l->tg_version == v))
    return;
  for (k = 0; k < l->nwant; k++)
    if (l->want[k] == v)
      {
        /* Asked again: to the front (it is on screen now). */
        memmove (&l->want[k], &l->want[k + 1], sizeof *l->want * (size_t) (l->nwant - k - 1));
        l->want[l->nwant - 1] = v;
        return;
      }
  if (l->nwant == LIB_WANT_MAX)
    {
      /* The oldest request goes: it has scrolled out of sight long ago. */
      memmove (&l->want[0], &l->want[1], sizeof *l->want * (LIB_WANT_MAX - 1));
      l->nwant--;
    }
  l->want[l->nwant++] = v;
}

int lib_background_only (library *l)
{
  if (!l->lazy || l->roots || l->nstack || l->dir || l->nwant > 0)
    return 0;
  if (l->tg)
    return !l->tg_wanted;
  if (l->thumbs_pending < 0)
    l->thumbs_pending = cat_count_thumbs_pending (l->db);
  return l->thumbs_pending > 0;
}

int lib_busy (library *l)
{
  if (l->roots || l->nstack || l->dir || l->tg)
    return 1;
  if (l->lazy && l->nwant > 0)
    return 1;
  if (l->thumbs_pending < 0)
    l->thumbs_pending = cat_count_thumbs_pending (l->db);
  return l->thumbs_pending > 0;
}

void lib_get_progress (library *l, lib_progress *p)
{
  p->importing = l->importing;
  p->found = l->found;
  p->added = l->added;
  p->skipped = l->skipped;
  p->relinked = l->relinked;
  p->watched_only = l->watched_only;
  snprintf (p->what, sizeof p->what, "%s", l->what);
  if (l->thumbs_pending < 0)
    l->thumbs_pending = cat_count_thumbs_pending (l->db);
  p->thumbs_pending = l->thumbs_pending;
}

void lib_thumbs_changed (library *l)
{
  l->thumbs_pending = -1;
}

void lib_thumb_path (library *l, cat_id v, char *out, size_t n)
{
  char shard[8], dir[1100], leaf[24];
  snprintf (shard, sizeof shard, "%02x", (unsigned) (v & 0xFF));
  fsys_join (dir, sizeof dir, l->root, "Thumbs");
  fsys_join (out, n, dir, shard);
  snprintf (dir, sizeof dir, "%s", out);
  snprintf (leaf, sizeof leaf, "%lld", v);
  fsys_join (out, n, dir, leaf);
}

void lib_retry_failed_thumbs (library *l)
{
  cat_retry_failed_thumbs (l->db);
  l->thumbs_pending = -1;
}

static void remove_previews_of_gone (library *l, const cat_id *originals, int n);

int lib_remove (library *l, const cat_id *v, int n)
{
  int i;
  cat_id *orig = malloc (sizeof *orig * (size_t) (n > 0 ? n : 1));
  for (i = 0; orig && i < n; i++)
    {
      cat_details d;
      orig[i] = cat_get_details (l->db, v[i], &d) == 0 ? d.original : 0;
    }
  if (cat_remove_versions (l->db, v, n))
    {
      free (orig);
      return -1;
    }
  if (orig)
    remove_previews_of_gone (l, orig, n);
  free (orig);
  for (i = 0; i < n; i++)
    {
      char path[1100];
      lib_thumb_path (l, v[i], path, sizeof path);
      remove (path);
    }
  lib_thumbs_changed (l);
  return 0;
}

/* --- import ------------------------------------------------------------ */

static int push_dir (library *l, const char *path)
{
  if (l->nstack == l->capstack)
    {
      int cap = l->capstack ? l->capstack * 2 : 16;
      char **s = realloc (l->stack, sizeof *s * cap);
      if (!s)
        return -1;
      l->stack = s;
      l->capstack = cap;
    }
  l->stack[l->nstack] = strdup (path);
  if (!l->stack[l->nstack])
    return -1;
  l->nstack++;
  return 0;
}

/* FNV-1a, 64 bit. */
static unsigned long long fnv (unsigned long long h, const unsigned char *p, size_t n)
{
  while (n--)
    {
      h ^= *p++;
      h *= 1099511628211ULL;
    }
  return h;
}

/* Whether the name ends /jpg, /jpeg, /jpe or /png (.png elsewhere), in
   any case; a ",xxx" filetype suffix (files from other systems) is
   ignored. */
static int has_photo_extension (const char *path)
{
  static const char *const ext[] = { "jpg", "jpeg", "jpe", "png" };
  const char *dot = NULL, *end, *q;
  size_t n, k;
  if (!path)
    return 0;
  end = path + strlen (path);
  if (end - path > 4 && end[-4] == ',' && isxdigit ((unsigned char) end[-3]) && isxdigit ((unsigned char) end[-2])
      && isxdigit ((unsigned char) end[-1]))
    end -= 4;
  for (q = path; q < end; q++)
    if (*q == '/' || *q == '.')
      dot = q;
  if (!dot)
    return 0;
  n = (size_t) (end - dot - 1);
  for (k = 0; k < sizeof ext / sizeof ext[0]; k++)
    if (n == strlen (ext[k]) && strncasecmp (dot + 1, ext[k], n) == 0)
      return 1;
  return 0;
}

/* Which files are worth opening to look at. JPEG and PNG files are known
   by their type, or by their name when they're typed otherwise (DOS or
   Text from a card or a network share without MimeMap); raw files come
   typed every way (DOS, Text, Data, TIFF or nothing), so their extension
   lets any type through. The contents decide in the end. */
static int likely_image_type (int ft, const char *path)
{
  return ft == FT_JPEG || ft == FT_PNG || ft == FT_NONE || ft == FT_DATA || ft == FT_TIFF
         || has_photo_extension (path) || img_has_raw_extension (path) || rawjob_claims_name (path);
}

/* Imports one file. Unreadable or non-image files are skipped quietly
   (counted only if they looked like images). */
static void import_file (library *l, const char *path, const fsys_entry *e)
{
  FILE *f;
  size_t got;
  img_info info;
  cat_original o;
  cat_id existing, v;
  unsigned long long h = 14695981039346656037ULL;
  int fmt;
  if (!likely_image_type (e->filetype, path))
    return;
  {
    /* Known and unchanged: nothing to read (looking through a watched
       folder again is quick). Back in its place if it was missing. */
    cat_id orig;
    int missing;
    if (cat_known_file (l->db, path, e->size, e->mtime, &orig, &missing))
      {
        if (missing)
          {
            cat_set_missing (l->db, orig, 0);
            l->relinked++;
          }
        else
          l->skipped++;                /* already there */
        return;
      }
  }
  f = fopen (path, "rb");
  if (!f)
    {
      snprintf (l->err, sizeof l->err, "Can't open %s", path);
      l->skipped++;
      return;
    }
  got = fread (l->head, 1, HEAD_BYTES, f);
  fmt = img_probe_named (l->head, got, path);
  if (fmt == IMG_UNKNOWN && rawjob_kind_for (IMG_UNKNOWN, path, l->head, got) > 0)
    fmt = IMG_PLUGIN;                  /* a format plug-in claims it */
  if (fmt == IMG_UNKNOWN)
    {
      if (e->filetype == FT_JPEG || e->filetype == FT_PNG || has_photo_extension (path))
        {
          snprintf (l->err, sizeof l->err, "%s isn't a readable image", fsys_leaf (path));
          l->skipped++;
        }
      fclose (f);
      return;
    }
  l->found++;
  if (fmt == IMG_RAW)
    img_read_raw_info (l->head, got, &info);   /* the rest comes from the plug-in with the thumbnail */
  else if (fmt == IMG_PLUGIN)
    {
      /* A format plug-in claims it: all its details come from the plug-in
         with the thumbnail. */
      memset (&info, 0, sizeof info);
      info.format = IMG_PLUGIN;
      info.orientation = 1;
    }
  else if (img_read_info (l->head, got, &info))
    {
      snprintf (l->err, sizeof l->err, "%s: can't read its size", fsys_leaf (path));
      l->skipped++;
      fclose (f);
      return;
    }
  /* A caption and keywords given elsewhere (an XMP packet near the start). */
  char xcap[CAT_CAPTION_MAX], xkw[CAT_KEYWORDS_MAX];
  xmp_read (l->head, got, xcap, sizeof xcap, xkw, sizeof xkw);
  /* Content hash: the size, the first 64 KB and the last 64 KB. */
  h = fnv (h, (const unsigned char *) &e->size, sizeof e->size);
  h = fnv (h, l->head, got < HASH_BYTES ? got : HASH_BYTES);
  if (e->size > 2 * HASH_BYTES && fseek (f, -HASH_BYTES, SEEK_END) == 0)
    {
      size_t tail = fread (l->head, 1, HASH_BYTES, f);
      h = fnv (h, l->head, tail);
    }
  fclose (f);
  h &= 0x7FFFFFFFFFFFFFFFULL;          /* SQLite integers are signed */

  memset (&o, 0, sizeof o);
  o.path = path;
  o.size = e->size;
  o.mtime = e->mtime;
  o.hash = h;
  /* JPEG and PNG files as what they are, however they were typed. */
  o.filetype = info.format == IMG_JPEG ? FT_JPEG : info.format == IMG_PNG ? FT_PNG
             : e->filetype != FT_NONE ? e->filetype : FT_DATA;
  o.batch = l->batch;
  o.info = &info;
  if (!l->project)
    {
      /* A single file: its directory names the project. */
      char parent[1024], *leaf;
      snprintf (parent, sizeof parent, "%s", path);
      leaf = (char *) fsys_leaf (parent);
      if (leaf > parent)
        leaf[-1] = '\0';
      l->project = cat_project_ensure (l->db, leaf > parent ? fsys_leaf (parent) : "Imported");
    }
  v = cat_add_original (l->db, l->project, &o, &existing);
  if (v > 0)
    {
      if (xcap[0])
        cat_set_caption (l->db, &v, 1, xcap);
      if (xkw[0])
        cat_add_keywords (l->db, &v, 1, xkw);
      l->added++;
      if (l->thumbs_pending >= 0)
        l->thumbs_pending++;
    }
  else if (v == 0)
    {
      /* Already known. If the one we know has gone from its old place,
         this is it, moved: its new place (no duplicate). */
      cat_details d;
      fsys_entry old;
      if (cat_get_details (l->db, existing, &d) == 0 && strcmp (d.path, path) != 0
          && fsys_stat (d.path, &old) != 0 && cat_relink (l->db, existing, path, e->mtime) == 0)
        {
          l->relinked++;
          l->thumbs_pending = -1;
        }
      else
        l->skipped++;
    }
  else
    snprintf (l->err, sizeof l->err, "%s", cat_error (l->db));
}

/* One import step: one directory entry, or opening the next directory or
   root. Returns 1 if the list changed, 0 if not, -1 when idle. */
static int import_step (library *l)
{
  fsys_entry e;
  if (l->dir)
    {
      int r = fsys_readdir (l->dir, &e);
      if (r == 1)
        {
          char full[1300];
          fsys_join (full, sizeof full, l->dirpath, e.name);
          if (e.is_dir)
            {
              if (e.name[0] != '!')    /* application directories aren't photos */
                push_dir (l, full);
              return 0;
            }
          {
            int before = l->added;
            import_file (l, full, &e);
            return l->added != before;
          }
        }
      fsys_closedir (l->dir);
      l->dir = NULL;
      return 0;
    }
  if (l->nstack)
    {
      char *p = l->stack[--l->nstack];
      snprintf (l->dirpath, sizeof l->dirpath, "%s", p);
      free (p);
      l->dir = fsys_opendir (l->dirpath);
      if (!l->dir)
        snprintf (l->err, sizeof l->err, "Can't read the directory %s", l->dirpath);
      return 0;
    }
  if (l->roots)
    {
      root_req *r = l->roots;
      static int counter;
      l->roots = r->next;
      if (!l->roots)
        l->roots_tail = NULL;
      if (!l->importing)
        {
          l->found = l->added = l->skipped = l->relinked = 0;
          l->importing = 1;
          l->watched_only = 1;
          snprintf (l->what, sizeof l->what, "%s", fsys_leaf (r->path));
          l->batch = (long long) time (NULL) * 100 + (++counter % 100);
        }
      if (!r->project)
        l->watched_only = 0;
      if (fsys_stat (r->path, &e) != 0)
        {
          if (!r->project)              /* a watched folder's disc may simply be away */
            snprintf (l->err, sizeof l->err, "Can't find %s", r->path);
          l->project = 0;
        }
      else if (e.is_dir)
        {
          if (r->project)
            l->project = r->project;   /* a watched folder: into its own project, whatever it is called now */
          else
            {
              int watch = 0, albums = 0;
              l->project = cat_project_ensure (l->db, fsys_leaf (r->path));
              cat_project_set_root (l->db, l->project, r->path);
              cat_project_watching (l->db, l->project, &watch, &albums);
              if ((l->watch_new && !watch) || (l->albums_new && !albums))
                cat_project_set_watching (l->db, l->project, watch || l->watch_new, albums || l->albums_new);
            }
          push_dir (l, r->path);
        }
      else
        {
          int before = l->added;
          l->project = 0;
          import_file (l, r->path, &e);
          free (r);
          return l->added != before;
        }
      free (r);
      return 0;
    }
  if (l->importing)
    {
      l->importing = 0;
      cat_tidy (l->db);                  /* a project made for photos that were all relinked */
      cat_sync_all_folder_albums (l->db);   /* folder albums follow what came in (and what moved) */
      lib_previews_changed (l);          /* new photos to make previews of */
      return 1;                          /* finished: the counts are final */
    }
  return -1;
}

/* --- thumbnails ---------------------------------------------------------- */

/* One thumbnail step. 1 if a thumbnail finished (done called), 0 if
   working, -1 when idle. */
static int thumb_step (library *l, lib_thumb_fn *done, void *ctx)
{
  int r;
  if (l->tg && l->lazy && !l->tg_wanted && l->nwant > 0)
    {
      /* A photo has come on screen: put the background one down (it is
         still wanted, so it is picked up again later). */
      thumbgen_free (l->tg);
      l->tg = NULL;
    }
  if (!l->tg)
    {
      char src[1024], dst[1100], e[200];
      int orient = 1, found = 0;
      cat_id v = 0;
      l->tg_wanted = 0;
      if (l->lazy)
        {
          /* The newest request that still needs one. */
          while (l->nwant > 0 && !found)
            {
              v = l->want[--l->nwant];
              found = cat_thumb_source (l->db, v, src, sizeof src, &orient, l->tg_recipe, sizeof l->tg_recipe) == 1;
            }
          l->tg_wanted = found;
        }
      if (found)
        ;
      else if (cat_next_thumb (l->db, &v, src, sizeof src, &orient, l->tg_recipe, sizeof l->tg_recipe) != 1)
        {
          l->thumbs_pending = 0;
          return -1;
        }
      if (!l->shard_made[v & 0xFF])
        {
          char dir[1100];
          lib_thumb_path (l, v, dst, sizeof dst);
          snprintf (dir, sizeof dir, "%s", dst);
          *(char *) fsys_leaf (dir) = '\0';
          dir[strlen (dir) - 1] = '\0';      /* drop the separator */
          fsys_mkdirs (dir);
          l->shard_made[v & 0xFF] = 1;
        }
      lib_thumb_path (l, v, dst, sizeof dst);
      snprintf (l->tg_dst, sizeof l->tg_dst, "%s", dst);
      /* With a recipe, take the pixels, develop them, then save. */
      l->tg = thumbgen_start (src, l->tg_recipe[0] ? NULL : dst, THUMB_SIDE, orient, e, sizeof e);
      if (!l->tg)
        {
          snprintf (l->err, sizeof l->err, "%s: %s", fsys_leaf (src), e);
          cat_set_thumb (l->db, v, THUMB_FAILED);
          if (l->thumbs_pending > 0)
            l->thumbs_pending--;
          return 0;
        }
      l->tg_version = v;
      return 0;
    }
  r = thumbgen_step (l->tg, ROWS_PER_STEP);
  if (r == 0)
    return 0;
  if (r < 0)
    snprintf (l->err, sizeof l->err, "Thumbnail: %s", thumbgen_error (l->tg));
  else if (l->tg_recipe[0])
    {
      rgb_image img;
      recipe rc;
      develop *d = malloc (sizeof *d);
      char e[200] = "Not enough memory";
      r = -1;
      if (d && thumbgen_take (l->tg, &img) == 0)
        {
          recipe_parse (&rc, l->tg_recipe);
          r = develop_image (&rc, &img);
          if (r == 0)
            r = img_save_jpeg (&img, l->tg_dst, 85, e, sizeof e) == 0 ? 1 : -1;
          if (r < 0)
            snprintf (l->err, sizeof l->err, "Thumbnail: %s", e);
          free (img.rgb);
        }
      free (d);
    }
  {
    /* A raw file: its size, orientation and camera, from the plug-in. */
    img_info ri;
    if (r > 0 && thumbgen_raw_details (l->tg, &ri))
      cat_set_original_info (l->db, l->tg_version, &ri);
  }
  cat_set_thumb (l->db, l->tg_version, r > 0 ? THUMB_READY : THUMB_FAILED);
  if (l->thumbs_pending > 0)
    l->thumbs_pending--;
  thumbgen_free (l->tg);
  l->tg = NULL;
  if (done && r > 0)
    done (ctx, l->tg_version);
  return 1;
}

int lib_work (library *l, unsigned (*now) (void), unsigned deadline,
              lib_thumb_fn *done, void *ctx)
{
  int changed = 0, first = 1, in_txn = 0;
  while (first || (int) (now () - deadline) < 0)
    {
      int r;
      first = 0;
      if (!in_txn)
        in_txn = cat_begin (l->db) == 0;
      r = import_step (l);
      if (r > 0)
        changed |= LIB_LIST_CHANGED;
      if (r >= 0)
        continue;
      r = thumb_step (l, done, ctx);
      if (r > 0)
        changed |= LIB_THUMBS_CHANGED;
      if (r < 0)
        break;                           /* nothing left */
    }
  if (in_txn)
    cat_commit (l->db);
  return changed;
}

/* --- previews ------------------------------------------------------------ */

#define PREVIEW_QUALITY 92
#define PREVIEW_LOOKS 64                 /* originals looked at in one step at most */

void lib_set_previews (library *l, int mode)
{
  if (mode != PREVIEWS_OFF && mode != PREVIEWS_ALL)
    mode = PREVIEWS_RAW;
  if (l->pv_mode != mode)
    {
      l->pv_mode = mode;
      lib_previews_changed (l);
    }
}

int lib_previews (library *l) { return l->pv_mode; }

void lib_hold_previews (library *l, int hold) { l->pv_hold = hold; }

void lib_previews_changed (library *l)
{
  cat_origin o;
  l->pv_below = 0;
  /* An empty library: nothing to look through (no null events wanted). */
  l->pv_done = cat_origin_below (l->db, 0, &o) != 1;
}

int lib_preview_wanted (library *l, int format, const char *path)
{
  if (img_is_plugin_format (format))
    return l->pv_mode != PREVIEWS_OFF && rawjob_available_kind (rawjob_kind_for (format, path, NULL, 0));
  return l->pv_mode == PREVIEWS_ALL && (format == IMG_JPEG || format == IMG_PNG);
}

int lib_previews_busy (library *l)
{
  return l && (l->pv_tg || (!l->pv_done && l->pv_mode != PREVIEWS_OFF)) && !l->pv_hold;
}

/* What a preview depends on, as a number. */
static unsigned preview_key (const cat_origin *o)
{
  char text[160];
  const char *c;
  unsigned x = 2166136261u;
  snprintf (text, sizeof text, "1 %d %lld %lld %d %u", PREVIEW_SIDE, o->size, o->mtime, o->orientation,
            img_is_plugin_format (o->format) ? rawjob_stamp_kind (rawjob_kind_for (o->format, o->path, NULL, 0)) : 0u);
  for (c = text; *c; c++)
    x = (x ^ (unsigned char) *c) * 16777619u;
  return x;
}

static void preview_dir (library *l, cat_id original, char *out, size_t n)
{
  char shard[8], dir[1100];
  snprintf (shard, sizeof shard, "%02x", (unsigned) (original & 0xFF));
  fsys_join (dir, sizeof dir, l->root, "Previews");
  fsys_join (out, n, dir, shard);
}

static void origin_leaf (const cat_origin *o, char *leaf, size_t n)
{
  snprintf (leaf, n, "%lld_%08x", o->id, preview_key (o));
}

int lib_preview_leaf (library *l, cat_id original, char *leaf, size_t n)
{
  cat_origin o;
  if (cat_origin_of (l->db, original, &o) != 1)
    return 0;
  origin_leaf (&o, leaf, n);
  return 1;
}

static int find_origin (library *l, const cat_origin *o, char *out, size_t n)
{
  char dir[1100], leaf[40];
  fsys_entry e;
  preview_dir (l, o->id, dir, sizeof dir);
  origin_leaf (o, leaf, sizeof leaf);
  fsys_join (out, n, dir, leaf);
  return fsys_stat (out, &e) == 0 && !e.is_dir && e.size > 0;
}

int lib_preview_find (library *l, cat_id original, char *out, size_t n)
{
  cat_origin o;
  if (cat_origin_of (l->db, original, &o) != 1)
    return 0;
  return find_origin (l, &o, out, n);
}

/* Removes the original's previews other than keep (out of date). */
static void remove_others (cat_id original, const char *dir, const char *keep)
{
  char prefix[32], path[1300], gone[8][40];
  fsys_dir *d = fsys_opendir (dir);
  fsys_entry e;
  int n = 0, k;
  if (!d)
    return;
  snprintf (prefix, sizeof prefix, "%lld_", original);
  while (n < 8 && fsys_readdir (d, &e) == 1)
    if (!e.is_dir && strncmp (e.name, prefix, strlen (prefix)) == 0 && strcmp (e.name, keep) != 0)
      snprintf (gone[n++], sizeof gone[0], "%s", e.name);
  fsys_closedir (d);
  for (k = 0; k < n; k++)
    {
      fsys_join (path, sizeof path, dir, gone[k]);
      remove (path);
    }
}

/* Originals no longer in the library: their previews go too. */
static void remove_previews_of_gone (library *l, const cat_id *originals, int n)
{
  int i;
  for (i = 0; i < n; i++)
    {
      cat_origin o;
      char dir[1100];
      if (originals[i] <= 0 || cat_origin_of (l->db, originals[i], &o) == 1)
        continue;
      preview_dir (l, originals[i], dir, sizeof dir);
      remove_others (originals[i], dir, "");
    }
}

static int save_origin (library *l, const cat_origin *o, const rgb_image *img)
{
  char dir[1100], leaf[40], path[1300], tmp[1300], e[200];
  preview_dir (l, o->id, dir, sizeof dir);
  if (!l->pv_shard_made[o->id & 0xFF])
    {
      fsys_mkdirs (dir);
      l->pv_shard_made[o->id & 0xFF] = 1;
    }
  origin_leaf (o, leaf, sizeof leaf);
  fsys_join (path, sizeof path, dir, leaf);
  fsys_join (tmp, sizeof tmp, dir, "new");
  /* Written under another name, then renamed: a preview half written (a
     power cut) is never taken for a whole one. */
  remove (tmp);
  if (img_save_jpeg (img, tmp, PREVIEW_QUALITY, e, sizeof e))
    {
      snprintf (l->err, sizeof l->err, "Preview: %s", e);
      remove (tmp);
      return -1;
    }
  remove (path);
  if (rename (tmp, path))
    {
      snprintf (l->err, sizeof l->err, "Preview: can't rename it in %s", dir);
      remove (tmp);
      return -1;
    }
  fsys_settype (path, FT_JPEG);
  remove_others (o->id, dir, leaf);
  return 1;
}

int lib_preview_save (library *l, cat_id original, const rgb_image *img)
{
  cat_origin o;
  if (cat_origin_of (l->db, original, &o) != 1 || !lib_preview_wanted (l, o.format, o.path))
    return 0;
  return save_origin (l, &o, img);
}

/* One step: 1 a preview made, 0 working, -1 nothing left (or held). */
static int preview_step (library *l)
{
  if (l->pv_tg)
    {
      int r = thumbgen_step (l->pv_tg, ROWS_PER_STEP * 2);
      rgb_image img;
      cat_origin o;
      if (r == 0)
        return 0;
      if (r > 0 && thumbgen_take (l->pv_tg, &img) == 0)
        {
          /* Kept only if the original is still as it was when started. */
          if (cat_origin_of (l->db, l->pv_original, &o) == 1)
            r = save_origin (l, &o, &img);
          free (img.rgb);
        }
      else if (r < 0)
        snprintf (l->err, sizeof l->err, "Preview: %s", thumbgen_error (l->pv_tg));
      thumbgen_free (l->pv_tg);
      l->pv_tg = NULL;
      return r > 0;
    }
  if (l->pv_hold || l->pv_done || l->pv_mode == PREVIEWS_OFF)
    return -1;
  {
    int looked;
    for (looked = 0; looked < PREVIEW_LOOKS; looked++)
      {
        cat_origin o;
        char path[1300], e[200];
        if (cat_origin_below (l->db, l->pv_below, &o) != 1)
          {
            l->pv_done = 1;
            return -1;
          }
        l->pv_below = o.id;
        if (o.missing || !lib_preview_wanted (l, o.format, o.path) || find_origin (l, &o, path, sizeof path))
          continue;
        /* Upright as the Viewer shows it: a raw's orientation comes from the
           plug-in, as the catalogue has it after its thumbnail. */
        l->pv_tg = thumbgen_start (o.path, NULL, PREVIEW_SIDE, o.orientation, e, sizeof e);
        if (!l->pv_tg)
          {
            snprintf (l->err, sizeof l->err, "Preview of %s: %s", fsys_leaf (o.path), e);
            continue;
          }
        l->pv_original = o.id;
        snprintf (l->pv_leaf, sizeof l->pv_leaf, "%s", fsys_leaf (o.path));
        return 0;
      }
  }
  return 0;
}

thumbgen *lib_preview_adopt (library *l, cat_id original)
{
  thumbgen *t;
  if (!l || !l->pv_tg || l->pv_original != original)
    return NULL;
  t = l->pv_tg;
  l->pv_tg = NULL;
  return t;
}

int lib_previews_making (library *l)
{
  return l && (l->pv_tg != NULL || (!l->pv_done && l->pv_mode != PREVIEWS_OFF));
}

int lib_preview_now (library *l, char *leaf, size_t n)
{
  if (!l || !l->pv_tg)
    return 0;
  snprintf (leaf, n, "%s", l->pv_leaf);
  return 1;
}

int lib_preview_work (library *l, unsigned (*now) (void), unsigned deadline)
{
  int made = 0, first = 1;
  while (first || (int) (now () - deadline) < 0)
    {
      int r = preview_step (l);
      first = 0;
      if (r > 0)
        made++;
      if (r < 0)
        break;
      /* A raw is being developed by the plug-in in its task window: give
         it the computer rather than asking again and again. */
      if (r == 0 && l->pv_tg && rawjob_busy () && thumbgen_progress (l->pv_tg) == 0)
        break;
    }
  return made;
}

int lib_discard_previews (library *l, long long *bytes)
{
  char top[1100], dir[1100], path[1300], shard[8];
  int k, n = 0;
  /* One being made carries on (a raw's plug-in job can't be stopped). */
  *bytes = 0;
  fsys_join (top, sizeof top, l->root, "Previews");
  for (k = 0; k < 256; k++)
    {
      fsys_dir *d;
      fsys_entry e;
      char (*names)[40] = NULL;
      int m = 0, cap = 0, i;
      snprintf (shard, sizeof shard, "%02x", k);
      fsys_join (dir, sizeof dir, top, shard);
      if (!(d = fsys_opendir (dir)))
        continue;
      while (fsys_readdir (d, &e) == 1)
        {
          if (e.is_dir)
            continue;
          if (m == cap)
            {
              char (*nn)[40] = realloc (names, sizeof *names * (size_t) (cap = cap ? cap * 2 : 64));
              if (!nn)
                break;
              names = nn;
            }
          snprintf (names[m++], sizeof names[0], "%s", e.name);
          *bytes += e.size;
        }
      fsys_closedir (d);
      for (i = 0; i < m; i++)
        {
          fsys_join (path, sizeof path, dir, names[i]);
          if (remove (path) == 0)
            n++;
        }
      free (names);
    }
  memset (l->pv_shard_made, 0, sizeof l->pv_shard_made);
  lib_previews_changed (l);
  return n;
}
