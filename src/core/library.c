/* library.c: see library.h. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include "exif.h"
#include "fsys.h"
#include "library.h"
#include "libxfer.h"
#include "xmp.h"
#include "thumbgen.h"
#include "develop.h"

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

int lib_remove (library *l, const cat_id *v, int n)
{
  int i;
  if (cat_remove_versions (l->db, v, n))
    return -1;
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

/* Raw files come typed every way (DOS from a card, Text, Data, TIFF or
   nothing), so their extension lets any type through to the probe. */
static int likely_image_type (int ft, const char *path)
{
  return ft == FT_JPEG || ft == FT_PNG || ft == FT_NONE || ft == FT_DATA || ft == FT_TIFF
         || img_has_raw_extension (path);
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
  if (img_probe_named (l->head, got, path) == IMG_UNKNOWN)
    {
      if (e->filetype == FT_JPEG || e->filetype == FT_PNG)
        {
          snprintf (l->err, sizeof l->err, "%s isn't a readable image", fsys_leaf (path));
          l->skipped++;
        }
      fclose (f);
      return;
    }
  l->found++;
  if (img_probe_named (l->head, got, path) == IMG_RAW)
    img_read_raw_info (l->head, got, &info);   /* the rest comes from the plug-in with the thumbnail */
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
  o.filetype = e->filetype != FT_NONE ? e->filetype
             : info.format == IMG_JPEG ? FT_JPEG : info.format == IMG_PNG ? FT_PNG : FT_DATA;
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
