/* libxfer.c: see libxfer.h. The other library's catalogue is ATTACHed to
   this one's connection as "x" while its rows are read or written. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <time.h>
#include "sqlite3.h"
#include "fsys.h"
#include "libxfer.h"

enum { PH_PLANNED, PH_ORIGS, PH_MERGE, PH_THUMBS, PH_DONE };

typedef struct
{
  char *src, *dst;
  cat_id thumb_of;                     /* a thumbnail for this version (import), or 0 */
} job;

/* One of the other library's originals (import). */
typedef struct
{
  cat_id id;
  char *path;                          /* where it is now (moved with its library) */
  char *dst;                           /* where it goes (copied) or path */
  char *project, *root;                /* its first version's project, and that project's folder */
  long long size;
  sqlite3_int64 hash;
  int exists;
  int copy_failed;
  cat_id map;                          /* the original here; 0 new */
} xorig;

typedef struct { cat_id from, to; int added; } idpair;
typedef struct { idpair *p; int n, cap; } idmap;

/* Paths given out, case ignored (RISC OS's filing systems ignore it). */
typedef struct { char **slot; int cap, n; } strset;

struct libxfer
{
  library *l;
  int export, phase, copy, originals;
  char other[1024];                    /* the other library */
  char home[1024];                     /* where it was when its paths were written */
  char sep, ext;
  job *jobs;
  int njobs, capjobs, next;
  int copied, copy_failed;
  long long bytes;
  xorig *orig;
  int norig;
  strset used;
  /* results */
  int photos, versions, known, skipped_versions, missing;
  libx_survey s;
  int failed;
  char err[400];
};

/* --- small helpers -------------------------------------------------------- */

static void seps (libxfer *x)
{
  char t[8];
  fsys_join (t, sizeof t, "a", "b");
  x->sep = t[1];
  x->ext = x->sep == '.' ? '/' : '.';
}

static char *dup (const char *s)
{
  size_t n = strlen (s ? s : "") + 1;
  char *d = malloc (n);
  if (d)
    memcpy (d, s ? s : "", n);
  return d;
}

static unsigned hash_ci (const char *s)
{
  unsigned h = 2166136261u;
  for (; *s; s++)
    {
      unsigned c = (unsigned char) *s;
      if (c >= 'A' && c <= 'Z')
        c += 32;
      h = (h ^ c) * 16777619u;
    }
  return h;
}

static int set_has (strset *t, const char *s)
{
  unsigned i;
  if (!t->cap)
    return 0;
  for (i = hash_ci (s) & (unsigned) (t->cap - 1); t->slot[i]; i = (i + 1) & (unsigned) (t->cap - 1))
    if (!strcasecmp (t->slot[i], s))
      return 1;
  return 0;
}

static int set_add (strset *t, const char *s)
{
  unsigned i;
  if ((t->n + 1) * 2 > t->cap)
    {
      int cap = t->cap ? t->cap * 2 : 256, j;
      char **slot = calloc ((size_t) cap, sizeof *slot);
      if (!slot)
        return -1;
      for (j = 0; j < t->cap; j++)
        if (t->slot[j])
          {
            unsigned k = hash_ci (t->slot[j]) & (unsigned) (cap - 1);
            while (slot[k])
              k = (k + 1) & (unsigned) (cap - 1);
            slot[k] = t->slot[j];
          }
      free (t->slot);
      t->slot = slot;
      t->cap = cap;
    }
  for (i = hash_ci (s) & (unsigned) (t->cap - 1); t->slot[i]; i = (i + 1) & (unsigned) (t->cap - 1))
    ;
  if (!(t->slot[i] = dup (s)))
    return -1;
  t->n++;
  return 0;
}

static void set_free (strset *t)
{
  int i;
  for (i = 0; i < t->cap; i++)
    free (t->slot[i]);
  free (t->slot);
  memset (t, 0, sizeof *t);
}

static int map_put (idmap *m, cat_id from, cat_id to, int added)
{
  if (m->n == m->cap)
    {
      int cap = m->cap ? m->cap * 2 : 64;
      idpair *p = realloc (m->p, sizeof *p * (size_t) cap);
      if (!p)
        return -1;
      m->p = p;
      m->cap = cap;
    }
  m->p[m->n].from = from;
  m->p[m->n].to = to;
  m->p[m->n].added = added;
  m->n++;
  return 0;
}

/* Keys are put in ascending order (rows read ORDER BY id). */
static idpair *map_find (idmap *m, cat_id from)
{
  int lo = 0, hi = m->n - 1;
  while (lo <= hi)
    {
      int mid = (lo + hi) / 2;
      if (m->p[mid].from == from)
        return &m->p[mid];
      if (m->p[mid].from < from)
        lo = mid + 1;
      else
        hi = mid - 1;
    }
  return NULL;
}

static cat_id map_get (idmap *m, cat_id from)
{
  idpair *p = map_find (m, from);
  return p ? p->to : 0;
}

/* A project's name as a leaf: letters, digits, - and _ kept. */
static void safe_leaf (const char *name, char *out, size_t n)
{
  size_t i = 0;
  for (; *name && i + 1 < n && i < 48; name++)
    {
      unsigned char c = (unsigned char) *name;
      out[i++] = ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9')
                  || c == '-' || c == '_') ? (char) c : '_';
    }
  out[i] = 0;
  if (!i)
    snprintf (out, n, "Photos");
}

static int under (libxfer *x, const char *path, const char *dir)
{
  size_t n = strlen (dir);
  return n && !strncmp (path, dir, n) && path[n] == x->sep;
}

static void thumb_path (const char *root, cat_id v, char *out, size_t n)
{
  char dir[1100], shard[8], leaf[24];
  snprintf (shard, sizeof shard, "%02x", (unsigned) (v & 0xFF));
  fsys_join (dir, sizeof dir, root, "Thumbs");
  fsys_join (out, n, dir, shard);
  snprintf (dir, sizeof dir, "%s", out);
  snprintf (leaf, sizeof leaf, "%lld", v);
  fsys_join (out, n, dir, leaf);
}

static int exists (const char *path)
{
  fsys_entry e;
  return fsys_stat (path, &e) == 0;
}

/* Where an original goes in the library at root: Originals.<project>.<its
   path inside the project's folder> (or just its leaf), made unique. */
static void place (libxfer *x, const char *root, const char *project, const char *proot,
                   const char *path, int check_disc, char *out, size_t n)
{
  char base[1100], dir[1100], leaf[64];
  const char *rel = proot && under (x, path, proot) ? path + strlen (proot) + 1 : fsys_leaf (path);
  int k;
  fsys_join (dir, sizeof dir, root, "Originals");
  safe_leaf (project, leaf, sizeof leaf);
  fsys_join (base, sizeof base, dir, leaf);
  fsys_join (out, n, base, rel);
  for (k = 2; (set_has (&x->used, out) || (check_disc && exists (out))) && k < 10000; k++)
    {
      /* name-2/jpg: the number goes before the extension */
      char stem[1100];
      const char *lf;
      char *dot;
      fsys_join (stem, sizeof stem, base, rel);
      lf = fsys_leaf (stem);
      dot = strrchr (lf, x->ext);
      if (dot && dot != lf)
        {
          char tail[300];
          snprintf (tail, sizeof tail, "%s", dot);
          *dot = 0;
          snprintf (out, n, "%s-%d%s", stem, k, tail);
        }
      else
        snprintf (out, n, "%s-%d", stem, k);
    }
  set_add (&x->used, out);
}

static int add_job (libxfer *x, const char *src, const char *dst, cat_id thumb_of)
{
  if (x->njobs == x->capjobs)
    {
      int cap = x->capjobs ? x->capjobs * 2 : 128;
      job *j = realloc (x->jobs, sizeof *j * (size_t) cap);
      if (!j)
        return -1;
      x->jobs = j;
      x->capjobs = cap;
    }
  x->jobs[x->njobs].src = dup (src);
  x->jobs[x->njobs].dst = dup (dst);
  x->jobs[x->njobs].thumb_of = thumb_of;
  if (!x->jobs[x->njobs].src || !x->jobs[x->njobs].dst)
    return -1;
  x->njobs++;
  return 0;
}

static sqlite3 *dbof (libxfer *x)
{
  return cat_sqlite (lib_cat (x->l));
}

static int sqlfail (libxfer *x, const char *what)
{
  snprintf (x->err, sizeof x->err, "%s: %s", what, sqlite3_errmsg (dbof (x)));
  return -1;
}

static int run (libxfer *x, const char *sql)
{
  char *msg = NULL;
  if (sqlite3_exec (dbof (x), sql, NULL, NULL, &msg) != SQLITE_OK)
    {
      snprintf (x->err, sizeof x->err, "%s", msg ? msg : "SQLite error");
      sqlite3_free (msg);
      return -1;
    }
  return 0;
}

static sqlite3_stmt *prep (libxfer *x, const char *sql)
{
  sqlite3_stmt *s = NULL;
  if (sqlite3_prepare_v2 (dbof (x), sql, -1, &s, NULL) != SQLITE_OK)
    {
      sqlfail (x, "Can't read the catalogue");
      return NULL;
    }
  return s;
}

static const char *ctext (sqlite3_stmt *s, int i)
{
  const unsigned char *t = sqlite3_column_text (s, i);
  return t ? (const char *) t : "";
}

static int attach (libxfer *x, const char *root)
{
  char path[1100];
  sqlite3_stmt *s;
  int rc;
  fsys_join (path, sizeof path, root, "Catalogue");
  if (!(s = prep (x, "ATTACH ?1 AS x")))
    return -1;
  sqlite3_bind_text (s, 1, path, -1, SQLITE_TRANSIENT);
  rc = sqlite3_step (s);
  sqlite3_finalize (s);
  return rc == SQLITE_DONE ? 0 : sqlfail (x, "Can't open the other library's catalogue");
}

static void detach (libxfer *x)
{
  sqlite3_exec (dbof (x), "DETACH x", NULL, NULL, NULL);
}

/* A one-number query, or dflt. */
static long long one (libxfer *x, const char *sql, long long dflt)
{
  sqlite3_stmt *s = prep (x, sql);
  long long v = dflt;
  if (s && sqlite3_step (s) == SQLITE_ROW && sqlite3_column_type (s, 0) != SQLITE_NULL)
    v = sqlite3_column_int64 (s, 0);
  sqlite3_finalize (s);
  return v;
}

int libx_is_library (const char *dir)
{
  char path[1100];
  fsys_entry e;
  fsys_join (path, sizeof path, dir, "Catalogue");
  return fsys_stat (path, &e) == 0 && !e.is_dir;
}

/* --- moved libraries -------------------------------------------------------- */

/* Rows whose column starts with old: old replaced by now. The rows are
   read first and changed after (changing them while the SELECT reads
   through the column's index could meet a changed row again). */
static int rehome_column (sqlite3 *db, const char *table, const char *col, const char *old, const char *now)
{
  char sql[200];
  sqlite3_stmt *s, *u;
  size_t n = strlen (old);
  typedef struct { cat_id id; char *path; } row;
  row *r = NULL;
  int nr = 0, cap = 0, i, rc = 0;
  snprintf (sql, sizeof sql, "SELECT id, %s FROM %s WHERE %s IS NOT NULL", col, table, col);
  if (sqlite3_prepare_v2 (db, sql, -1, &s, NULL) != SQLITE_OK)
    return -1;
  while (sqlite3_step (s) == SQLITE_ROW)
    {
      const char *p = (const char *) sqlite3_column_text (s, 1);
      if (!p || strncmp (p, old, n))
        continue;
      if (nr == cap)
        {
          row *t = realloc (r, sizeof *r * (size_t) (cap = cap ? cap * 2 : 64));
          if (!t)
            {
              rc = -1;
              break;
            }
          r = t;
        }
      r[nr].id = sqlite3_column_int64 (s, 0);
      if (!(r[nr].path = malloc (strlen (now) + strlen (p + n) + 1)))
        {
          rc = -1;
          break;
        }
      strcpy (r[nr].path, now);
      strcat (r[nr].path, p + n);
      nr++;
    }
  sqlite3_finalize (s);
  snprintf (sql, sizeof sql, "UPDATE %s SET %s = ?2 WHERE id = ?1", table, col);
  if (!rc && sqlite3_prepare_v2 (db, sql, -1, &u, NULL) == SQLITE_OK)
    {
      for (i = 0; i < nr && !rc; i++)
        {
          sqlite3_bind_int64 (u, 1, r[i].id);
          sqlite3_bind_text (u, 2, r[i].path, -1, SQLITE_TRANSIENT);
          if (sqlite3_step (u) != SQLITE_DONE)
            rc = -1;                   /* (a clash: all of it is rolled back) */
          sqlite3_reset (u);
        }
      sqlite3_finalize (u);
    }
  else
    rc = -1;
  for (i = 0; i < nr; i++)
    free (r[i].path);
  free (r);
  return rc ? -1 : nr;
}

int libx_rehome (cat *c, const char *root)
{
  sqlite3 *db = cat_sqlite (c);
  sqlite3_stmt *s;
  char home[1024] = "", old[1030], now[1030], t[8];
  int changed = 0;
  if (sqlite3_prepare_v2 (db, "SELECT value FROM meta WHERE key = 'home'", -1, &s, NULL) != SQLITE_OK)
    return -1;
  if (sqlite3_step (s) == SQLITE_ROW && sqlite3_column_text (s, 0))
    snprintf (home, sizeof home, "%s", (const char *) sqlite3_column_text (s, 0));
  sqlite3_finalize (s);
  if (home[0] && !strcmp (home, root))
    return 0;
  fsys_join (t, sizeof t, "a", "b");
  snprintf (old, sizeof old, "%s%c", home, t[1]);
  snprintf (now, sizeof now, "%s%c", root, t[1]);
  if (sqlite3_exec (db, "BEGIN", NULL, NULL, NULL) != SQLITE_OK)
    return -1;
  if (home[0])
    {
      int a = rehome_column (db, "original", "path", old, now);
      int b = rehome_column (db, "project", "root", old, now);
      if (a < 0 || b < 0)
        {
          sqlite3_exec (db, "ROLLBACK", NULL, NULL, NULL);
          return -1;
        }
      changed = a + b;
    }
  if (sqlite3_prepare_v2 (db, "INSERT OR REPLACE INTO meta VALUES ('home', ?1)", -1, &s, NULL) == SQLITE_OK)
    {
      sqlite3_bind_text (s, 1, root, -1, SQLITE_TRANSIENT);
      sqlite3_step (s);
      sqlite3_finalize (s);
    }
  if (sqlite3_exec (db, "COMMIT", NULL, NULL, NULL) != SQLITE_OK)
    {
      sqlite3_exec (db, "ROLLBACK", NULL, NULL, NULL);
      return -1;
    }
  return changed > 0;
}

/* --- export ------------------------------------------------------------------- */

static const char ORIG_COLS[] =
  "path, size, mtime, hash, filetype, format, width, height, orientation, taken, make, model,"
  " exposure, fnumber, iso, focal, thumb_offset, thumb_length, batch, missing";
static const char VER_COLS[] =
  "original, project, name, rating, flag, recipe, thumb, created, modified, caption, stack, label";

static int export_rows (libxfer *x, const cat_id *v, int n, const char *dst)
{
  char sql[1200];
  int all = n < 0, i;
  sqlite3_stmt *s;
  if (run (x, "CREATE TABLE x.sel (id INTEGER PRIMARY KEY)"))
    return -1;
  if (all)
    {
      if (run (x, "INSERT INTO x.sel SELECT id FROM main.version"))
        return -1;
    }
  else
    {
      if (!(s = prep (x, "INSERT OR IGNORE INTO x.sel VALUES (?1)")))
        return -1;
      for (i = 0; i < n; i++)
        {
          sqlite3_bind_int64 (s, 1, v[i]);
          sqlite3_step (s);
          sqlite3_reset (s);
        }
      sqlite3_finalize (s);
    }
  snprintf (sql, sizeof sql,
            "INSERT INTO x.original (id, %s) SELECT id, %s FROM main.original"
            " WHERE id IN (SELECT original FROM main.version WHERE id IN (SELECT id FROM x.sel));", ORIG_COLS, ORIG_COLS);
  if (run (x, sql))
    return -1;
  snprintf (sql, sizeof sql,
            "INSERT INTO x.project (id, name, created, root, watch, folder_albums)"
            " SELECT id, name, created, root, %s, folder_albums FROM main.project WHERE %s;",
            all ? "watch" : "0", all ? "1" : "id IN (SELECT project FROM main.version WHERE id IN (SELECT id FROM x.sel))");
  if (run (x, sql))
    return -1;
  snprintf (sql, sizeof sql, "INSERT INTO x.version (id, %s) SELECT id, %s FROM main.version"
            " WHERE id IN (SELECT id FROM x.sel);", VER_COLS, VER_COLS);
  if (run (x, sql))
    return -1;
  /* Stacks: only those with two or more of their versions here; the pick
     one of them. */
  if (run (x, "INSERT INTO x.stack (id, pick, open) SELECT id, pick, open FROM main.stack"
              "  WHERE id IN (SELECT stack FROM x.version WHERE stack IS NOT NULL GROUP BY stack HAVING COUNT(*) > 1);"
              "UPDATE x.version SET stack = NULL WHERE stack IS NOT NULL AND stack NOT IN (SELECT id FROM x.stack);"
              "UPDATE x.stack SET pick = (SELECT MIN(v.id) FROM x.version v WHERE v.stack = stack.id)"
              "  WHERE pick NOT IN (SELECT id FROM x.version);"))
    return -1;
  snprintf (sql, sizeof sql,
            "INSERT INTO x.album (id, name, created, project, folder, smart) SELECT id, name, created, project, folder, smart"
            " FROM main.album WHERE %s;"
            "INSERT INTO x.album_item (album, version, pos) SELECT album, version, pos FROM main.album_item"
            " WHERE album IN (SELECT id FROM x.album) AND version IN (SELECT id FROM x.sel);",
            all ? "1" : "smart IS NULL AND id IN (SELECT album FROM main.album_item WHERE version IN (SELECT id FROM x.sel))");
  if (run (x, sql))
    return -1;
  snprintf (sql, sizeof sql,
            "INSERT INTO x.keyword (id, name) SELECT id, name FROM main.keyword WHERE %s;"
            "INSERT INTO x.version_keyword (version, keyword) SELECT version, keyword FROM main.version_keyword"
            " WHERE version IN (SELECT id FROM x.sel);"
            "INSERT INTO x.preset (id, name, recipe) SELECT id, name, recipe FROM main.preset;"
            "DROP TABLE x.sel;",
            all ? "1" : "id IN (SELECT keyword FROM main.version_keyword WHERE version IN (SELECT id FROM x.sel))");
  if (run (x, sql))
    return -1;
  if ((s = prep (x, "INSERT OR REPLACE INTO x.meta VALUES ('home', ?1)")))
    {
      sqlite3_bind_text (s, 1, dst, -1, SQLITE_TRANSIENT);
      sqlite3_step (s);
      sqlite3_finalize (s);
    }
  return 0;
}

/* The originals copied in: each one's new path, and the projects' folders
   and folder albums' folders moved with them. */
/* Each exported project's directory in Originals: its name as a leaf,
   made unique ("My Trip" and "My_Trip" don't share one). */
typedef struct { cat_id id; char leaf[56]; } pleaf;

static int project_leaves (libxfer *x, pleaf **out, int *count)
{
  sqlite3_stmt *s = prep (x, "SELECT id, name FROM x.project ORDER BY id");
  strset seen = { 0 };
  pleaf *p = NULL;
  int n = 0, cap = 0, rc = 0;
  *out = NULL;
  *count = 0;
  if (!s)
    return -1;
  while (sqlite3_step (s) == SQLITE_ROW)
    {
      char base[45];
      int k;
      if (n == cap)
        {
          pleaf *t = realloc (p, sizeof *p * (size_t) (cap = cap ? cap * 2 : 16));
          if (!t)
            {
              rc = -1;
              break;
            }
          p = t;
        }
      p[n].id = sqlite3_column_int64 (s, 0);
      safe_leaf (ctext (s, 1), base, sizeof base);
      snprintf (p[n].leaf, sizeof p[n].leaf, "%s", base);
      for (k = 2; set_has (&seen, p[n].leaf); k++)
        snprintf (p[n].leaf, sizeof p[n].leaf, "%s_%d", base, k);
      set_add (&seen, p[n].leaf);
      n++;
    }
  sqlite3_finalize (s);
  set_free (&seen);
  *out = p;
  *count = n;
  return rc;
}

static const char *leaf_of (const pleaf *p, int n, cat_id id)
{
  int i;
  for (i = 0; i < n; i++)
    if (p[i].id == id)
      return p[i].leaf;
  return "Photos";
}

static int export_bundle (libxfer *x, const char *dst)
{
  sqlite3_stmt *s, *u;
  char newp[1100], dir[1100];
  typedef struct { cat_id id; char *path, *project, *root; } row;
  row *r = NULL;
  pleaf *pl;
  int n = 0, cap = 0, i, rc = 0, npl;
  if (project_leaves (x, &pl, &npl))
    {
      free (pl);
      return -1;
    }
  if (!(s = prep (x, "SELECT o.id, o.path, p.id, p.root FROM x.original o JOIN x.project p"
                     " ON p.id = (SELECT v.project FROM x.version v WHERE v.original = o.id ORDER BY v.id LIMIT 1)"
                     " ORDER BY o.id")))
    {
      free (pl);
      return -1;
    }
  while (sqlite3_step (s) == SQLITE_ROW)
    {
      if (n == cap)
        {
          row *t = realloc (r, sizeof *r * (size_t) (cap = cap ? cap * 2 : 256));
          if (!t)
            {
              rc = -1;
              break;
            }
          r = t;
        }
      r[n].id = sqlite3_column_int64 (s, 0);
      r[n].path = dup (ctext (s, 1));
      r[n].project = dup (leaf_of (pl, npl, sqlite3_column_int64 (s, 2)));
      r[n].root = sqlite3_column_type (s, 3) == SQLITE_NULL ? NULL : dup (ctext (s, 3));
      n++;
    }
  sqlite3_finalize (s);
  if (!rc && (u = prep (x, "UPDATE x.original SET path = ?2, missing = 0 WHERE id = ?1")))
    {
      for (i = 0; i < n && !rc; i++)
        {
          if (!r[i].path || !exists (r[i].path))
            {
              x->missing++;                /* left where it was */
              continue;
            }
          place (x, dst, r[i].project, r[i].root, r[i].path, 0, newp, sizeof newp);
          sqlite3_bind_int64 (u, 1, r[i].id);
          sqlite3_bind_text (u, 2, newp, -1, SQLITE_TRANSIENT);
          if (sqlite3_step (u) != SQLITE_DONE)
            rc = sqlfail (x, "Can't write the new catalogue");
          sqlite3_reset (u);
          if (!rc && add_job (x, r[i].path, newp, 0))
            rc = -1;
        }
      sqlite3_finalize (u);
    }
  else
    rc = -1;
  for (i = 0; i < n; i++)
    {
      free (r[i].path);
      free (r[i].project);
      free (r[i].root);
    }
  free (r);
  if (rc)
    {
      free (pl);
      return rc;
    }
  /* Projects' folders (a folder album's folder is relative to it). */
  fsys_join (dir, sizeof dir, dst, "Originals");
  if (!(s = prep (x, "SELECT id FROM x.project WHERE root IS NOT NULL")))
    {
      free (pl);
      return -1;
    }
  u = prep (x, "UPDATE x.project SET root = ?2 WHERE id = ?1");
  while (u && sqlite3_step (s) == SQLITE_ROW)
    {
      fsys_join (newp, sizeof newp, dir, leaf_of (pl, npl, sqlite3_column_int64 (s, 0)));
      sqlite3_bind_int64 (u, 1, sqlite3_column_int64 (s, 0));
      sqlite3_bind_text (u, 2, newp, -1, SQLITE_TRANSIENT);
      sqlite3_step (u);
      sqlite3_reset (u);
    }
  sqlite3_finalize (s);
  sqlite3_finalize (u);
  free (pl);
  return 0;
}

static libxfer *new_xfer (library *l)
{
  libxfer *x = calloc (1, sizeof *x);
  if (x)
    {
      x->l = l;
      seps (x);
    }
  return x;
}

int libx_export (libxfer **out, library *l, const char *dst, const cat_id *versions, int n,
                 int originals, char *err, size_t errlen)
{
  libxfer *x;
  char path[1100];
  cat *c;
  sqlite3_stmt *s;
  int rc;
  *out = NULL;
  if (n == 0)
    {
      snprintf (err, errlen, "There are no photos to export.");
      return -1;
    }
  if (exists (dst))
    {
      snprintf (err, errlen, "There is already something called %s there: choose another name.", fsys_leaf (dst));
      return -1;
    }
  if (!(x = new_xfer (l)))
    {
      snprintf (err, errlen, "Not enough memory");
      return -1;
    }
  x->export = 1;
  x->originals = originals;
  snprintf (x->other, sizeof x->other, "%s", dst);
  fsys_join (path, sizeof path, dst, "Thumbs");
  if (fsys_mkdirs (path))
    {
      snprintf (err, errlen, "Can't create %s", dst);
      libx_free (x);
      return -1;
    }
  fsys_join (path, sizeof path, dst, "Catalogue");
  if (cat_open (&c, path, err, errlen))
    {
      fsys_remove_tree (dst);
      libx_free (x);
      return -1;
    }
  cat_close (c);
  rc = attach (x, dst);
  if (!rc)
    {
      rc = run (x, "BEGIN") || export_rows (x, versions, n, dst) || (originals && export_bundle (x, dst));
      if (!rc)
        {
          /* thumbnails: the same ids there */
          if ((s = prep (x, "SELECT id FROM x.version WHERE thumb = 1 ORDER BY id")))
            {
              while (!rc && sqlite3_step (s) == SQLITE_ROW)
                {
                  char a[1100], b[1100];
                  cat_id v = sqlite3_column_int64 (s, 0);
                  lib_thumb_path (l, v, a, sizeof a);
                  thumb_path (dst, v, b, sizeof b);
                  rc = add_job (x, a, b, 0);
                }
              sqlite3_finalize (s);
            }
          x->photos = (int) one (x, "SELECT COUNT(*) FROM x.original", 0);
          x->versions = (int) one (x, "SELECT COUNT(*) FROM x.version", 0);
        }
      if (!rc)
        rc = run (x, "COMMIT");
      if (rc)
        sqlite3_exec (dbof (x), "ROLLBACK", NULL, NULL, NULL);
      detach (x);
    }
  if (rc)
    {
      snprintf (err, errlen, "%s", x->err[0] ? x->err : "Not enough memory");
      fsys_remove_tree (dst);
      libx_free (x);
      return -1;
    }
  x->phase = PH_ORIGS;
  *out = x;
  return 0;
}

/* --- import ----------------------------------------------------------------- */

/* The other library's paths, where its own originals are now. */
static void other_path (libxfer *x, const char *p, char *out, size_t n)
{
  if (x->home[0] && strcmp (x->home, x->other) && under (x, p, x->home))
    snprintf (out, n, "%s%s", x->other, p + strlen (x->home));
  else
    snprintf (out, n, "%s", p);
}

static cat_id find_original (libxfer *x, const char *path, long long size, sqlite3_int64 hash)
{
  sqlite3_stmt *s = prep (x, "SELECT id FROM main.original WHERE path = ?1 OR (size = ?2 AND hash = ?3) LIMIT 1");
  cat_id id = 0;
  if (!s)
    return -1;
  sqlite3_bind_text (s, 1, path, -1, SQLITE_TRANSIENT);
  sqlite3_bind_int64 (s, 2, size);
  sqlite3_bind_int64 (s, 3, hash);
  if (sqlite3_step (s) == SQLITE_ROW)
    id = sqlite3_column_int64 (s, 0);
  sqlite3_finalize (s);
  return id;
}

static int plan_import (libxfer *x)
{
  sqlite3_stmt *s;
  int cap = 0;
  x->home[0] = 0;
  if ((s = prep (x, "SELECT value FROM x.meta WHERE key = 'home'")))
    {
      if (sqlite3_step (s) == SQLITE_ROW)
        snprintf (x->home, sizeof x->home, "%s", ctext (s, 0));
      sqlite3_finalize (s);
    }
  if (!(s = prep (x, "SELECT o.id, o.path, o.size, o.hash, p.name, p.root FROM x.original o LEFT JOIN x.project p"
                     " ON p.id = (SELECT v.project FROM x.version v WHERE v.original = o.id ORDER BY v.id LIMIT 1)"
                     " ORDER BY o.id")))
    return -1;
  while (sqlite3_step (s) == SQLITE_ROW)
    {
      xorig *o;
      char p[1100], r[1100];
      if (x->norig == cap)
        {
          xorig *t = realloc (x->orig, sizeof *t * (size_t) (cap = cap ? cap * 2 : 256));
          if (!t)
            {
              sqlite3_finalize (s);
              snprintf (x->err, sizeof x->err, "Not enough memory");
              return -1;
            }
          x->orig = t;
        }
      o = &x->orig[x->norig++];
      memset (o, 0, sizeof *o);
      o->id = sqlite3_column_int64 (s, 0);
      other_path (x, ctext (s, 1), p, sizeof p);
      o->path = dup (p);
      o->size = sqlite3_column_int64 (s, 2);
      o->project = dup (sqlite3_column_type (s, 4) == SQLITE_NULL ? "Imported" : ctext (s, 4));
      if (sqlite3_column_type (s, 5) != SQLITE_NULL)
        {
          other_path (x, ctext (s, 5), r, sizeof r);
          o->root = dup (r);
        }
      o->hash = sqlite3_column_int64 (s, 3);
      o->map = find_original (x, p, o->size, o->hash);
      if (o->map < 0)
        {
          sqlite3_finalize (s);
          return -1;
        }
      x->s.photos++;
      if (o->map)
        x->s.known++;
      else if ((o->exists = exists (p)))
        x->s.bytes += o->size;
      else
        x->s.missing++;
    }
  sqlite3_finalize (s);
  return 0;
}

static xorig *orig_of (libxfer *x, cat_id id)
{
  int lo = 0, hi = x->norig - 1;
  while (lo <= hi)
    {
      int mid = (lo + hi) / 2;
      if (x->orig[mid].id == id)
        return &x->orig[mid];
      if (x->orig[mid].id < id)
        lo = mid + 1;
      else
        hi = mid - 1;
    }
  return NULL;
}

/* Counts what would come across. */
static int survey (libxfer *x)
{
  sqlite3_stmt *s, *q;
  x->s.versions = (int) one (x, "SELECT COUNT(*) FROM x.version", 0);
  if (!(s = prep (x, "SELECT original, recipe FROM x.version"))
      || !(q = prep (x, "SELECT 1 FROM main.version WHERE original = ?1 AND recipe = ?2")))
    {
      sqlite3_finalize (s);
      return -1;
    }
  while (sqlite3_step (s) == SQLITE_ROW)
    {
      xorig *o = orig_of (x, sqlite3_column_int64 (s, 0));
      if (!o)
        continue;
      if (!o->map)
        x->s.new_versions++;
      else
        {
          sqlite3_bind_int64 (q, 1, o->map);
          sqlite3_bind_text (q, 2, ctext (s, 1), -1, SQLITE_TRANSIENT);
          if (sqlite3_step (q) != SQLITE_ROW)
            x->s.new_versions++;
          sqlite3_reset (q);
        }
    }
  sqlite3_finalize (s);
  sqlite3_finalize (q);
  x->s.projects = (int) one (x, "SELECT COUNT(*) FROM x.project WHERE id IN (SELECT project FROM x.version)", 0);
  x->s.new_projects = (int) one (x, "SELECT COUNT(*) FROM x.project WHERE id IN (SELECT project FROM x.version)"
                                    " AND name NOT IN (SELECT name FROM main.project)", 0);
  x->s.albums = (int) one (x, "SELECT COUNT(*) FROM x.album WHERE folder IS NULL", 0);
  x->s.new_albums = (int) one (x, "SELECT COUNT(*) FROM x.album a WHERE folder IS NULL AND NOT EXISTS"
                                  " (SELECT 1 FROM main.album b WHERE b.name = a.name AND b.folder IS NULL"
                                  "  AND (b.smart IS NULL) = (a.smart IS NULL))", 0);
  x->s.keywords = (int) one (x, "SELECT COUNT(*) FROM x.keyword", 0);
  x->s.presets = (int) one (x, "SELECT COUNT(*) FROM x.preset", 0);
  return 0;
}

int libx_import_plan (libxfer **out, library *l, const char *src, libx_survey *sv,
                      char *err, size_t errlen)
{
  libxfer *x;
  char path[1100];
  cat *c;
  int rc;
  *out = NULL;
  memset (sv, 0, sizeof *sv);
  if (!libx_is_library (src))
    {
      snprintf (err, errlen, "%s isn't a Lumen library (it has no Catalogue).", fsys_leaf (src));
      return -1;
    }
  if (!(x = new_xfer (l)))
    {
      snprintf (err, errlen, "Not enough memory");
      return -1;
    }
  if (fsys_canonical (src, x->other, sizeof x->other) || !x->other[0])
    snprintf (x->other, sizeof x->other, "%s", src);
  if (!strcasecmp (x->other, lib_root (l)))
    {
      snprintf (err, errlen, "That is the library Lumen has open.");
      libx_free (x);
      return -1;
    }
  /* Brought up to this version's catalogue first, as opening it would. */
  fsys_join (path, sizeof path, x->other, "Catalogue");
  if (cat_open (&c, path, err, errlen))
    {
      libx_free (x);
      return -1;
    }
  cat_close (c);
  rc = attach (x, x->other);
  if (!rc)
    {
      rc = plan_import (x) || survey (x);
      detach (x);
    }
  if (rc)
    {
      snprintf (err, errlen, "%s", x->err[0] ? x->err : "Not enough memory");
      libx_free (x);
      return -1;
    }
  snprintf (x->s.name, sizeof x->s.name, "%s", fsys_leaf (x->other));
  *sv = x->s;
  *out = x;
  return 0;
}

void libx_import_go (libxfer *x, int copy)
{
  int i;
  if (x->export || x->phase != PH_PLANNED)
    return;
  x->copy = copy;
  for (i = 0; i < x->norig; i++)
    {
      xorig *o = &x->orig[i];
      char dst[1100];
      if (o->map)
        continue;
      if (copy && o->exists)
        {
          place (x, lib_root (x->l), o->project, o->root, o->path, 1, dst, sizeof dst);
          o->dst = dup (dst);
          if (add_job (x, o->path, dst, 0))
            x->failed = 1;
        }
      else
        o->dst = dup (o->path);
    }
  x->phase = PH_ORIGS;
}

static int merge (libxfer *x)
{
  sqlite3 *db = dbof (x);
  sqlite3_stmt *s = NULL, *q = NULL, *ins = NULL;
  idmap pm = { 0 }, vm = { 0 }, km = { 0 }, am = { 0 };
  long long batch;
  int i, rc = -1;
  char sql[800];
  time_t now = time (NULL);

  if (attach (x, x->other))
    return -1;
  if (run (x, "BEGIN"))
    {
      detach (x);
      return -1;
    }
  batch = one (x, "SELECT IFNULL(MAX(batch), 0) + 1 FROM main.original", 1);
  x->known = 0;
  x->photos = 0;


  /* originals */
  snprintf (sql, sizeof sql, "INSERT INTO main.original (%s) SELECT ?2, size, mtime, hash, filetype, format,"
            " width, height, orientation, taken, make, model, exposure, fnumber, iso, focal, thumb_offset,"
            " thumb_length, ?3, ?4 FROM x.original WHERE id = ?1", ORIG_COLS);
  if (!(ins = prep (x, sql)))
    goto out;
  for (i = 0; i < x->norig; i++)
    {
      xorig *o = &x->orig[i];
      const char *path;
      /* Found again, inside the transaction: what was here when the import
         was planned may have been removed (or its id reused) since. */
      path = o->dst && !o->copy_failed ? o->dst : o->path;
      /* added meanwhile (imported while this copied)? */
      if ((o->map = find_original (x, path, o->size, o->hash)) < 0)
        goto out;
      if (o->map)
        {
          x->known++;
          continue;
        }
      sqlite3_bind_int64 (ins, 1, o->id);
      sqlite3_bind_text (ins, 2, path, -1, SQLITE_TRANSIENT);
      sqlite3_bind_int64 (ins, 3, batch);
      sqlite3_bind_int (ins, 4, !exists (path));
      if (sqlite3_step (ins) != SQLITE_DONE)
        {
          sqlfail (x, "Can't add the photos");
          goto out;
        }
      sqlite3_reset (ins);
      o->map = sqlite3_last_insert_rowid (db);
      x->photos++;
    }
  sqlite3_finalize (ins);
  ins = NULL;

  /* projects, by name */
  if (!(s = prep (x, "SELECT id, name, created FROM x.project ORDER BY id"))
      || !(q = prep (x, "SELECT id FROM main.project WHERE name = ?1"))
      || !(ins = prep (x, "INSERT INTO main.project (name, created, root, watch, folder_albums) VALUES (?1, ?2, ?3, 0, 0)")))
    goto out;
  while (sqlite3_step (s) == SQLITE_ROW)
    {
      cat_id id = 0;
      sqlite3_bind_text (q, 1, ctext (s, 1), -1, SQLITE_TRANSIENT);
      if (sqlite3_step (q) == SQLITE_ROW)
        id = sqlite3_column_int64 (q, 0);
      sqlite3_reset (q);
      if (!id)
        {
          /* Only if any of its photos come across. */
          sqlite3_stmt *u = prep (x, "SELECT 1 FROM x.version WHERE project = ?1 LIMIT 1");
          int used = 0;
          if (u)
            {
              sqlite3_bind_int64 (u, 1, sqlite3_column_int64 (s, 0));
              used = sqlite3_step (u) == SQLITE_ROW;
              sqlite3_finalize (u);
            }
          if (!used)
            continue;
          sqlite3_bind_text (ins, 1, ctext (s, 1), -1, SQLITE_TRANSIENT);
          sqlite3_bind_int64 (ins, 2, sqlite3_column_type (s, 2) == SQLITE_NULL ? (sqlite3_int64) now : sqlite3_column_int64 (s, 2));
          sqlite3_bind_null (ins, 3);
          if (sqlite3_step (ins) != SQLITE_DONE)
            {
              sqlfail (x, "Can't add the projects");
              goto out;
            }
          sqlite3_reset (ins);
          id = sqlite3_last_insert_rowid (db);
        }
      if (map_put (&pm, sqlite3_column_int64 (s, 0), id, 0))
        goto out;
    }
  sqlite3_finalize (s);
  sqlite3_finalize (q);
  sqlite3_finalize (ins);
  s = q = ins = NULL;

  /* versions: one with the same adjustments as one here is that one */
  snprintf (sql, sizeof sql, "INSERT INTO main.version (%s) VALUES (?1,?2,?3,?4,?5,?6,?7,?8,?9,?10,NULL,?11)", VER_COLS);
  if (!(s = prep (x, "SELECT id, original, project, name, rating, flag, recipe, thumb, created, modified,"
                     " caption, label FROM x.version ORDER BY id"))
      || !(q = prep (x, "SELECT id FROM main.version WHERE original = ?1 AND recipe = ?2 ORDER BY id LIMIT 1"))
      || !(ins = prep (x, sql)))
    goto out;
  while (sqlite3_step (s) == SQLITE_ROW)
    {
      cat_id xid = sqlite3_column_int64 (s, 0), id = 0;
      xorig *o = orig_of (x, sqlite3_column_int64 (s, 1));
      cat_id proj = map_get (&pm, sqlite3_column_int64 (s, 2));
      const char *recipe = ctext (s, 6);
      int thumb = sqlite3_column_int (s, 7);
      if (!o || !o->map || !proj)
        continue;
      sqlite3_bind_int64 (q, 1, o->map);
      sqlite3_bind_text (q, 2, recipe, -1, SQLITE_TRANSIENT);
      if (sqlite3_step (q) == SQLITE_ROW)
        id = sqlite3_column_int64 (q, 0);
      sqlite3_reset (q);
      if (id)
        {
          x->skipped_versions++;
          if (map_put (&vm, xid, id, 0))
            goto out;
          continue;
        }
      {
        char a[1100];
        thumb_path (x->other, xid, a, sizeof a);
        if (thumb != 1 || !exists (a))
          thumb = 0;                   /* made here */
        sqlite3_bind_int64 (ins, 1, o->map);
        sqlite3_bind_int64 (ins, 2, proj);
        if (sqlite3_column_type (s, 3) == SQLITE_NULL)
          sqlite3_bind_null (ins, 3);
        else
          sqlite3_bind_text (ins, 3, ctext (s, 3), -1, SQLITE_TRANSIENT);
        sqlite3_bind_int (ins, 4, sqlite3_column_int (s, 4));
        sqlite3_bind_int (ins, 5, sqlite3_column_int (s, 5));
        sqlite3_bind_text (ins, 6, recipe, -1, SQLITE_TRANSIENT);
        sqlite3_bind_int (ins, 7, thumb);
        sqlite3_bind_int64 (ins, 8, sqlite3_column_int64 (s, 8));
        sqlite3_bind_int64 (ins, 9, sqlite3_column_int64 (s, 9));
        sqlite3_bind_text (ins, 10, ctext (s, 10), -1, SQLITE_TRANSIENT);
        sqlite3_bind_int (ins, 11, sqlite3_column_int (s, 11));
        if (sqlite3_step (ins) != SQLITE_DONE)
          {
            sqlfail (x, "Can't add the versions");
            goto out;
          }
        sqlite3_reset (ins);
        id = sqlite3_last_insert_rowid (db);
        x->versions++;
        if (map_put (&vm, xid, id, 1))
          goto out;
        if (thumb == 1)
          {
            char b[1100];
            lib_thumb_path (x->l, id, b, sizeof b);
            if (add_job (x, a, b, id))
              goto out;
          }
      }
    }
  sqlite3_finalize (s);
  sqlite3_finalize (q);
  sqlite3_finalize (ins);
  s = q = ins = NULL;

  /* keywords, by name (case ignored), and which versions have them */
  if (!(s = prep (x, "SELECT id, name FROM x.keyword ORDER BY id"))
      || !(ins = prep (x, "INSERT OR IGNORE INTO main.keyword (name) VALUES (?1)"))
      || !(q = prep (x, "SELECT id FROM main.keyword WHERE name = ?1")))
    goto out;
  while (sqlite3_step (s) == SQLITE_ROW)
    {
      sqlite3_bind_text (ins, 1, ctext (s, 1), -1, SQLITE_TRANSIENT);
      sqlite3_step (ins);
      sqlite3_reset (ins);
      sqlite3_bind_text (q, 1, ctext (s, 1), -1, SQLITE_TRANSIENT);
      if (sqlite3_step (q) == SQLITE_ROW && map_put (&km, sqlite3_column_int64 (s, 0), sqlite3_column_int64 (q, 0), 0))
        goto out;
      sqlite3_reset (q);
    }
  sqlite3_finalize (s);
  sqlite3_finalize (q);
  sqlite3_finalize (ins);
  s = q = ins = NULL;
  if (!(s = prep (x, "SELECT version, keyword FROM x.version_keyword"))
      || !(ins = prep (x, "INSERT OR IGNORE INTO main.version_keyword (version, keyword) VALUES (?1, ?2)")))
    goto out;
  while (sqlite3_step (s) == SQLITE_ROW)
    {
      cat_id v = map_get (&vm, sqlite3_column_int64 (s, 0)), k = map_get (&km, sqlite3_column_int64 (s, 1));
      if (!v || !k)
        continue;
      sqlite3_bind_int64 (ins, 1, v);
      sqlite3_bind_int64 (ins, 2, k);
      sqlite3_step (ins);
      sqlite3_reset (ins);
    }
  sqlite3_finalize (s);
  sqlite3_finalize (ins);
  s = ins = NULL;

  /* albums, by name; smart ones only if there isn't one of that name */
  if (!(s = prep (x, "SELECT id, name, created, smart FROM x.album WHERE folder IS NULL ORDER BY id"))
      || !(q = prep (x, "SELECT id FROM main.album WHERE name = ?1 AND folder IS NULL"
                        " AND (smart IS NULL) = (?2 IS NULL) ORDER BY id LIMIT 1"))
      || !(ins = prep (x, "INSERT INTO main.album (name, created, smart) VALUES (?1, ?2, ?3)")))
    goto out;
  while (sqlite3_step (s) == SQLITE_ROW)
    {
      cat_id id = 0;
      int smart = sqlite3_column_type (s, 3) != SQLITE_NULL;
      sqlite3_bind_text (q, 1, ctext (s, 1), -1, SQLITE_TRANSIENT);
      if (smart)
        sqlite3_bind_text (q, 2, ctext (s, 3), -1, SQLITE_TRANSIENT);
      else
        sqlite3_bind_null (q, 2);
      if (sqlite3_step (q) == SQLITE_ROW)
        id = sqlite3_column_int64 (q, 0);
      sqlite3_reset (q);
      if (id && smart)
        continue;                      /* its own rule stays */
      if (!id)
        {
          sqlite3_bind_text (ins, 1, ctext (s, 1), -1, SQLITE_TRANSIENT);
          sqlite3_bind_int64 (ins, 2, sqlite3_column_type (s, 2) == SQLITE_NULL ? (sqlite3_int64) now : sqlite3_column_int64 (s, 2));
          if (smart)
            sqlite3_bind_text (ins, 3, ctext (s, 3), -1, SQLITE_TRANSIENT);
          else
            sqlite3_bind_null (ins, 3);
          if (sqlite3_step (ins) != SQLITE_DONE)
            {
              sqlfail (x, "Can't add the albums");
              goto out;
            }
          sqlite3_reset (ins);
          id = sqlite3_last_insert_rowid (db);
        }
      if (!smart && map_put (&am, sqlite3_column_int64 (s, 0), id, 0))
        goto out;
    }
  sqlite3_finalize (s);
  sqlite3_finalize (q);
  sqlite3_finalize (ins);
  s = q = ins = NULL;
  /* their photos, after the ones already in them */
  if (!(s = prep (x, "SELECT album, version FROM x.album_item ORDER BY album, pos"))
      || !(ins = prep (x, "INSERT OR IGNORE INTO main.album_item (album, version, pos) VALUES (?1, ?2,"
                          " (SELECT IFNULL(MAX(pos), 0) + 1 FROM main.album_item WHERE album = ?1))")))
    goto out;
  while (sqlite3_step (s) == SQLITE_ROW)
    {
      cat_id a = map_get (&am, sqlite3_column_int64 (s, 0)), v = map_get (&vm, sqlite3_column_int64 (s, 1));
      if (!a || !v)
        continue;
      sqlite3_bind_int64 (ins, 1, a);
      sqlite3_bind_int64 (ins, 2, v);
      sqlite3_step (ins);
      sqlite3_reset (ins);
    }
  sqlite3_finalize (s);
  sqlite3_finalize (ins);
  s = ins = NULL;

  /* stacks whose versions all came in new */
  if (!(s = prep (x, "SELECT id, pick, open FROM x.stack ORDER BY id"))
      || !(q = prep (x, "SELECT id FROM x.version WHERE stack = ?1"))
      || !(ins = prep (x, "INSERT INTO main.stack (pick, open) VALUES (?1, ?2)")))
    goto out;
  while (sqlite3_step (s) == SQLITE_ROW)
    {
      cat_id members[256], st, pick = map_get (&vm, sqlite3_column_int64 (s, 1));
      int n = 0, ok = 1, j;
      sqlite3_bind_int64 (q, 1, sqlite3_column_int64 (s, 0));
      while (sqlite3_step (q) == SQLITE_ROW)
        {
          idpair *p = map_find (&vm, sqlite3_column_int64 (q, 0));
          if (!p || !p->added || n == 256)
            ok = 0;
          else
            members[n++] = p->to;
        }
      sqlite3_reset (q);
      if (!ok || n < 2)
        continue;
      if (!pick)
        pick = members[0];
      sqlite3_bind_int64 (ins, 1, pick);
      sqlite3_bind_int (ins, 2, sqlite3_column_int (s, 2));
      if (sqlite3_step (ins) != SQLITE_DONE)
        {
          sqlfail (x, "Can't add the stacks");
          goto out;
        }
      sqlite3_reset (ins);
      st = sqlite3_last_insert_rowid (db);
      for (j = 0; j < n; j++)
        {
          sqlite3_stmt *u = prep (x, "UPDATE main.version SET stack = ?1 WHERE id = ?2");
          if (!u)
            goto out;
          sqlite3_bind_int64 (u, 1, st);
          sqlite3_bind_int64 (u, 2, members[j]);
          sqlite3_step (u);
          sqlite3_finalize (u);
        }
    }
  sqlite3_finalize (s);
  sqlite3_finalize (q);
  sqlite3_finalize (ins);
  s = q = ins = NULL;

  if (run (x, "INSERT OR IGNORE INTO main.preset (name, recipe) SELECT name, recipe FROM x.preset")
      || run (x, "COMMIT"))
    goto out;
  rc = 0;
out:
  sqlite3_finalize (s);
  sqlite3_finalize (q);
  sqlite3_finalize (ins);
  if (rc)
    {
      sqlite3_exec (db, "ROLLBACK", NULL, NULL, NULL);
      /* thumbnails planned for versions that aren't there */
      for (i = x->next; i < x->njobs; i++)
        if (x->jobs[i].thumb_of)
          x->jobs[i].thumb_of = -1;
    }
  detach (x);
  free (pm.p);
  free (vm.p);
  free (km.p);
  free (am.p);
  return rc;
}

/* --- working ------------------------------------------------------------- */

static int copy_one (libxfer *x, job *j)
{
  char parent[1100];
  const char *leaf = fsys_leaf (j->dst);
  fsys_entry from;
  int typed;
  if (j->thumb_of < 0)
    return 0;
  snprintf (parent, sizeof parent, "%.*s", (int) (leaf - j->dst > 0 ? leaf - j->dst - 1 : 0), j->dst);
  typed = fsys_stat (j->src, &from) == 0 && !from.is_dir && from.filetype >= 0;
  if ((parent[0] && fsys_mkdirs (parent)) || fsys_copy_tree (j->src, j->dst))
    {
      x->copy_failed++;
      if (!x->err[0])
        snprintf (x->err, sizeof x->err, "Can't copy %s to %s", j->src, j->dst);
      if (j->thumb_of > 0)
        cat_set_thumb (lib_cat (x->l), j->thumb_of, THUMB_NONE);   /* made here instead */
      return -1;
    }
  if (typed)
    fsys_settype (j->dst, from.filetype);   /* as it was (not every filing system's copy keeps it) */
  x->copied++;
  return 0;
}

int libx_work (libxfer *x, unsigned (*now) (void), unsigned deadline)
{
  int flags = 0, steps = 0;
  if (x->phase == PH_PLANNED || x->phase == PH_DONE)
    return 0;
  while (x->phase != PH_DONE && (steps == 0 || (int) (now () - deadline) < 0))
    {
      steps++;
      if (x->phase == PH_ORIGS || x->phase == PH_THUMBS)
        {
          if (x->next < x->njobs)
            {
              job *j = &x->jobs[x->next++];
              if (copy_one (x, j) && !j->thumb_of && !x->export)
                {
                  int i;
                  for (i = 0; i < x->norig; i++)
                    if (x->orig[i].dst && !strcmp (x->orig[i].dst, j->dst) && !x->orig[i].map)
                      x->orig[i].copy_failed = 1;
                }
              if (j->thumb_of && x->next == x->njobs)
                flags |= LIB_THUMBS_CHANGED;
              else if (j->thumb_of && x->next % 32 == 0)
                flags |= LIB_THUMBS_CHANGED;
              continue;
            }
          if (x->export || x->phase == PH_THUMBS)
            {
              x->phase = PH_DONE;
              if (!x->export)
                lib_thumbs_changed (x->l);
              flags |= LIB_THUMBS_CHANGED;
              break;
            }
          x->phase = PH_MERGE;
          continue;
        }
      if (x->phase == PH_MERGE)
        {
          if (merge (x))
            {
              x->failed = 1;
              x->phase = PH_DONE;
              break;
            }
          lib_thumbs_changed (x->l);
          flags |= LIB_LIST_CHANGED;
          x->phase = PH_THUMBS;
          break;                        /* the list is shown again before the thumbnails */
        }
    }
  return flags;
}

int libx_finished (libxfer *x) { return x->phase == PH_DONE; }
int libx_started (libxfer *x) { return x->phase != PH_PLANNED; }
int libx_is_export (libxfer *x) { return x->export; }

void libx_progress (libxfer *x, int *done, int *total, char *what, size_t n)
{
  *done = x->next;
  *total = x->njobs;
  if (x->export)
    snprintf (what, n, "Exporting %d photo%s to %s", x->photos, x->photos == 1 ? "" : "s", fsys_leaf (x->other));
  else if (x->phase == PH_ORIGS)
    snprintf (what, n, "Copying photos from %s", fsys_leaf (x->other));
  else
    snprintf (what, n, "Importing the library %s", fsys_leaf (x->other));
}

void libx_result (libxfer *x, char *out, size_t n, int *failed)
{
  const char *leaf = fsys_leaf (x->other);
  *failed = x->failed;
  if (x->failed)
    {
      snprintf (out, n, "%s", x->err[0] ? x->err : "It didn't work.");
      return;
    }
  if (x->export)
    {
      int len = snprintf (out, n, "Exported %d photo%s to %s%s.", x->photos, x->photos == 1 ? "" : "s", leaf,
                          x->originals ? ", with their originals" : "");
      if (x->originals && x->missing && len > 0 && (size_t) len < n)
        len += snprintf (out + len, n - (size_t) len, " %d original%s couldn't be found and %s left out.",
                         x->missing, x->missing == 1 ? "" : "s", x->missing == 1 ? "was" : "were");
      if (x->copy_failed && len > 0 && (size_t) len < n)
        snprintf (out + len, n - (size_t) len, " %d file%s couldn't be copied.", x->copy_failed, x->copy_failed == 1 ? "" : "s");
      return;
    }
  {
    int len = snprintf (out, n, "From %s: %d new photo%s", leaf, x->photos, x->photos == 1 ? "" : "s");
    int extra = x->versions - x->photos;
    if (len > 0 && (size_t) len < n && extra > 0)
      len += snprintf (out + len, n - (size_t) len, " and %d more version%s", extra, extra == 1 ? "" : "s");
    if (len > 0 && (size_t) len < n)
      len += snprintf (out + len, n - (size_t) len, "%s", x->copy && x->photos ? ", copied into this library." : ".");
    if (len > 0 && (size_t) len < n && x->known)
      len += snprintf (out + len, n - (size_t) len, " %d %s already here.", x->known, x->known == 1 ? "was" : "were");
    if (len > 0 && (size_t) len < n && x->copy_failed)
      snprintf (out + len, n - (size_t) len, " %d file%s couldn't be copied.", x->copy_failed, x->copy_failed == 1 ? "" : "s");
  }
}

void libx_counts (libxfer *x, int *photos, int *versions)
{
  *photos = x->photos;
  *versions = x->versions;
}

int libx_measure (library *l, const cat_id *v, int n, int *photos, long long *bytes)
{
  sqlite3 *db = cat_sqlite (lib_cat (l));
  sqlite3_stmt *s;
  int i, rc = -1;
  *photos = 0;
  *bytes = 0;
  if (n >= 0)
    {
      if (sqlite3_exec (db, "CREATE TEMP TABLE IF NOT EXISTS libx_sel (id INTEGER PRIMARY KEY);"
                            "DELETE FROM temp.libx_sel;", NULL, NULL, NULL) != SQLITE_OK
          || sqlite3_prepare_v2 (db, "INSERT OR IGNORE INTO temp.libx_sel VALUES (?1)", -1, &s, NULL) != SQLITE_OK)
        return -1;
      for (i = 0; i < n; i++)
        {
          sqlite3_bind_int64 (s, 1, v[i]);
          sqlite3_step (s);
          sqlite3_reset (s);
        }
      sqlite3_finalize (s);
    }
  if (sqlite3_prepare_v2 (db, n < 0 ? "SELECT COUNT(*), IFNULL(SUM(size), 0) FROM original"
                                      " WHERE id IN (SELECT original FROM version)"
                                    : "SELECT COUNT(*), IFNULL(SUM(size), 0) FROM original WHERE id IN"
                                      " (SELECT original FROM version WHERE id IN (SELECT id FROM temp.libx_sel))",
                          -1, &s, NULL) == SQLITE_OK)
    {
      if (sqlite3_step (s) == SQLITE_ROW)
        {
          *photos = sqlite3_column_int (s, 0);
          *bytes = sqlite3_column_int64 (s, 1);
          rc = 0;
        }
      sqlite3_finalize (s);
    }
  if (n >= 0)
    sqlite3_exec (db, "DELETE FROM temp.libx_sel", NULL, NULL, NULL);
  return rc;
}

void libx_free (libxfer *x)
{
  int i;
  if (!x)
    return;
  for (i = 0; i < x->njobs; i++)
    {
      free (x->jobs[i].src);
      free (x->jobs[i].dst);
    }
  free (x->jobs);
  for (i = 0; i < x->norig; i++)
    {
      free (x->orig[i].path);
      free (x->orig[i].dst);
      free (x->orig[i].project);
      free (x->orig[i].root);
    }
  free (x->orig);
  set_free (&x->used);
  free (x);
}
