/* library.c: see library.h. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include "exif.h"
#include "fsys.h"
#include "library.h"
#include "thumbgen.h"
#include "develop.h"

#define HEAD_BYTES (128 * 1024)       /* enough for EXIF in nearly every camera's files */
#define HASH_BYTES (64 * 1024)
#define ROWS_PER_STEP 16

typedef struct root_req
{
  struct root_req *next;
  char path[1024];
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
  int found, added, skipped, importing;
  unsigned char *head;
  /* thumbnails */
  thumbgen *tg;
  cat_id tg_version;
  char tg_recipe[1024];               /* applied when the thumbnail is finished */
  char tg_dst[1100];
  int thumbs_pending;                 /* cached count; -1 to recount */
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
  l->thumbs_pending = -1;
  *out = l;
  return 0;
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

int lib_import (library *l, const char *path)
{
  root_req *r = calloc (1, sizeof *r);
  if (!r)
    return -1;
  snprintf (r->path, sizeof r->path, "%s", path);
  if (l->roots_tail)
    l->roots_tail->next = r;
  else
    l->roots = r;
  l->roots_tail = r;
  return 0;
}

int lib_busy (library *l)
{
  if (l->roots || l->nstack || l->dir || l->tg)
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

static int likely_image_type (int ft)
{
  return ft == FT_JPEG || ft == FT_PNG || ft == FT_NONE || ft == FT_DATA;
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
  if (!likely_image_type (e->filetype))
    return;
  f = fopen (path, "rb");
  if (!f)
    {
      snprintf (l->err, sizeof l->err, "Can't open %s", path);
      l->skipped++;
      return;
    }
  got = fread (l->head, 1, HEAD_BYTES, f);
  if (img_probe (l->head, got) == IMG_UNKNOWN)
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
  if (img_read_info (l->head, got, &info))
    {
      snprintf (l->err, sizeof l->err, "%s: can't read its size", fsys_leaf (path));
      l->skipped++;
      fclose (f);
      return;
    }
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
  o.filetype = e->filetype == FT_NONE ? (info.format == IMG_JPEG ? FT_JPEG : FT_PNG) : e->filetype;
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
      l->added++;
      if (l->thumbs_pending >= 0)
        l->thumbs_pending++;
    }
  else if (v == 0)
    l->skipped++;
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
          l->found = l->added = l->skipped = 0;
          l->importing = 1;
          l->batch = (long long) time (NULL) * 100 + (++counter % 100);
        }
      if (fsys_stat (r->path, &e) != 0)
        {
          snprintf (l->err, sizeof l->err, "Can't find %s", r->path);
          l->project = 0;
        }
      else if (e.is_dir)
        {
          l->project = cat_project_ensure (l->db, fsys_leaf (r->path));
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
  if (!l->tg)
    {
      char src[1024], dst[1100], e[200];
      int orient = 1;
      cat_id v;
      if (cat_next_thumb (l->db, &v, src, sizeof src, &orient, l->tg_recipe, sizeof l->tg_recipe) != 1)
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
