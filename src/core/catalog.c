/* catalog.c: see catalog.h. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <ctype.h>
#include <strings.h>
#include "sqlite3.h"
#include "catalog.h"
#include "fsys.h"
#include "recipe.h"

struct cat
{
  sqlite3 *db;
  char err[256];
};

#define SCHEMA_VERSION 1

static void col_text (sqlite3_stmt *s, int i, char *out, size_t n);

static const char schema[] =
  "CREATE TABLE IF NOT EXISTS meta (key TEXT PRIMARY KEY, value);"
  "CREATE TABLE IF NOT EXISTS project (id INTEGER PRIMARY KEY, name TEXT NOT NULL UNIQUE, created INTEGER);"
  "CREATE TABLE IF NOT EXISTS original ("
  "  id INTEGER PRIMARY KEY, path TEXT NOT NULL UNIQUE, size INTEGER, mtime INTEGER, hash INTEGER,"
  "  filetype INTEGER, format INTEGER, width INTEGER, height INTEGER, orientation INTEGER,"
  "  taken TEXT, make TEXT, model TEXT, exposure REAL, fnumber REAL, iso INTEGER, focal REAL,"
  "  thumb_offset INTEGER, thumb_length INTEGER, batch INTEGER);"
  "CREATE INDEX IF NOT EXISTS original_hash ON original (size, hash);"
  "CREATE TABLE IF NOT EXISTS version ("
  "  id INTEGER PRIMARY KEY, original INTEGER NOT NULL REFERENCES original(id),"
  "  project INTEGER NOT NULL REFERENCES project(id), name TEXT,"
  "  rating INTEGER NOT NULL DEFAULT 0, flag INTEGER NOT NULL DEFAULT 0,"
  "  recipe TEXT NOT NULL DEFAULT '', thumb INTEGER NOT NULL DEFAULT 0,"
  "  created INTEGER, modified INTEGER);"
  "CREATE INDEX IF NOT EXISTS version_project ON version (project);"
  "CREATE INDEX IF NOT EXISTS version_thumb ON version (thumb);"
  "CREATE TABLE IF NOT EXISTS album (id INTEGER PRIMARY KEY, name TEXT NOT NULL, created INTEGER);"
  "CREATE TABLE IF NOT EXISTS album_item (album INTEGER NOT NULL, version INTEGER NOT NULL,"
  "  pos INTEGER, PRIMARY KEY (album, version));"
  "CREATE TABLE IF NOT EXISTS preset (id INTEGER PRIMARY KEY, name TEXT NOT NULL UNIQUE, recipe TEXT NOT NULL);";

static int fail (cat *c, const char *what)
{
  snprintf (c->err, sizeof c->err, "%s: %s", what, sqlite3_errmsg (c->db));
  return -1;
}

static int exec (cat *c, const char *sql)
{
  char *msg = NULL;
  if (sqlite3_exec (c->db, sql, NULL, NULL, &msg) != SQLITE_OK)
    {
      snprintf (c->err, sizeof c->err, "%s", msg ? msg : "SQLite error");
      sqlite3_free (msg);
      return -1;
    }
  return 0;
}

static sqlite3_stmt *prep (cat *c, const char *sql);
static void bind_text_or_null (sqlite3_stmt *s, int i, const char *t);

/* Schema 2 (0.0.16): originals whose files can't be found are marked. */
/* One schema step, all or nothing: a step half done (power lost after an
   ALTER TABLE) would fail for ever after with "duplicate column". */
static int step (cat *c, const char *sql)
{
  if (exec (c, "BEGIN") || exec (c, sql))
    {
      char keep[256];
      snprintf (keep, sizeof keep, "%s", c->err);
      exec (c, "ROLLBACK");
      snprintf (c->err, sizeof c->err, "%s", keep);
      return -1;
    }
  return exec (c, "COMMIT");
}

static int upgrade (cat *c)
{
  sqlite3_stmt *s = prep (c, "SELECT value FROM meta WHERE key = 'schema'");
  int v = 1;
  if (!s)
    return -1;
  if (sqlite3_step (s) == SQLITE_ROW)
    v = sqlite3_column_int (s, 0);
  sqlite3_finalize (s);
  if (v < 2 && step (c, "ALTER TABLE original ADD COLUMN missing INTEGER NOT NULL DEFAULT 0;"
                        "UPDATE meta SET value = 2 WHERE key = 'schema';"))
    return -1;
  /* Schema 3 (0.0.19): a project's folder, whether it is watched and has
     albums from its folders; a folder album's project and folder. */
  if (v < 3 && step (c, "ALTER TABLE project ADD COLUMN root TEXT;"
                        "ALTER TABLE project ADD COLUMN watch INTEGER NOT NULL DEFAULT 0;"
                        "ALTER TABLE project ADD COLUMN folder_albums INTEGER NOT NULL DEFAULT 0;"
                        "ALTER TABLE album ADD COLUMN project INTEGER;"
                        "ALTER TABLE album ADD COLUMN folder TEXT;"
                        "UPDATE meta SET value = 3 WHERE key = 'schema';"))
    return -1;
  /* Schema 4 (0.0.23): captions, keywords, and smart albums' rules. */
  if (v < 4 && step (c, "ALTER TABLE version ADD COLUMN caption TEXT NOT NULL DEFAULT '';"
                        "CREATE TABLE IF NOT EXISTS keyword (id INTEGER PRIMARY KEY, name TEXT NOT NULL UNIQUE COLLATE NOCASE);"
                        "CREATE TABLE IF NOT EXISTS version_keyword (version INTEGER NOT NULL, keyword INTEGER NOT NULL,"
                        "  PRIMARY KEY (version, keyword));"
                        "CREATE INDEX IF NOT EXISTS version_keyword_kw ON version_keyword (keyword);"
                        "ALTER TABLE album ADD COLUMN smart TEXT;"
                        "UPDATE meta SET value = 4 WHERE key = 'schema';"))
    return -1;
  /* Schema 5 (0.0.23): stacks. */
  if (v < 5 && step (c, "ALTER TABLE version ADD COLUMN stack INTEGER;"
                        "CREATE TABLE IF NOT EXISTS stack (id INTEGER PRIMARY KEY, pick INTEGER NOT NULL,"
                        "  open INTEGER NOT NULL DEFAULT 0);"
                        "CREATE INDEX IF NOT EXISTS version_stack ON version (stack);"
                        "UPDATE meta SET value = 5 WHERE key = 'schema';"))
    return -1;
  /* Schema 6 (0.0.25): colour labels. */
  if (v < 6 && step (c, "ALTER TABLE version ADD COLUMN label INTEGER NOT NULL DEFAULT 0;"
                        "UPDATE meta SET value = 6 WHERE key = 'schema';"))
    return -1;
  return 0;
}

int cat_open (cat **out, const char *path, char *err, size_t errlen)
{
  cat *c = calloc (1, sizeof *c);
  *out = NULL;
  if (!c)
    {
      snprintf (err, errlen, "Not enough memory");
      return -1;
    }
  if (sqlite3_open_v2 (path, &c->db, SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE, NULL) != SQLITE_OK)
    {
      snprintf (err, errlen, "Can't open the catalogue %s: %s", path, c->db ? sqlite3_errmsg (c->db) : "out of memory");
      sqlite3_close (c->db);
      free (c);
      return -1;
    }
  if (exec (c, "PRAGMA locking_mode=EXCLUSIVE; PRAGMA journal_mode=TRUNCATE;"
               "PRAGMA synchronous=NORMAL; PRAGMA foreign_keys=ON;")
      || exec (c, schema)
      || exec (c, "INSERT OR IGNORE INTO meta VALUES ('schema', 1);")
      || upgrade (c))
    {
      snprintf (err, errlen, "%s", c->err);
      cat_close (c);
      return -1;
    }
  *out = c;
  return 0;
}

void cat_close (cat *c)
{
  if (c)
    {
      sqlite3_close (c->db);
      free (c);
    }
}

const char *cat_error (cat *c)
{
  return c->err;
}

int cat_begin (cat *c) { return exec (c, "BEGIN"); }
int cat_commit (cat *c) { return exec (c, "COMMIT"); }

static sqlite3_stmt *prep (cat *c, const char *sql)
{
  sqlite3_stmt *s = NULL;
  if (sqlite3_prepare_v2 (c->db, sql, -1, &s, NULL) != SQLITE_OK)
    {
      fail (c, "prepare");
      return NULL;
    }
  return s;
}

cat_id cat_project_ensure (cat *c, const char *name)
{
  sqlite3_stmt *s = prep (c, "INSERT OR IGNORE INTO project (name, created) VALUES (?1, ?2)");
  cat_id id = -1;
  if (!s)
    return -1;
  sqlite3_bind_text (s, 1, name, -1, SQLITE_TRANSIENT);
  sqlite3_bind_int64 (s, 2, (sqlite3_int64) time (NULL));
  sqlite3_step (s);
  sqlite3_finalize (s);
  s = prep (c, "SELECT id FROM project WHERE name = ?1");
  if (!s)
    return -1;
  sqlite3_bind_text (s, 1, name, -1, SQLITE_TRANSIENT);
  if (sqlite3_step (s) == SQLITE_ROW)
    id = sqlite3_column_int64 (s, 0);
  sqlite3_finalize (s);
  return id;
}

cat_id cat_album_create (cat *c, const char *name)
{
  sqlite3_stmt *s = prep (c, "INSERT INTO album (name, created) VALUES (?1, ?2)");
  if (!s)
    return -1;
  sqlite3_bind_text (s, 1, name, -1, SQLITE_TRANSIENT);
  sqlite3_bind_int64 (s, 2, (sqlite3_int64) time (NULL));
  if (sqlite3_step (s) != SQLITE_DONE)
    {
      fail (c, "new album");
      sqlite3_finalize (s);
      return -1;
    }
  sqlite3_finalize (s);
  return sqlite3_last_insert_rowid (c->db);
}

static int for_each_version (cat *c, const char *sql, cat_id a, const cat_id *v, int n, int b)
{
  sqlite3_stmt *s = prep (c, sql);
  int i, rc = 0;
  if (!s)
    return -1;
  exec (c, "SAVEPOINT many");
  for (i = 0; i < n; i++)
    {
      sqlite3_reset (s);
      sqlite3_bind_int64 (s, 1, a);
      sqlite3_bind_int64 (s, 2, v[i]);
      sqlite3_bind_int (s, 3, b);
      if (sqlite3_step (s) != SQLITE_DONE)
        {
          rc = fail (c, "update");
          break;
        }
    }
  sqlite3_finalize (s);
  exec (c, rc ? "ROLLBACK TO many; RELEASE many" : "RELEASE many");
  return rc;
}

int cat_album_add (cat *c, cat_id album, const cat_id *v, int n)
{
  cat_rule r;
  int smart = cat_smart_get (c, album, &r);
  if (smart)
    {
      if (smart > 0)
        snprintf (c->err, sizeof c->err, "A smart album gathers its own photos: change its rule instead.");
      return -1;
    }
  return for_each_version (c,
    "INSERT OR IGNORE INTO album_item (album, version, pos) VALUES (?1, ?2,"
    " (SELECT IFNULL(MAX(pos), 0) + 1 + ?3 FROM album_item WHERE album = ?1))", album, v, n, 0);
}

int cat_album_remove (cat *c, cat_id album, const cat_id *v, int n)
{
  return for_each_version (c, "DELETE FROM album_item WHERE album = ?1 AND version = ?2 AND ?3 = ?3", album, v, n, 0);
}

int cat_set_rating (cat *c, const cat_id *v, int n, int rating)
{
  return for_each_version (c,
    "UPDATE version SET rating = ?3, modified = ?1 WHERE id = ?2", (cat_id) time (NULL), v, n, rating);
}

const char *const cat_label_names[CAT_LABELS] = { "", "Red", "Orange", "Yellow", "Green", "Blue", "Purple", "Grey" };

int cat_set_label (cat *c, const cat_id *v, int n, int label)
{
  if (label < 0 || label >= CAT_LABELS)
    label = 0;
  return for_each_version (c, "UPDATE version SET label = ?3, modified = ?1 WHERE id = ?2", (cat_id) time (NULL),
                           v, n, label);
}

int cat_set_flag (cat *c, const cat_id *v, int n, int flag)
{
  if (flag < 0)
    return for_each_version (c,
      "UPDATE version SET flag = 1 - flag, modified = ?1 WHERE id = ?2 AND ?3 = ?3", (cat_id) time (NULL), v, n, 0);
  return for_each_version (c,
    "UPDATE version SET flag = ?3, modified = ?1 WHERE id = ?2", (cat_id) time (NULL), v, n, flag ? 1 : 0);
}

/* Stacks left with one version (or none) go; a stack whose pick went
   gets its first version as pick. */
static int stacks_tidy (cat *c)
{
  return exec (c, "UPDATE version SET stack = NULL WHERE stack IN"
                  " (SELECT stack FROM version WHERE stack IS NOT NULL GROUP BY stack HAVING COUNT(*) < 2);"
                  "DELETE FROM stack WHERE id NOT IN (SELECT DISTINCT stack FROM version WHERE stack IS NOT NULL);"
                  "UPDATE stack SET pick = (SELECT MIN(id) FROM version WHERE version.stack = stack.id)"
                  " WHERE pick NOT IN (SELECT id FROM version WHERE version.stack = stack.id);");
}

/* --- removing and renaming */

int cat_remove_versions (cat *c, const cat_id *v, int n)
{
  static const char *const steps[] = {
    "DELETE FROM album_item WHERE version = ?1",
    "DELETE FROM version_keyword WHERE version = ?1",
    "DELETE FROM version WHERE id = ?1",
  };
  size_t k;
  int i, rc = 0;
  exec (c, "SAVEPOINT removing");
  for (k = 0; k < sizeof steps / sizeof steps[0] && !rc; k++)
    {
      sqlite3_stmt *s = prep (c, steps[k]);
      if (!s)
        {
          rc = -1;
          break;
        }
      for (i = 0; i < n; i++)
        {
          sqlite3_reset (s);
          sqlite3_bind_int64 (s, 1, v[i]);
          if (sqlite3_step (s) != SQLITE_DONE)
            {
              rc = fail (c, "remove");
              break;
            }
        }
      sqlite3_finalize (s);
    }
  /* Originals no version uses any more go (so they can be imported
     again), and projects left empty. */
  if (!rc && exec (c, "DELETE FROM original WHERE id NOT IN (SELECT DISTINCT original FROM version)"))
    rc = -1;
  if (!rc && exec (c, "DELETE FROM project WHERE id NOT IN (SELECT DISTINCT project FROM version)"))
    rc = -1;
  if (!rc && exec (c, "DELETE FROM keyword WHERE id NOT IN (SELECT DISTINCT keyword FROM version_keyword)"))
    rc = -1;
  if (!rc && stacks_tidy (c))
    rc = -1;
  exec (c, rc ? "ROLLBACK TO removing; RELEASE removing" : "RELEASE removing");
  return rc;
}

/* --- originals that moved */

int cat_original_next (cat *c, cat_id after, cat_id *id, char *path, size_t n, int *missing)
{
  sqlite3_stmt *s = prep (c, "SELECT id, path, missing FROM original WHERE id > ?1 ORDER BY id LIMIT 1");
  int r = 0;
  if (!s)
    return -1;
  sqlite3_bind_int64 (s, 1, after);
  if (sqlite3_step (s) == SQLITE_ROW)
    {
      *id = sqlite3_column_int64 (s, 0);
      col_text (s, 1, path, n);
      *missing = sqlite3_column_int (s, 2);
      r = 1;
    }
  sqlite3_finalize (s);
  return r;
}

int cat_set_missing (cat *c, cat_id original, int missing)
{
  sqlite3_stmt *s = prep (c, "UPDATE original SET missing = ?2 WHERE id = ?1");
  int rc = 0;
  if (!s)
    return -1;
  sqlite3_bind_int64 (s, 1, original);
  sqlite3_bind_int (s, 2, missing);
  if (sqlite3_step (s) != SQLITE_DONE)
    rc = fail (c, "missing");
  sqlite3_finalize (s);
  return rc;
}

int cat_relink (cat *c, cat_id version, const char *path, long long mtime)
{
  /* The original's new place; its thumbnail is made again if it failed. */
  sqlite3_stmt *s = prep (c, "UPDATE original SET path = ?2, mtime = ?3, missing = 0"
                             " WHERE id = (SELECT original FROM version WHERE id = ?1)");
  int rc = 0;
  if (!s)
    return -1;
  sqlite3_bind_int64 (s, 1, version);
  sqlite3_bind_text (s, 2, path, -1, SQLITE_TRANSIENT);
  sqlite3_bind_int64 (s, 3, mtime);
  if (sqlite3_step (s) != SQLITE_DONE)
    rc = fail (c, "relink");
  sqlite3_finalize (s);
  if (!rc)
    {
      s = prep (c, "UPDATE version SET thumb = 0 WHERE original = (SELECT original FROM version WHERE id = ?1)"
                   " AND thumb = 2");
      if (s)
        {
          sqlite3_bind_int64 (s, 1, version);
          sqlite3_step (s);
          sqlite3_finalize (s);
        }
    }
  return rc;
}

int cat_set_original_info (cat *c, cat_id version, const img_info *in)
{
  /* Only what it gives: a raw file's own EXIF may have more (an empty
     lens or date from the plug-in doesn't wipe the one read at import). */
  sqlite3_stmt *s = prep (c, "UPDATE original SET width = ?2, height = ?3, orientation = ?4,"
                             " make = COALESCE(?5, make), model = COALESCE(?6, model), taken = COALESCE(?7, taken),"
                             " iso = CASE WHEN ?8 > 0 THEN ?8 ELSE iso END,"
                             " exposure = CASE WHEN ?9 > 0 THEN ?9 ELSE exposure END,"
                             " fnumber = CASE WHEN ?10 > 0 THEN ?10 ELSE fnumber END,"
                             " focal = CASE WHEN ?11 > 0 THEN ?11 ELSE focal END"
                             " WHERE id = (SELECT original FROM version WHERE id = ?1)");
  int rc = 0;
  if (!s)
    return -1;
  sqlite3_bind_int64 (s, 1, version);
  sqlite3_bind_int (s, 2, in->width);
  sqlite3_bind_int (s, 3, in->height);
  sqlite3_bind_int (s, 4, in->orientation);
  bind_text_or_null (s, 5, in->make);
  bind_text_or_null (s, 6, in->model);
  bind_text_or_null (s, 7, in->taken);
  sqlite3_bind_int (s, 8, in->iso);
  sqlite3_bind_double (s, 9, in->exposure);
  sqlite3_bind_double (s, 10, in->fnumber);
  sqlite3_bind_double (s, 11, in->focal);
  if (sqlite3_step (s) != SQLITE_DONE)
    rc = fail (c, "details");
  sqlite3_finalize (s);
  return rc;
}

int cat_retry_failed_thumbs (cat *c)
{
  return exec (c, "UPDATE version SET thumb = 0 WHERE thumb = 2");
}

int cat_tidy (cat *c)
{
  /* Empty projects go, and with them their folder albums. */
  return exec (c, "DELETE FROM project WHERE id NOT IN (SELECT DISTINCT project FROM version);"
                  "DELETE FROM album_item WHERE album IN (SELECT id FROM album WHERE folder IS NOT NULL"
                  "  AND project NOT IN (SELECT id FROM project));"
                  "DELETE FROM album WHERE folder IS NOT NULL AND project NOT IN (SELECT id FROM project)");
}

int cat_count_missing (cat *c)
{
  sqlite3_stmt *s = prep (c, "SELECT COUNT(*) FROM version v JOIN original o ON o.id = v.original WHERE o.missing = 1");
  int n = 0;
  if (!s)
    return 0;
  if (sqlite3_step (s) == SQLITE_ROW)
    n = sqlite3_column_int (s, 0);
  sqlite3_finalize (s);
  return n;
}

int cat_group_versions (cat *c, int kind, cat_id id, cat_id **out, int *count)
{
  cat_rule rule;
  if (kind == SRC_ALBUM && cat_smart_get (c, id, &rule) == 1)
    {
      /* A smart album's photos: those its rule matches now. */
      cat_source src = { SRC_ALBUM, id, 0, "" };
      cat_item *it = NULL;
      int m = 0, k;
      cat_id *a;
      if (cat_list (c, &src, &it, &m))
        return -1;
      a = malloc (sizeof *a * (size_t) (m ? m : 1));
      if (!a)
        {
          free (it);
          return -1;
        }
      for (k = 0; k < m; k++)
        a[k] = it[k].id;
      free (it);
      *out = a;
      *count = m;
      return 0;
    }
  sqlite3_stmt *s = prep (c, kind == SRC_ALBUM ? "SELECT version FROM album_item WHERE album = ?1"
                                               : "SELECT id FROM version WHERE project = ?1");
  int n = 0, cap = 64;
  cat_id *a = malloc (sizeof *a * (size_t) cap);
  if (!s || !a)
    {
      free (a);
      if (s)
        sqlite3_finalize (s);
      return -1;
    }
  sqlite3_bind_int64 (s, 1, id);
  while (sqlite3_step (s) == SQLITE_ROW)
    {
      if (n == cap)
        {
          cat_id *b = realloc (a, sizeof *a * (size_t) cap * 2);
          if (!b)
            break;
          a = b;
          cap *= 2;
        }
      a[n++] = sqlite3_column_int64 (s, 0);
    }
  sqlite3_finalize (s);
  *out = a;
  *count = n;
  return 0;
}

int cat_album_delete (cat *c, cat_id album)
{
  sqlite3_stmt *s;
  int rc = 0;
  exec (c, "SAVEPOINT album_delete");
  s = prep (c, "DELETE FROM album_item WHERE album = ?1");
  if (s)
    {
      sqlite3_bind_int64 (s, 1, album);
      if (sqlite3_step (s) != SQLITE_DONE)
        rc = fail (c, "album");
      sqlite3_finalize (s);
    }
  s = rc ? NULL : prep (c, "DELETE FROM album WHERE id = ?1");
  if (s)
    {
      sqlite3_bind_int64 (s, 1, album);
      if (sqlite3_step (s) != SQLITE_DONE)
        rc = fail (c, "album");
      sqlite3_finalize (s);
    }
  exec (c, rc ? "ROLLBACK TO album_delete; RELEASE album_delete" : "RELEASE album_delete");
  return rc;
}

int cat_group_rename (cat *c, int kind, cat_id id, const char *name)
{
  sqlite3_stmt *s;
  int rc = 0;
  while (*name == ' ')
    name++;
  if (!*name)
    {
      snprintf (c->err, sizeof c->err, "A name is needed.");
      return -1;
    }
  s = prep (c, kind == SRC_ALBUM ? "UPDATE album SET name = ?2 WHERE id = ?1" : "UPDATE project SET name = ?2 WHERE id = ?1");
  if (!s)
    return -1;
  sqlite3_bind_int64 (s, 1, id);
  sqlite3_bind_text (s, 2, name, -1, SQLITE_TRANSIENT);
  if (sqlite3_step (s) != SQLITE_DONE)
    {
      if (sqlite3_errcode (c->db) == SQLITE_CONSTRAINT)
        snprintf (c->err, sizeof c->err, "There is already a project called \"%s\".", name);
      else
        fail (c, "rename");
      rc = -1;
    }
  sqlite3_finalize (s);
  return rc;
}

static void bind_text_or_null (sqlite3_stmt *s, int i, const char *t)
{
  if (t && t[0])
    sqlite3_bind_text (s, i, t, -1, SQLITE_TRANSIENT);
  else
    sqlite3_bind_null (s, i);
}

cat_id cat_add_original (cat *c, cat_id project, const cat_original *o, cat_id *existing)
{
  sqlite3_stmt *s;
  cat_id orig, ver;
  const img_info *in = o->info;
  *existing = 0;
  /* Already here? By path, or by the same content somewhere else. */
  s = prep (c, "SELECT v.id FROM original o JOIN version v ON v.original = o.id"
               " WHERE o.path = ?1 OR (o.size = ?2 AND o.hash = ?3) ORDER BY v.id LIMIT 1");
  if (!s)
    return -1;
  sqlite3_bind_text (s, 1, o->path, -1, SQLITE_TRANSIENT);
  sqlite3_bind_int64 (s, 2, o->size);
  sqlite3_bind_int64 (s, 3, (sqlite3_int64) o->hash);
  if (sqlite3_step (s) == SQLITE_ROW)
    *existing = sqlite3_column_int64 (s, 0);
  sqlite3_finalize (s);
  if (*existing)
    return 0;

  s = prep (c, "INSERT INTO original (path, size, mtime, hash, filetype, format, width, height,"
               " orientation, taken, make, model, exposure, fnumber, iso, focal, thumb_offset,"
               " thumb_length, batch) VALUES (?1,?2,?3,?4,?5,?6,?7,?8,?9,?10,?11,?12,?13,?14,?15,?16,?17,?18,?19)");
  if (!s)
    return -1;
  sqlite3_bind_text (s, 1, o->path, -1, SQLITE_TRANSIENT);
  sqlite3_bind_int64 (s, 2, o->size);
  sqlite3_bind_int64 (s, 3, o->mtime);
  sqlite3_bind_int64 (s, 4, (sqlite3_int64) o->hash);
  sqlite3_bind_int (s, 5, o->filetype);
  sqlite3_bind_int (s, 6, in->format);
  sqlite3_bind_int (s, 7, in->width);
  sqlite3_bind_int (s, 8, in->height);
  sqlite3_bind_int (s, 9, in->orientation);
  bind_text_or_null (s, 10, in->taken);
  bind_text_or_null (s, 11, in->make);
  bind_text_or_null (s, 12, in->model);
  sqlite3_bind_double (s, 13, in->exposure);
  sqlite3_bind_double (s, 14, in->fnumber);
  sqlite3_bind_int (s, 15, in->iso);
  sqlite3_bind_double (s, 16, in->focal);
  sqlite3_bind_int64 (s, 17, in->thumb_offset);
  sqlite3_bind_int64 (s, 18, in->thumb_length);
  sqlite3_bind_int64 (s, 19, o->batch);
  if (sqlite3_step (s) != SQLITE_DONE)
    {
      fail (c, "add original");
      sqlite3_finalize (s);
      return -1;
    }
  sqlite3_finalize (s);
  orig = sqlite3_last_insert_rowid (c->db);

  s = prep (c, "INSERT INTO version (original, project, name, created, modified) VALUES (?1, ?2, ?3, ?4, ?4)");
  if (!s)
    return -1;
  sqlite3_bind_int64 (s, 1, orig);
  sqlite3_bind_int64 (s, 2, project);
  sqlite3_bind_text (s, 3, fsys_leaf (o->path), -1, SQLITE_TRANSIENT);
  sqlite3_bind_int64 (s, 4, (sqlite3_int64) time (NULL));
  if (sqlite3_step (s) != SQLITE_DONE)
    {
      fail (c, "add version");
      sqlite3_finalize (s);
      return -1;
    }
  sqlite3_finalize (s);
  ver = sqlite3_last_insert_rowid (c->db);
  return ver;
}

/* Adds up to max words of q to words[] (as LIKE patterns, %word%) and their
   test to match: each must be in the name, the camera, the date (as stored,
   or with - for :), the project, the caption or one of the keywords. */
static int add_words (const char *q, char words[][70], int nw, int max, char *match, size_t mn)
{
  while (*q && nw < max)
    {
      size_t len = 0;
      int p = 3 + nw;
      while (*q == ' ')
        q++;
      if (!*q)
        break;
      words[nw][0] = '%';
      while (*q && *q != ' ' && len < 64)
        {
          char ch = *q++;
          if (ch == '%' || ch == '_' || ch == '\\')
            words[nw][1 + len++] = '\\';   /* LIKE's own characters, taken literally */
          words[nw][1 + len++] = ch;
        }
      words[nw][1 + len] = '%';
      words[nw][2 + len] = '\0';
      snprintf (match + strlen (match), mn - strlen (match),
                " AND (v.name LIKE ?%d ESCAPE '\\' OR IFNULL(o.make, '') LIKE ?%d ESCAPE '\\'"
                " OR IFNULL(o.model, '') LIKE ?%d ESCAPE '\\' OR REPLACE(IFNULL(o.taken, ''), ':', '-') LIKE ?%d ESCAPE '\\'"
                " OR IFNULL(o.taken, '') LIKE ?%d ESCAPE '\\'"
                " OR (SELECT name FROM project WHERE id = v.project) LIKE ?%d ESCAPE '\\'"
                " OR v.caption LIKE ?%d ESCAPE '\\'"
                " OR CASE v.label WHEN 1 THEN 'Red' WHEN 2 THEN 'Orange' WHEN 3 THEN 'Yellow' WHEN 4 THEN 'Green'"
                " WHEN 5 THEN 'Blue' WHEN 6 THEN 'Purple' WHEN 7 THEN 'Grey' ELSE '' END LIKE ?%d ESCAPE '\\'"
                " OR EXISTS (SELECT 1 FROM version_keyword vk JOIN keyword k ON k.id = vk.keyword"
                " WHERE vk.version = v.id AND k.name LIKE ?%d ESCAPE '\\'))",
                p, p, p, p, p, p, p, p, p);
      nw++;
    }
  return nw;
}

int cat_list (cat *c, const cat_source *src, cat_item **items, int *count)
{
  char sql[16000];
  const char *where;
  sqlite3_stmt *s;
  int n = 0, cap = 256, smart = 0, min_rating = src->min_rating;
  cat_item *a;
  cat_rule rule;
  if (src->kind == SRC_ALBUM && (smart = cat_smart_get (c, src->id, &rule)) < 0)
    return -1;
  switch (src->kind)
    {
    case SRC_FLAGGED:     where = "v.flag = 1 AND v.rating >= ?2"; break;
    case SRC_REJECTED:    where = "v.rating = -1"; break;
    case SRC_LAST_IMPORT: where = "o.batch = (SELECT MAX(batch) FROM original) AND v.rating >= ?2"; break;
    case SRC_PROJECT:     where = "v.project = ?1 AND v.rating >= ?2"; break;
    case SRC_ALBUM:       where = smart ? "v.rating >= ?2"
                                        : "v.id IN (SELECT version FROM album_item WHERE album = ?1) AND v.rating >= ?2"; break;
    case SRC_MISSING:     where = "o.missing = 1 AND (v.rating >= ?2 OR v.rating = -1)"; break;
    default:              where = "v.rating >= ?2"; break;
    }
  /* Search: up to four words (?3..?6); a smart album's rule adds up to
     eight of its own (?7..?14), and its other limits (?15, ?16 for the dates). */
  char words[12][70], match[12000];
  int nw = 0, k;
  match[0] = '\0';
  nw = add_words (src->search, words, 0, 4, match, sizeof match);
  if (smart)
    {
      nw = add_words (rule.words, words, nw, nw + 8, match, sizeof match);
      if (rule.min_rating > min_rating)
        min_rating = rule.min_rating;
      if (rule.flagged)
        strcat (match, " AND v.flag = 1");
      if (rule.edited)
        strcat (match, " AND v.recipe <> ''");
      if (rule.from[0])
        strcat (match, " AND REPLACE(SUBSTR(o.taken, 1, 10), ':', '-') >= ?15");
      if (rule.to[0])
        strcat (match, " AND REPLACE(SUBSTR(o.taken, 1, 10), ':', '-') <= ?16");
    }
  /* A closed stack shows one photo where photos are browsed (not in albums,
     rejects, missing files or a search, which look for each one): its pick,
     or if the view leaves the pick out (rejected, too few stars, another
     project), the first of the rest that it shows. Done below, on the rows. */
  int collapse = (src->kind == SRC_ALL || src->kind == SRC_PROJECT || src->kind == SRC_LAST_IMPORT) && !nw;
  snprintf (sql, sizeof sql,
            "SELECT v.id, v.name, v.rating, v.flag, v.thumb, o.filetype, v.recipe <> '', o.missing,"
            " IFNULL(v.stack, 0), (SELECT COUNT(*) FROM version m WHERE m.stack = v.stack),"
            " IFNULL((SELECT s.pick = v.id FROM stack s WHERE s.id = v.stack), 0),"
            " IFNULL((SELECT s.open FROM stack s WHERE s.id = v.stack), 0), v.label"
            " FROM version v JOIN original o ON o.id = v.original WHERE %s%s"
            /* by date, then name: a stack's photos by their pick's, together, the pick first */
            " ORDER BY IFNULL((SELECT IFNULL(po.taken, '9999') FROM stack s JOIN version pv ON pv.id = s.pick"
            " JOIN original po ON po.id = pv.original WHERE s.id = v.stack), IFNULL(o.taken, '9999')),"
            " IFNULL((SELECT pv.name FROM stack s JOIN version pv ON pv.id = s.pick WHERE s.id = v.stack), v.name),"
            " IFNULL((SELECT s.pick FROM stack s WHERE s.id = v.stack), v.id),"
            " v.id <> IFNULL((SELECT s.pick FROM stack s WHERE s.id = v.stack), v.id),"
            " IFNULL(o.taken, '9999'), v.name, v.id", where, match);
  s = prep (c, sql);
  if (!s)
    return -1;
  sqlite3_bind_int64 (s, 1, src->id);
  sqlite3_bind_int (s, 2, min_rating);
  for (k = 0; k < nw; k++)
    sqlite3_bind_text (s, 3 + k, words[k], -1, SQLITE_TRANSIENT);
  if (smart && rule.from[0])
    sqlite3_bind_text (s, 15, rule.from, -1, SQLITE_TRANSIENT);
  if (smart && rule.to[0])
    sqlite3_bind_text (s, 16, rule.to, -1, SQLITE_TRANSIENT);
  a = malloc (sizeof *a * cap);
  if (!a)
    {
      sqlite3_finalize (s);
      return -1;
    }
  while (sqlite3_step (s) == SQLITE_ROW)
    {
      const unsigned char *name = sqlite3_column_text (s, 1);
      if (n == cap)
        {
          cat_item *b = realloc (a, sizeof *a * cap * 2);
          if (!b)
            break;
          a = b;
          cap *= 2;
        }
      a[n].id = sqlite3_column_int64 (s, 0);
      snprintf (a[n].name, sizeof a[n].name, "%s", name ? (const char *) name : "");
      a[n].rating = sqlite3_column_int (s, 2);
      a[n].flag = sqlite3_column_int (s, 3);
      a[n].thumb_state = sqlite3_column_int (s, 4);
      a[n].filetype = sqlite3_column_int (s, 5);
      a[n].edited = sqlite3_column_int (s, 6);
      a[n].missing = sqlite3_column_int (s, 7);
      a[n].stack = sqlite3_column_int64 (s, 8);
      a[n].stack_count = a[n].stack ? sqlite3_column_int (s, 9) : 0;
      a[n].stack_pick = sqlite3_column_int (s, 10);
      a[n].stack_open = sqlite3_column_int (s, 11);
      a[n].label = sqlite3_column_int (s, 12);
      /* A stack's rows come together, its pick first: a closed stack keeps
         the first of them shown here. */
      if (collapse && a[n].stack && !a[n].stack_open && n > 0)
        {
          int j, seen = 0;
          for (j = n - 1; j >= 0 && a[j].stack == a[n].stack; j--)
            seen = 1;
          if (seen)
            continue;
        }
      n++;
    }
  sqlite3_finalize (s);
  *items = a;
  *count = n;
  return 0;
}

int cat_next_thumb (cat *c, cat_id *version, char *path, size_t n, int *orientation,
                    char *recipe, size_t rn)
{
  sqlite3_stmt *s = prep (c, "SELECT v.id, o.path, o.orientation, v.recipe FROM version v"
                             " JOIN original o ON o.id = v.original WHERE v.thumb = 0"
                             " ORDER BY o.batch DESC, IFNULL(o.taken, '9999'), v.name LIMIT 1");
  int r = 0;
  if (!s)
    return -1;
  if (sqlite3_step (s) == SQLITE_ROW)
    {
      *version = sqlite3_column_int64 (s, 0);
      snprintf (path, n, "%s", (const char *) sqlite3_column_text (s, 1));
      *orientation = sqlite3_column_int (s, 2);
      snprintf (recipe, rn, "%s", sqlite3_column_text (s, 3) ? (const char *) sqlite3_column_text (s, 3) : "");
      r = 1;
    }
  sqlite3_finalize (s);
  return r;
}

int cat_thumb_source (cat *c, cat_id version, char *path, size_t n, int *orientation, char *recipe, size_t rn)
{
  sqlite3_stmt *s = prep (c, "SELECT o.path, o.orientation, v.recipe FROM version v"
                             " JOIN original o ON o.id = v.original WHERE v.id = ?1 AND v.thumb = 0");
  int r = 0;
  if (!s)
    return -1;
  sqlite3_bind_int64 (s, 1, version);
  if (sqlite3_step (s) == SQLITE_ROW)
    {
      snprintf (path, n, "%s", (const char *) sqlite3_column_text (s, 0));
      *orientation = sqlite3_column_int (s, 1);
      snprintf (recipe, rn, "%s", sqlite3_column_text (s, 2) ? (const char *) sqlite3_column_text (s, 2) : "");
      r = 1;
    }
  sqlite3_finalize (s);
  return r;
}

int cat_set_thumb (cat *c, cat_id version, int state)
{
  sqlite3_stmt *s = prep (c, "UPDATE version SET thumb = ?2 WHERE id = ?1");
  int r;
  if (!s)
    return -1;
  sqlite3_bind_int64 (s, 1, version);
  sqlite3_bind_int (s, 2, state);
  r = sqlite3_step (s) == SQLITE_DONE ? 0 : fail (c, "thumbnail state");
  sqlite3_finalize (s);
  return r;
}

int cat_get_recipe (cat *c, cat_id version, char *out, size_t n)
{
  sqlite3_stmt *s = prep (c, "SELECT recipe FROM version WHERE id = ?1");
  int r = -1;
  out[0] = '\0';
  if (!s)
    return -1;
  sqlite3_bind_int64 (s, 1, version);
  if (sqlite3_step (s) == SQLITE_ROW)
    {
      snprintf (out, n, "%s", sqlite3_column_text (s, 0) ? (const char *) sqlite3_column_text (s, 0) : "");
      r = 0;
    }
  sqlite3_finalize (s);
  return r;
}

int cat_set_recipe (cat *c, cat_id version, const char *recipe)
{
  sqlite3_stmt *s = prep (c, "UPDATE version SET recipe = ?2, thumb = 0, modified = ?3 WHERE id = ?1");
  int r;
  if (!s)
    return -1;
  sqlite3_bind_int64 (s, 1, version);
  sqlite3_bind_text (s, 2, recipe, -1, SQLITE_TRANSIENT);
  sqlite3_bind_int64 (s, 3, (sqlite3_int64) time (NULL));
  r = sqlite3_step (s) == SQLITE_DONE ? 0 : fail (c, "save the adjustments");
  sqlite3_finalize (s);
  return r;
}

cat_id cat_preset_save (cat *c, const char *name, const char *look)
{
  sqlite3_stmt *s = prep (c, "INSERT INTO preset (name, recipe) VALUES (?1, ?2)"
                             " ON CONFLICT (name) DO UPDATE SET recipe = excluded.recipe");
  cat_id id = -1;
  if (!s)
    return -1;
  sqlite3_bind_text (s, 1, name, -1, SQLITE_TRANSIENT);
  sqlite3_bind_text (s, 2, look, -1, SQLITE_TRANSIENT);
  if (sqlite3_step (s) != SQLITE_DONE)
    {
      fail (c, "save the preset");
      sqlite3_finalize (s);
      return -1;
    }
  sqlite3_finalize (s);
  s = prep (c, "SELECT id FROM preset WHERE name = ?1");
  if (!s)
    return -1;
  sqlite3_bind_text (s, 1, name, -1, SQLITE_TRANSIENT);
  if (sqlite3_step (s) == SQLITE_ROW)
    id = sqlite3_column_int64 (s, 0);
  sqlite3_finalize (s);
  return id;
}

int cat_presets (cat *c, cat_preset **out, int *count)
{
  sqlite3_stmt *s = prep (c, "SELECT id, name, recipe FROM preset ORDER BY name COLLATE NOCASE");
  int n = 0, cap = 16;
  cat_preset *a;
  *out = NULL;
  *count = 0;
  if (!s)
    return -1;
  a = malloc (sizeof *a * cap);
  while (a && sqlite3_step (s) == SQLITE_ROW)
    {
      if (n == cap)
        {
          cat_preset *b = realloc (a, sizeof *a * cap * 2);
          if (!b)
            break;
          a = b;
          cap *= 2;
        }
      a[n].id = sqlite3_column_int64 (s, 0);
      col_text (s, 1, a[n].name, sizeof a[n].name);
      col_text (s, 2, a[n].recipe, sizeof a[n].recipe);
      n++;
    }
  sqlite3_finalize (s);
  *out = a;
  *count = n;
  return a ? 0 : -1;
}

int cat_preset_delete (cat *c, cat_id id)
{
  sqlite3_stmt *s = prep (c, "DELETE FROM preset WHERE id = ?1");
  int r;
  if (!s)
    return -1;
  sqlite3_bind_int64 (s, 1, id);
  r = sqlite3_step (s) == SQLITE_DONE ? 0 : fail (c, "delete the preset");
  sqlite3_finalize (s);
  return r;
}

int cat_apply_look (cat *c, const cat_id *versions, int n, const char *look)
{
  recipe from, to;
  int i, rc = 0;
  recipe_parse (&from, look);
  exec (c, "SAVEPOINT look");
  for (i = 0; i < n && rc == 0; i++)
    {
      static char text[RECIPE_TEXT_MAX];
      if (cat_get_recipe (c, versions[i], text, sizeof text))
        continue;
      recipe_parse (&to, text);
      recipe_copy_look (&to, &from);
      recipe_format (&to, text, sizeof text);
      rc = cat_set_recipe (c, versions[i], text);
    }
  exec (c, rc ? "ROLLBACK TO look; RELEASE look" : "RELEASE look");
  return rc;
}

int cat_count_thumbs_pending (cat *c)
{
  sqlite3_stmt *s = prep (c, "SELECT COUNT(*) FROM version WHERE thumb = 0");
  int n = 0;
  if (!s)
    return 0;
  if (sqlite3_step (s) == SQLITE_ROW)
    n = sqlite3_column_int (s, 0);
  sqlite3_finalize (s);
  return n;
}

int cat_groups (cat *c, int kind, cat_group **out, int *count)
{
  sqlite3_stmt *s = prep (c, kind == SRC_PROJECT
      ? "SELECT p.id, p.name, (SELECT COUNT(*) FROM version v WHERE v.project = p.id AND v.rating >= 0)"
        " FROM project p ORDER BY p.name COLLATE NOCASE"
      : "SELECT a.id, a.name, (SELECT COUNT(*) FROM album_item i WHERE i.album = a.id), a.smart IS NOT NULL"
        " FROM album a WHERE a.folder IS NULL ORDER BY a.name COLLATE NOCASE, a.id");
  int n = 0, cap = 32;
  cat_group *g;
  if (!s)
    return -1;
  g = malloc (sizeof *g * cap);
  while (g && sqlite3_step (s) == SQLITE_ROW)
    {
      if (n == cap)
        {
          cat_group *h = realloc (g, sizeof *g * cap * 2);
          if (!h)
            break;
          g = h;
          cap *= 2;
        }
      g[n].id = sqlite3_column_int64 (s, 0);
      snprintf (g[n].name, sizeof g[n].name, "%s", (const char *) sqlite3_column_text (s, 1));
      g[n].count = sqlite3_column_int (s, 2);
      g[n].smart = kind == SRC_ALBUM && sqlite3_column_int (s, 3);
      n++;
    }
  sqlite3_finalize (s);
  for (int k = 0; g && k < n; k++)
    if (g[k].smart)
      {
        cat_source src = { SRC_ALBUM, g[k].id, 0, "" };
        cat_item *it = NULL;
        int m = 0;
        if (cat_list (c, &src, &it, &m) == 0)
          g[k].count = m;
        free (it);
      }
  *out = g;
  *count = n;
  return g ? 0 : -1;
}

void cat_group_name (cat *c, int kind, cat_id id, char *out, size_t n)
{
  sqlite3_stmt *s = prep (c, kind == SRC_PROJECT ? "SELECT name FROM project WHERE id = ?1"
                                                 : "SELECT name FROM album WHERE id = ?1");
  out[0] = '\0';
  if (!s)
    return;
  sqlite3_bind_int64 (s, 1, id);
  if (sqlite3_step (s) == SQLITE_ROW)
    snprintf (out, n, "%s", (const char *) sqlite3_column_text (s, 0));
  sqlite3_finalize (s);
}

static void col_text (sqlite3_stmt *s, int i, char *out, size_t n)
{
  const unsigned char *t = sqlite3_column_text (s, i);
  snprintf (out, n, "%s", t ? (const char *) t : "");
}

int cat_get_details (cat *c, cat_id version, cat_details *d)
{
  sqlite3_stmt *s = prep (c, "SELECT o.path, o.width, o.height, o.orientation, o.iso, o.taken, o.make,"
                             " o.model, o.exposure, o.fnumber, o.focal, v.project, o.missing, o.id, v.rating, p.name"
                             " FROM version v JOIN original o ON o.id = v.original"
                             " LEFT JOIN project p ON p.id = v.project WHERE v.id = ?1");
  int r = -1;
  memset (d, 0, sizeof *d);
  if (!s)
    return -1;
  sqlite3_bind_int64 (s, 1, version);
  if (sqlite3_step (s) == SQLITE_ROW)
    {
      col_text (s, 0, d->path, sizeof d->path);
      d->width = sqlite3_column_int (s, 1);
      d->height = sqlite3_column_int (s, 2);
      d->orientation = sqlite3_column_int (s, 3);
      d->iso = sqlite3_column_int (s, 4);
      col_text (s, 5, d->taken, sizeof d->taken);
      col_text (s, 6, d->make, sizeof d->make);
      col_text (s, 7, d->model, sizeof d->model);
      d->exposure = sqlite3_column_double (s, 8);
      d->fnumber = sqlite3_column_double (s, 9);
      d->focal = sqlite3_column_double (s, 10);
      d->project = sqlite3_column_int64 (s, 11);
      d->missing = sqlite3_column_int (s, 12);
      d->original = sqlite3_column_int64 (s, 13);
      d->rating = sqlite3_column_int (s, 14);
      col_text (s, 15, d->project_name, sizeof d->project_name);
      r = 0;
    }
  sqlite3_finalize (s);
  if (r == 0 && (s = prep (c, "SELECT COUNT(*), SUM(id <= ?2) FROM version WHERE original = ?1")))
    {
      sqlite3_bind_int64 (s, 1, d->original);
      sqlite3_bind_int64 (s, 2, version);
      if (sqlite3_step (s) == SQLITE_ROW)
        {
          d->versions = sqlite3_column_int (s, 0);
          d->version_no = sqlite3_column_int (s, 1);
        }
      sqlite3_finalize (s);
    }
  return r;
}

/* --- projects' folders: watching them, and albums from their layout */

static char sep_of (const char *path)
{
  const char *leaf = fsys_leaf (path);
  return leaf > path ? leaf[-1] : '.';
}

int cat_project_set_root (cat *c, cat_id project, const char *root)
{
  sqlite3_stmt *s = prep (c, "UPDATE project SET root = ?2 WHERE id = ?1 AND root IS NULL");
  if (!s)
    return -1;
  sqlite3_bind_int64 (s, 1, project);
  sqlite3_bind_text (s, 2, root, -1, SQLITE_TRANSIENT);
  sqlite3_step (s);
  sqlite3_finalize (s);
  return 0;
}

int cat_project_root (cat *c, cat_id project, char *out, size_t n)
{
  sqlite3_stmt *s = prep (c, "SELECT root FROM project WHERE id = ?1");
  out[0] = '\0';
  if (!s)
    return -1;
  sqlite3_bind_int64 (s, 1, project);
  if (sqlite3_step (s) == SQLITE_ROW && sqlite3_column_type (s, 0) != SQLITE_NULL)
    col_text (s, 0, out, n);
  sqlite3_finalize (s);
  if (out[0])
    return 0;
  /* Imported before Lumen kept it: the directory all its photos are in. */
  s = prep (c, "SELECT o.path FROM version v JOIN original o ON o.id = v.original WHERE v.project = ?1");
  if (!s)
    return -1;
  sqlite3_bind_int64 (s, 1, project);
  while (sqlite3_step (s) == SQLITE_ROW)
    {
      const char *p = (const char *) sqlite3_column_text (s, 0);
      size_t l = (size_t) (fsys_leaf (p) - p);
      if (!p || l < 2)
        continue;
      if (!out[0])
        {
          if (l - 1 < n)
            {
              memcpy (out, p, l - 1);
              out[l - 1] = '\0';
            }
        }
      else
        {
          /* The common part, back to a whole directory name. */
          size_t k = 0, ol = strlen (out), dl = l - 1;
          char sep = sep_of (p);
          while (k < ol && k < dl && out[k] == p[k])
            k++;
          if (!(k == ol && (k == dl || p[k] == sep)))
            {
              if (!(k == dl && out[k] == sep))
                while (k > 0 && out[k] != sep)
                  k--;
              out[k] = '\0';
            }
        }
    }
  sqlite3_finalize (s);
  return out[0] ? 0 : -1;
}

int cat_project_watching (cat *c, cat_id project, int *watch, int *folder_albums)
{
  sqlite3_stmt *s = prep (c, "SELECT watch, folder_albums FROM project WHERE id = ?1");
  int rc = -1;
  *watch = *folder_albums = 0;
  if (!s)
    return -1;
  sqlite3_bind_int64 (s, 1, project);
  if (sqlite3_step (s) == SQLITE_ROW)
    {
      *watch = sqlite3_column_int (s, 0);
      *folder_albums = sqlite3_column_int (s, 1);
      rc = 0;
    }
  sqlite3_finalize (s);
  return rc;
}

int cat_project_set_watching (cat *c, cat_id project, int watch, int folder_albums)
{
  char root[1024];
  sqlite3_stmt *s;
  /* Watching needs the folder: kept now, if it wasn't. */
  if ((watch || folder_albums) && cat_project_root (c, project, root, sizeof root) == 0)
    cat_project_set_root (c, project, root);
  s = prep (c, "UPDATE project SET watch = ?2, folder_albums = ?3 WHERE id = ?1");
  if (!s)
    return -1;
  sqlite3_bind_int64 (s, 1, project);
  sqlite3_bind_int (s, 2, watch != 0);
  sqlite3_bind_int (s, 3, folder_albums != 0);
  if (sqlite3_step (s) != SQLITE_DONE)
    {
      sqlite3_finalize (s);
      return fail (c, "project");
    }
  sqlite3_finalize (s);
  return folder_albums ? 0 : cat_sync_folder_albums (c, project);
}

int cat_watched (cat *c, cat_watch **out, int *count)
{
  sqlite3_stmt *s = prep (c, "SELECT id, root FROM project WHERE watch = 1 AND root IS NOT NULL ORDER BY id");
  int n = 0, cap = 8;
  cat_watch *w;
  *out = NULL;
  *count = 0;
  if (!s)
    return -1;
  w = malloc (sizeof *w * cap);
  while (w && sqlite3_step (s) == SQLITE_ROW)
    {
      if (n == cap)
        {
          cat_watch *x = realloc (w, sizeof *w * cap * 2);
          if (!x)
            break;
          w = x;
          cap *= 2;
        }
      w[n].project = sqlite3_column_int64 (s, 0);
      col_text (s, 1, w[n].root, sizeof w[n].root);
      n++;
    }
  sqlite3_finalize (s);
  *out = w;
  *count = n;
  return w ? 0 : -1;
}

int cat_known_file (cat *c, const char *path, long long size, long long mtime, cat_id *original, int *missing)
{
  sqlite3_stmt *s = prep (c, "SELECT id, missing FROM original WHERE path = ?1 AND size = ?2 AND mtime = ?3");
  int found = 0;
  if (!s)
    return 0;
  sqlite3_bind_text (s, 1, path, -1, SQLITE_TRANSIENT);
  sqlite3_bind_int64 (s, 2, size);
  sqlite3_bind_int64 (s, 3, mtime);
  if (sqlite3_step (s) == SQLITE_ROW)
    {
      found = 1;
      *original = sqlite3_column_int64 (s, 0);
      *missing = sqlite3_column_int (s, 1);
    }
  sqlite3_finalize (s);
  return found;
}

/* One folder album (project, folder): its id, made if need be. */
static cat_id folder_album (cat *c, cat_id project, const char *folder, const char *name)
{
  sqlite3_stmt *s = prep (c, "SELECT id FROM album WHERE project = ?1 AND folder = ?2");
  cat_id id = 0;
  if (!s)
    return -1;
  sqlite3_bind_int64 (s, 1, project);
  sqlite3_bind_text (s, 2, folder, -1, SQLITE_TRANSIENT);
  if (sqlite3_step (s) == SQLITE_ROW)
    id = sqlite3_column_int64 (s, 0);
  sqlite3_finalize (s);
  if (id)
    return id;
  s = prep (c, "INSERT INTO album (name, created, project, folder) VALUES (?1, ?2, ?3, ?4)");
  if (!s)
    return -1;
  sqlite3_bind_text (s, 1, name, -1, SQLITE_TRANSIENT);
  sqlite3_bind_int64 (s, 2, (sqlite3_int64) time (NULL));
  sqlite3_bind_int64 (s, 3, project);
  sqlite3_bind_text (s, 4, folder, -1, SQLITE_TRANSIENT);
  if (sqlite3_step (s) != SQLITE_DONE)
    id = -1;
  else
    id = sqlite3_last_insert_rowid (c->db);
  sqlite3_finalize (s);
  return id;
}

int cat_sync_folder_albums (cat *c, cat_id project)
{
  char root[1024];
  int watch = 0, albums = 0, rc = 0;
  size_t rl;
  sqlite3_stmt *s, *ins;
  cat_project_watching (c, project, &watch, &albums);
  exec (c, "SAVEPOINT folders");
  /* Every folder album of the project is made again from the files. */
  s = prep (c, "DELETE FROM album_item WHERE album IN (SELECT id FROM album WHERE project = ?1 AND folder IS NOT NULL)");
  if (s)
    {
      sqlite3_bind_int64 (s, 1, project);
      sqlite3_step (s);
      sqlite3_finalize (s);
    }
  if (albums && cat_project_root (c, project, root, sizeof root) == 0 && (rl = strlen (root)) > 0)
    {
      s = prep (c, "SELECT v.id, o.path FROM version v JOIN original o ON o.id = v.original WHERE v.project = ?1");
      ins = prep (c, "INSERT OR IGNORE INTO album_item (album, version) VALUES (?1, ?2)");
      if (s && ins)
        {
          sqlite3_bind_int64 (s, 1, project);
          while (sqlite3_step (s) == SQLITE_ROW)
            {
              cat_id v = sqlite3_column_int64 (s, 0);
              const char *p = (const char *) sqlite3_column_text (s, 1);
              const char *leaf;
              char sep, rel[1024];
              size_t k;
              if (!p || strncmp (p, root, rl) != 0)
                continue;
              sep = sep_of (p);
              if (p[rl] != sep)
                continue;
              leaf = fsys_leaf (p);
              if (leaf <= p + rl + 1)
                continue;                  /* in the folder itself: the project is its album */
              snprintf (rel, sizeof rel, "%.*s", (int) (leaf - 1 - (p + rl + 1)), p + rl + 1);
              /* The folder and each folder above it, up to the project's. */
              for (k = strlen (rel); k > 0; )
                {
                  char folder[1024];
                  const char *name;
                  cat_id a;
                  snprintf (folder, sizeof folder, "%.*s", (int) k, rel);
                  name = strrchr (folder, sep);
                  name = name ? name + 1 : folder;
                  a = folder_album (c, project, folder, name);
                  if (a > 0)
                    {
                      sqlite3_reset (ins);
                      sqlite3_bind_int64 (ins, 1, a);
                      sqlite3_bind_int64 (ins, 2, v);
                      sqlite3_step (ins);
                    }
                  while (k > 0 && rel[k - 1] != sep)
                    k--;
                  if (k > 0)
                    k--;               /* past the separator */
                }
            }
        }
      else
        rc = -1;
      sqlite3_finalize (s);
      sqlite3_finalize (ins);
    }
  /* Folder albums with nothing in them go. */
  s = prep (c, "DELETE FROM album WHERE project = ?1 AND folder IS NOT NULL"
               " AND id NOT IN (SELECT DISTINCT album FROM album_item)");
  if (s)
    {
      sqlite3_bind_int64 (s, 1, project);
      sqlite3_step (s);
      sqlite3_finalize (s);
    }
  exec (c, rc ? "ROLLBACK TO folders; RELEASE folders" : "RELEASE folders");
  return rc;
}

int cat_sync_all_folder_albums (cat *c)
{
  sqlite3_stmt *s = prep (c, "SELECT id FROM project WHERE folder_albums = 1");
  cat_id ids[256];
  int n = 0, k, rc = 0;
  if (!s)
    return -1;
  while (n < 256 && sqlite3_step (s) == SQLITE_ROW)
    ids[n++] = sqlite3_column_int64 (s, 0);
  sqlite3_finalize (s);
  for (k = 0; k < n; k++)
    if (cat_sync_folder_albums (c, ids[k]))
      rc = -1;
  return rc;
}

int cat_folder_albums (cat *c, cat_id project, cat_folder **out, int *count)
{
  char root[1024];
  sqlite3_stmt *s;
  if (cat_project_root (c, project, root, sizeof root))
    snprintf (root, sizeof root, "x.x");
  s = prep (c, "SELECT a.id, a.name, a.folder,"
                             " (SELECT COUNT(*) FROM album_item i JOIN version v ON v.id = i.version"
                             "   WHERE i.album = a.id AND v.rating >= 0)"
                             " FROM album a WHERE a.project = ?1 AND a.folder IS NOT NULL ORDER BY a.folder COLLATE NOCASE");
  int n = 0, cap = 16;
  cat_folder *f;
  *out = NULL;
  *count = 0;
  if (!s)
    return -1;
  sqlite3_bind_int64 (s, 1, project);
  f = malloc (sizeof *f * cap);
  while (f && sqlite3_step (s) == SQLITE_ROW)
    {
      const char *folder = (const char *) sqlite3_column_text (s, 2), *q;
      char sep;
      if (n == cap)
        {
          cat_folder *x = realloc (f, sizeof *f * cap * 2);
          if (!x)
            break;
          f = x;
          cap *= 2;
        }
      f[n].id = sqlite3_column_int64 (s, 0);
      col_text (s, 1, f[n].name, sizeof f[n].name);
      f[n].count = sqlite3_column_int (s, 3);
      f[n].depth = 1;
      /* Its depth: the folder's separators (as the filing system's paths use them). */
      sep = sep_of (root);
      for (q = folder; q && *q; q++)
        if (*q == sep)
          f[n].depth++;
      n++;
    }
  sqlite3_finalize (s);
  *out = f;
  *count = n;
  return f ? 0 : -1;
}

int cat_album_is_folder (cat *c, cat_id album)
{
  sqlite3_stmt *s = prep (c, "SELECT folder IS NOT NULL FROM album WHERE id = ?1");
  int r = 0;
  if (!s)
    return 0;
  sqlite3_bind_int64 (s, 1, album);
  if (sqlite3_step (s) == SQLITE_ROW)
    r = sqlite3_column_int (s, 0);
  sqlite3_finalize (s);
  return r;
}

/* --- captions and keywords */

int cat_set_caption (cat *c, const cat_id *v, int n, const char *caption)
{
  sqlite3_stmt *s = prep (c, "UPDATE version SET caption = ?2 WHERE id = ?1");
  int i, rc = 0;
  if (!s)
    return -1;
  for (i = 0; i < n && !rc; i++)
    {
      sqlite3_reset (s);
      sqlite3_bind_int64 (s, 1, v[i]);
      sqlite3_bind_text (s, 2, caption, -1, SQLITE_TRANSIENT);
      if (sqlite3_step (s) != SQLITE_DONE)
        rc = fail (c, "caption");
    }
  sqlite3_finalize (s);
  return rc;
}

void cat_keyword_split (const char *list, void (*fn) (void *ctx, const char *kw), void *ctx)
{
  char seen[128][64];                  /* 8 KB: a callback may split another list meanwhile */
  int nseen = 0;
  while (list && *list)
    {
      char kw[64];
      size_t len = 0;
      int k, dup = 0, cut = 0;
      while (*list == ' ' || *list == ',' || *list == ';')
        list++;
      while (*list && *list != ',' && *list != ';')
        {
          unsigned char ch = (unsigned char) *list++;
          if (len < sizeof kw - 1 && ch >= ' ' && !cut)
            kw[len++] = (char) ch;
          else if (ch >= ' ')
            cut = 1;
        }
      if (cut)
        {
          /* Too long: cut at a whole UTF-8 character, not inside one. */
          size_t k = len;
          while (k && ((unsigned char) kw[k - 1] & 0xC0) == 0x80)
            k--;
          if (k && ((unsigned char) kw[k - 1] & 0xC0) == 0xC0)
            {
              unsigned char lead = (unsigned char) kw[k - 1];
              size_t need = lead >= 0xF0 ? 4 : lead >= 0xE0 ? 3 : 2;
              if (len - (k - 1) < need)
                len = k - 1;           /* its last character incomplete: dropped */
            }
        }
      while (len && kw[len - 1] == ' ')
        len--;
      kw[len] = '\0';
      if (!len)
        continue;
      for (k = 0; k < nseen && !dup; k++)
        dup = !strcasecmp (seen[k], kw);
      if (dup)
        continue;
      if (nseen < 128)
        memcpy (seen[nseen++], kw, len + 1);
      fn (ctx, kw);
    }
}

typedef struct { cat *c; const cat_id *v; int n, rc, add; } kw_job;

static void kw_apply (void *ctx, const char *kw)
{
  kw_job *j = ctx;
  sqlite3_stmt *s;
  cat_id id = 0;
  int i;
  if (j->rc)
    return;
  if (j->add)
    {
      s = prep (j->c, "INSERT OR IGNORE INTO keyword (name) VALUES (?1)");
      if (!s)
        {
          j->rc = -1;
          return;
        }
      sqlite3_bind_text (s, 1, kw, -1, SQLITE_TRANSIENT);
      sqlite3_step (s);
      sqlite3_finalize (s);
    }
  s = prep (j->c, "SELECT id FROM keyword WHERE name = ?1");
  if (!s)
    {
      j->rc = -1;
      return;
    }
  sqlite3_bind_text (s, 1, kw, -1, SQLITE_TRANSIENT);
  if (sqlite3_step (s) == SQLITE_ROW)
    id = sqlite3_column_int64 (s, 0);
  sqlite3_finalize (s);
  if (!id)
    return;                            /* taking away one nobody has */
  s = prep (j->c, j->add ? "INSERT OR IGNORE INTO version_keyword (version, keyword) VALUES (?1, ?2)"
                         : "DELETE FROM version_keyword WHERE version = ?1 AND keyword = ?2");
  if (!s)
    {
      j->rc = -1;
      return;
    }
  for (i = 0; i < j->n && !j->rc; i++)
    {
      sqlite3_reset (s);
      sqlite3_bind_int64 (s, 1, j->v[i]);
      sqlite3_bind_int64 (s, 2, id);
      if (sqlite3_step (s) != SQLITE_DONE)
        j->rc = fail (j->c, "keywords");
    }
  sqlite3_finalize (s);
}

static int keywords_change (cat *c, const cat_id *v, int n, const char *list, int add)
{
  kw_job j = { c, v, n, 0, add };
  exec (c, "SAVEPOINT keywords");
  cat_keyword_split (list, kw_apply, &j);
  if (!j.rc && !add && exec (c, "DELETE FROM keyword WHERE id NOT IN (SELECT DISTINCT keyword FROM version_keyword)"))
    j.rc = -1;
  exec (c, j.rc ? "ROLLBACK TO keywords; RELEASE keywords" : "RELEASE keywords");
  return j.rc;
}

int cat_add_keywords (cat *c, const cat_id *v, int n, const char *list)
{
  return keywords_change (c, v, n, list, 1);
}

int cat_remove_keywords (cat *c, const cat_id *v, int n, const char *list)
{
  return keywords_change (c, v, n, list, 0);
}

/* Appends name to a list ("a, b") in out. */
static void list_add (char *out, size_t n, const char *name)
{
  size_t len = strlen (out);
  snprintf (out + len, n - len, "%s%s", len ? ", " : "", name);
}

int cat_get_tags (cat *c, cat_id version, char *caption, size_t cn, char *keywords, size_t kn)
{
  return cat_common_tags (c, &version, 1, caption, cn, NULL, keywords, kn);
}

int cat_common_tags (cat *c, const cat_id *v, int n, char *caption, size_t cn, int *same,
                     char *keywords, size_t kn)
{
  sqlite3_stmt *s, *t;
  int i, all = 1;
  if (caption && cn)
    caption[0] = '\0';
  if (keywords && kn)
    keywords[0] = '\0';
  if (same)
    *same = 0;
  if (n <= 0)
    return 0;
  s = prep (c, "SELECT caption FROM version WHERE id = ?1");
  if (!s)
    return -1;
  for (i = 0; i < n; i++)
    {
      char cap[CAT_CAPTION_MAX];
      static char first[CAT_CAPTION_MAX];  /* the first one's, whole (the caller's copy may be shorter) */
      sqlite3_reset (s);
      sqlite3_bind_int64 (s, 1, v[i]);
      cap[0] = '\0';
      if (sqlite3_step (s) == SQLITE_ROW)
        col_text (s, 0, cap, sizeof cap);
      if (i == 0)
        {
          snprintf (first, sizeof first, "%s", cap);
          if (caption)
            snprintf (caption, cn, "%s", cap);
        }
      else if (strcmp (cap, first))
        all = 0;
    }
  sqlite3_finalize (s);
  if (!all && caption)
    caption[0] = '\0';
  if (same)
    *same = all;
  if (!keywords)
    return 0;
  /* The first one's keywords that all the others have too. */
  s = prep (c, "SELECT k.id, k.name FROM version_keyword vk JOIN keyword k ON k.id = vk.keyword"
               " WHERE vk.version = ?1 ORDER BY k.name COLLATE NOCASE");
  t = prep (c, "SELECT 1 FROM version_keyword WHERE version = ?1 AND keyword = ?2");
  if (!s || !t)
    {
      sqlite3_finalize (s);
      sqlite3_finalize (t);
      return -1;
    }
  sqlite3_bind_int64 (s, 1, v[0]);
  while (sqlite3_step (s) == SQLITE_ROW)
    {
      cat_id kw = sqlite3_column_int64 (s, 0);
      char name[64];
      int every = 1;
      for (i = 1; i < n && every; i++)
        {
          sqlite3_reset (t);
          sqlite3_bind_int64 (t, 1, v[i]);
          sqlite3_bind_int64 (t, 2, kw);
          every = sqlite3_step (t) == SQLITE_ROW;
        }
      if (every)
        {
          col_text (s, 1, name, sizeof name);
          list_add (keywords, kn, name);
        }
    }
  sqlite3_finalize (s);
  sqlite3_finalize (t);
  return 0;
}

int cat_keywords (cat *c, cat_keyword **out, int *count)
{
  sqlite3_stmt *s = prep (c, "SELECT k.name, COUNT(*) FROM keyword k JOIN version_keyword vk ON vk.keyword = k.id"
                             " GROUP BY k.id ORDER BY k.name COLLATE NOCASE");
  int n = 0, cap = 32;
  cat_keyword *a;
  *out = NULL;
  *count = 0;
  if (!s)
    return -1;
  a = malloc (sizeof *a * cap);
  while (a && sqlite3_step (s) == SQLITE_ROW)
    {
      if (n == cap)
        {
          cat_keyword *b = realloc (a, sizeof *a * cap * 2);
          if (!b)
            break;
          a = b;
          cap *= 2;
        }
      col_text (s, 0, a[n].name, sizeof a[n].name);
      a[n].count = sqlite3_column_int (s, 1);
      n++;
    }
  sqlite3_finalize (s);
  *out = a;
  *count = n;
  return a ? 0 : -1;
}

/* --- smart albums */

void cat_rule_format (const cat_rule *r, char *out, size_t n)
{
  snprintf (out, n, "words=%s\nrating=%d\nflagged=%d\nedited=%d\nfrom=%s\nto=%s\n", r->words, r->min_rating,
            r->flagged, r->edited, r->from, r->to);
}

void cat_rule_parse (const char *text, cat_rule *r)
{
  memset (r, 0, sizeof *r);
  while (text && *text)
    {
      const char *eol = strchr (text, '\n'), *eq = strchr (text, '=');
      size_t len = eol ? (size_t) (eol - text) : strlen (text);
      if (eq && eq < text + len)
        {
          size_t kl = (size_t) (eq - text), vl = len - kl - 1;
          char val[64];
          if (vl >= sizeof val)
            vl = sizeof val - 1;
          memcpy (val, eq + 1, vl);
          val[vl] = '\0';
          if (kl == 5 && !strncmp (text, "words", 5))
            snprintf (r->words, sizeof r->words, "%s", val);
          else if (kl == 6 && !strncmp (text, "rating", 6))
            r->min_rating = atoi (val) < 0 ? 0 : atoi (val) > 5 ? 5 : atoi (val);
          else if (kl == 7 && !strncmp (text, "flagged", 7))
            r->flagged = atoi (val) != 0;
          else if (kl == 6 && !strncmp (text, "edited", 6))
            r->edited = atoi (val) != 0;
          else if (kl == 4 && !strncmp (text, "from", 4))
            cat_date_normal (val, 0, r->from);
          else if (kl == 2 && !strncmp (text, "to", 2))
            cat_date_normal (val, 1, r->to);
        }
      text += len + (eol ? 1 : 0);
    }
}

int cat_date_normal (const char *in, int end, char out[11])
{
  int part[3] = { 0, 0, 0 }, digits[3] = { 0, 0, 0 }, np = 0, y, m, d;
  const char *p = in;
  out[0] = '\0';
  while (*p == ' ')
    p++;
  if (!*p)
    return 0;
  while (*p && np < 3)
    {
      if (!isdigit ((unsigned char) *p))
        return -1;
      while (isdigit ((unsigned char) *p))
        {
          if (digits[np] < 4)
            part[np] = part[np] * 10 + (*p - '0');
          digits[np]++;
          p++;
        }
      np++;
      if (*p == '-' || *p == '/' || *p == '.' || *p == ':')
        p++;
      else
        break;
    }
  while (*p == ' ')
    p++;
  if (*p || !np)
    return -1;
  if (digits[0] == 4)                  /* year first: 2026, 2026-05, 2026-05-03 */
    {
      y = part[0];
      m = np > 1 ? part[1] : end ? 12 : 1;
      d = np > 2 ? part[2] : end ? 31 : 1;
    }
  else if (np == 3 && digits[2] == 4)  /* day first, as written in Britain: 3/5/2026 */
    {
      d = part[0];
      m = part[1];
      y = part[2];
    }
  else if (np == 2 && digits[1] == 4)  /* 5/2026 */
    {
      m = part[0];
      y = part[1];
      d = end ? 31 : 1;
    }
  else
    return -1;
  if (y < 1800 || y > 2999 || m < 1 || m > 12 || d < 1 || d > 31)
    return -1;
  snprintf (out, 11, "%04d-%02d-%02d", y, m, d);
  return 0;
}

cat_id cat_smart_create (cat *c, const char *name, const cat_rule *r)
{
  cat_id id = cat_album_create (c, name);
  if (id > 0 && cat_smart_set (c, id, r))
    {
      char keep[256];
      snprintf (keep, sizeof keep, "%s", c->err);
      cat_album_delete (c, id);        /* not left behind as an ordinary album */
      snprintf (c->err, sizeof c->err, "%s", keep);
      return -1;
    }
  return id;
}

int cat_smart_get (cat *c, cat_id album, cat_rule *r)
{
  sqlite3_stmt *s = prep (c, "SELECT smart FROM album WHERE id = ?1");
  int found = 0;
  memset (r, 0, sizeof *r);
  if (!s)
    return -1;
  sqlite3_bind_int64 (s, 1, album);
  if (sqlite3_step (s) == SQLITE_ROW && sqlite3_column_type (s, 0) != SQLITE_NULL)
    {
      cat_rule_parse ((const char *) sqlite3_column_text (s, 0), r);
      found = 1;
    }
  sqlite3_finalize (s);
  return found;
}

int cat_smart_set (cat *c, cat_id album, const cat_rule *r)
{
  char text[256];
  sqlite3_stmt *s = prep (c, "UPDATE album SET smart = ?2 WHERE id = ?1");
  int rc = 0;
  if (!s)
    return -1;
  cat_rule_format (r, text, sizeof text);
  sqlite3_bind_int64 (s, 1, album);
  sqlite3_bind_text (s, 2, text, -1, SQLITE_TRANSIENT);
  if (sqlite3_step (s) != SQLITE_DONE)
    rc = fail (c, "smart album");
  sqlite3_finalize (s);
  return rc;
}

/* --- versions and stacks */

int cat_count_versions (cat *c, cat_id original)
{
  sqlite3_stmt *s = prep (c, "SELECT COUNT(*) FROM version WHERE original = ?1");
  int n = 0;
  if (!s)
    return -1;
  sqlite3_bind_int64 (s, 1, original);
  if (sqlite3_step (s) == SQLITE_ROW)
    n = sqlite3_column_int (s, 0);
  sqlite3_finalize (s);
  return n;
}

/* A name for version k of a photo called name: _vk before its extension
   (catbells_v2.jpg; on RISC OS, catbells_v2). */
static void version_name (const char *name, int k, char *out, size_t n)
{
  const char *dot = strrchr (name, '.'), *slash = strrchr (name, '/');
  const char *ext = dot && dot > name ? dot : slash && slash > name ? slash : NULL;
  if (ext)
    snprintf (out, n, "%.*s_v%d%s", (int) (ext - name), name, k, ext);
  else
    snprintf (out, n, "%s_v%d", name, k);
}

static cat_id stack_of (cat *c, cat_id v)
{
  sqlite3_stmt *s = prep (c, "SELECT IFNULL(stack, 0) FROM version WHERE id = ?1");
  cat_id st = 0;
  if (!s)
    return -1;
  sqlite3_bind_int64 (s, 1, v);
  if (sqlite3_step (s) == SQLITE_ROW)
    st = sqlite3_column_int64 (s, 0);
  sqlite3_finalize (s);
  return st;
}

cat_id cat_new_version (cat *c, cat_id v, int keep_look)
{
  sqlite3_stmt *s;
  cat_id nv = -1, orig = 0, st;
  char name[64] = "", base[64], newname[80];
  int k = 1, rc = 0;
  exec (c, "SAVEPOINT version");
  s = prep (c, "SELECT original, name FROM version WHERE id = ?1");
  if (!s)
    goto fail;
  sqlite3_bind_int64 (s, 1, v);
  if (sqlite3_step (s) == SQLITE_ROW)
    {
      orig = sqlite3_column_int64 (s, 0);
      col_text (s, 1, name, sizeof name);
    }
  sqlite3_finalize (s);
  if (!orig)
    {
      snprintf (c->err, sizeof c->err, "That photo is no longer in the library.");
      goto fail;
    }
  /* The name: the original's (its file's leaf), numbered: the first
     number no version of it has. */
  s = prep (c, "SELECT o.path FROM original o WHERE o.id = ?1");
  if (!s)
    goto fail;
  sqlite3_bind_int64 (s, 1, orig);
  snprintf (base, sizeof base, "%s", name);
  if (sqlite3_step (s) == SQLITE_ROW)
    snprintf (base, sizeof base, "%s", fsys_leaf ((const char *) sqlite3_column_text (s, 0)));
  sqlite3_finalize (s);
  s = prep (c, "SELECT 1 FROM version WHERE original = ?1 AND name = ?2");
  if (!s)
    goto fail;
  for (k = 2; k < 1000; k++)
    {
      int taken;
      version_name (base, k, newname, sizeof newname);
      sqlite3_reset (s);
      sqlite3_bind_int64 (s, 1, orig);
      sqlite3_bind_text (s, 2, newname, -1, SQLITE_TRANSIENT);
      taken = sqlite3_step (s) == SQLITE_ROW;
      if (!taken)
        break;
    }
  sqlite3_finalize (s);
  if (k >= 1000)
    {
      snprintf (c->err, sizeof c->err, "This photo has too many versions already.");
      goto fail;
    }
  s = prep (c, keep_look
               ? "INSERT INTO version (original, project, name, rating, flag, recipe, thumb, created, modified, caption)"
                 " SELECT original, project, ?2, 0, 0, recipe, 0, ?3, ?3, caption FROM version WHERE id = ?1"
               : "INSERT INTO version (original, project, name, rating, flag, recipe, thumb, created, modified, caption)"
                 " SELECT original, project, ?2, 0, 0, '', 0, ?3, ?3, caption FROM version WHERE id = ?1");
  if (!s)
    goto fail;
  sqlite3_bind_int64 (s, 1, v);
  sqlite3_bind_text (s, 2, newname, -1, SQLITE_TRANSIENT);
  sqlite3_bind_int64 (s, 3, (sqlite3_int64) time (NULL));
  if (sqlite3_step (s) != SQLITE_DONE)
    {
      sqlite3_finalize (s);
      fail (c, "new version");
      goto fail;
    }
  sqlite3_finalize (s);
  nv = sqlite3_last_insert_rowid (c->db);
  /* Its keywords and album places, as v's. */
  {
    static const char *const copy[] = {
      "INSERT OR IGNORE INTO version_keyword (version, keyword) SELECT ?2, keyword FROM version_keyword WHERE version = ?1",
      "INSERT OR IGNORE INTO album_item (album, version, pos) SELECT album, ?2, pos FROM album_item WHERE version = ?1",
    };
    size_t q;
    for (q = 0; q < sizeof copy / sizeof copy[0] && !rc; q++)
      {
        s = prep (c, copy[q]);
        if (!s)
          goto fail;
        sqlite3_bind_int64 (s, 1, v);
        sqlite3_bind_int64 (s, 2, nv);
        if (sqlite3_step (s) != SQLITE_DONE)
          rc = fail (c, "new version");
        sqlite3_finalize (s);
      }
    if (rc)
      goto fail;
  }
  /* Stacked with v, the stack open so the new one shows. */
  st = stack_of (c, v);
  if (st < 0)
    goto fail;
  if (!st)
    {
      cat_id two[2];
      two[0] = v;
      two[1] = nv;
      if ((st = cat_stack (c, two, 2, v)) <= 0)
        goto fail;
    }
  else
    {
      s = prep (c, "UPDATE version SET stack = ?2 WHERE id = ?1");
      if (!s)
        goto fail;
      sqlite3_bind_int64 (s, 1, nv);
      sqlite3_bind_int64 (s, 2, st);
      sqlite3_step (s);
      sqlite3_finalize (s);
    }
  if (cat_stack_open (c, nv, 1))
    goto fail;
  exec (c, "RELEASE version");
  return nv;
fail:
  exec (c, "ROLLBACK TO version; RELEASE version");
  return -1;
}

cat_id cat_stack (cat *c, const cat_id *v, int n, cat_id pick)
{
  sqlite3_stmt *s;
  cat_id st;
  int i, found = 0, was_open = 0;
  if (n < 2)
    {
      snprintf (c->err, sizeof c->err, "A stack needs two photos or more.");
      return -1;
    }
  for (i = 0; i < n; i++)
    if (v[i] == pick)
      found = 1;
  if (!found)
    pick = v[0];
  exec (c, "SAVEPOINT stacking");
  /* Stacks they were in before: merged into the new one (open if any was). */
  s = prep (c, "SELECT IFNULL(MAX(s.open), 0) FROM stack s JOIN version v ON v.stack = s.id WHERE v.id = ?1");
  for (i = 0; s && i < n; i++)
    {
      sqlite3_reset (s);
      sqlite3_bind_int64 (s, 1, v[i]);
      if (sqlite3_step (s) == SQLITE_ROW && sqlite3_column_int (s, 0))
        was_open = 1;
    }
  sqlite3_finalize (s);
  s = prep (c, "INSERT INTO stack (pick, open) VALUES (?1, ?2)");
  if (!s)
    goto fail;
  sqlite3_bind_int64 (s, 1, pick);
  sqlite3_bind_int (s, 2, was_open);
  if (sqlite3_step (s) != SQLITE_DONE)
    {
      sqlite3_finalize (s);
      fail (c, "stack");
      goto fail;
    }
  sqlite3_finalize (s);
  st = sqlite3_last_insert_rowid (c->db);
  /* Whole stacks come along: a member's stack-mates join too. */
  s = prep (c, "UPDATE version SET stack = ?2 WHERE id = ?1"
               " OR (stack IS NOT NULL AND stack = (SELECT stack FROM version WHERE id = ?1))");
  if (!s)
    goto fail;
  for (i = 0; i < n; i++)
    {
      sqlite3_reset (s);
      sqlite3_bind_int64 (s, 1, v[i]);
      sqlite3_bind_int64 (s, 2, st);
      if (sqlite3_step (s) != SQLITE_DONE)
        {
          sqlite3_finalize (s);
          fail (c, "stack");
          goto fail;
        }
    }
  sqlite3_finalize (s);
  if (stacks_tidy (c))
    goto fail;
  if (stack_of (c, pick) != st)
    {
      /* Fewer than two different photos (the same one twice): no stack. */
      snprintf (c->err, sizeof c->err, "A stack needs two photos or more.");
      goto fail;
    }
  exec (c, "RELEASE stacking");
  return st;
fail:
  exec (c, "ROLLBACK TO stacking; RELEASE stacking");
  return -1;
}

int cat_unstack (cat *c, const cat_id *v, int n)
{
  sqlite3_stmt *s = prep (c, "UPDATE version SET stack = NULL WHERE id = ?1");
  int i, rc = 0;
  if (!s)
    return -1;
  exec (c, "SAVEPOINT unstacking");
  for (i = 0; i < n && !rc; i++)
    {
      sqlite3_reset (s);
      sqlite3_bind_int64 (s, 1, v[i]);
      if (sqlite3_step (s) != SQLITE_DONE)
        rc = fail (c, "unstack");
    }
  sqlite3_finalize (s);
  if (!rc && stacks_tidy (c))
    rc = -1;
  exec (c, rc ? "ROLLBACK TO unstacking; RELEASE unstacking" : "RELEASE unstacking");
  return rc;
}

int cat_unstack_all (cat *c, const cat_id *v, int n)
{
  sqlite3_stmt *s = prep (c, "UPDATE version SET stack = NULL WHERE stack IS NOT NULL"
                             " AND stack = (SELECT stack FROM version WHERE id = ?1)");
  int i, rc = 0;
  if (!s)
    return -1;
  exec (c, "SAVEPOINT unstacking");
  for (i = 0; i < n && !rc; i++)
    {
      sqlite3_reset (s);
      sqlite3_bind_int64 (s, 1, v[i]);
      if (sqlite3_step (s) != SQLITE_DONE)
        rc = fail (c, "unstack");
    }
  sqlite3_finalize (s);
  if (!rc && stacks_tidy (c))
    rc = -1;
  exec (c, rc ? "ROLLBACK TO unstacking; RELEASE unstacking" : "RELEASE unstacking");
  return rc;
}

int cat_stack_pick (cat *c, cat_id v)
{
  sqlite3_stmt *s = prep (c, "UPDATE stack SET pick = ?1 WHERE id = (SELECT stack FROM version WHERE id = ?1)");
  int rc = 0;
  if (!s)
    return -1;
  sqlite3_bind_int64 (s, 1, v);
  if (sqlite3_step (s) != SQLITE_DONE)
    rc = fail (c, "pick");
  sqlite3_finalize (s);
  return rc;
}

int cat_stack_open (cat *c, cat_id v, int open)
{
  sqlite3_stmt *s = prep (c, open < 0 ? "UPDATE stack SET open = 1 - open WHERE id = (SELECT stack FROM version WHERE id = ?1)"
                                      : "UPDATE stack SET open = ?2 WHERE id = (SELECT stack FROM version WHERE id = ?1)");
  int rc = 0;
  if (!s)
    return -1;
  sqlite3_bind_int64 (s, 1, v);
  if (open >= 0)
    sqlite3_bind_int (s, 2, open ? 1 : 0);
  if (sqlite3_step (s) != SQLITE_DONE)
    rc = fail (c, "open stack");
  sqlite3_finalize (s);
  return rc;
}

/* EXIF's "YYYY:MM:DD HH:MM:SS" as seconds (from an arbitrary start, for
   differences); -1 if it isn't one. */
static long long taken_seconds (const char *t)
{
  int y, mo, d, h, mi, s;
  if (!t || sscanf (t, "%d:%d:%d %d:%d:%d", &y, &mo, &d, &h, &mi, &s) != 6)
    return -1;
  /* Days from a fixed start (civil to days, as Howard Hinnant's). */
  y -= mo <= 2;
  {
    long long era = (y >= 0 ? y : y - 399) / 400;
    long long yoe = y - era * 400;
    long long doy = (153 * (mo + (mo > 2 ? -3 : 9)) + 2) / 5 + d - 1;
    long long doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
    long long days = era * 146097 + doe;
    return ((days * 24 + h) * 60 + mi) * 60 + s;
  }
}

typedef struct { cat_id id; long long t; } timed;

static int cmp_timed (const void *a, const void *b)
{
  const timed *x = a, *y = b;
  return x->t < y->t ? -1 : x->t > y->t ? 1 : x->id < y->id ? -1 : x->id > y->id;
}

int cat_auto_stack (cat *c, const cat_id *v, int n, int seconds)
{
  sqlite3_stmt *s = prep (c, "SELECT o.taken FROM version v JOIN original o ON o.id = v.original WHERE v.id = ?1");
  timed *t;
  int i, m = 0, made = 0;
  if (!s)
    return -1;
  t = malloc (sizeof *t * (size_t) (n ? n : 1));
  if (!t)
    {
      sqlite3_finalize (s);
      return -1;
    }
  for (i = 0; i < n; i++)
    {
      long long secs = -1;
      sqlite3_reset (s);
      sqlite3_bind_int64 (s, 1, v[i]);
      if (sqlite3_step (s) == SQLITE_ROW && sqlite3_column_type (s, 0) != SQLITE_NULL)
        secs = taken_seconds ((const char *) sqlite3_column_text (s, 0));
      if (secs >= 0)
        {
          t[m].id = v[i];
          t[m].t = secs;
          m++;
        }
    }
  sqlite3_finalize (s);
  qsort (t, (size_t) m, sizeof *t, cmp_timed);
  for (i = 0; i < m;)
    {
      int j = i + 1;
      while (j < m && t[j].t - t[j - 1].t <= seconds)
        j++;
      int same = 1, k;
      for (k = i + 1; k < j && same; k++)  /* already one stack: nothing to do */
        same = stack_of (c, t[k].id) == stack_of (c, t[i].id) && stack_of (c, t[i].id) > 0;
      if (j - i >= 2 && !same)
        {
          cat_id *ids = malloc (sizeof *ids * (size_t) (j - i));
          if (ids)
            {
              for (k = i; k < j; k++)
                ids[k - i] = t[k].id;
              if (cat_stack (c, ids, j - i, ids[0]) > 0)
                made++;
              free (ids);
            }
        }
      i = j;
    }
  free (t);
  return made;
}
