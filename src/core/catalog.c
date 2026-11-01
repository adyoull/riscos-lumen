/* catalog.c: see catalog.h. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
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
      || exec (c, "INSERT OR IGNORE INTO meta VALUES ('schema', 1);"))
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

int cat_set_flag (cat *c, const cat_id *v, int n, int flag)
{
  if (flag < 0)
    return for_each_version (c,
      "UPDATE version SET flag = 1 - flag, modified = ?1 WHERE id = ?2 AND ?3 = ?3", (cat_id) time (NULL), v, n, 0);
  return for_each_version (c,
    "UPDATE version SET flag = ?3, modified = ?1 WHERE id = ?2", (cat_id) time (NULL), v, n, flag ? 1 : 0);
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

int cat_list (cat *c, const cat_source *src, cat_item **items, int *count)
{
  char sql[640];
  const char *where;
  sqlite3_stmt *s;
  int n = 0, cap = 256;
  cat_item *a;
  switch (src->kind)
    {
    case SRC_FLAGGED:     where = "v.flag = 1 AND v.rating >= ?2"; break;
    case SRC_REJECTED:    where = "v.rating = -1"; break;
    case SRC_LAST_IMPORT: where = "o.batch = (SELECT MAX(batch) FROM original) AND v.rating >= ?2"; break;
    case SRC_PROJECT:     where = "v.project = ?1 AND v.rating >= ?2"; break;
    case SRC_ALBUM:       where = "v.id IN (SELECT version FROM album_item WHERE album = ?1) AND v.rating >= ?2"; break;
    default:              where = "v.rating >= ?2"; break;
    }
  snprintf (sql, sizeof sql,
            "SELECT v.id, v.name, v.rating, v.flag, v.thumb, o.filetype, v.recipe <> '' FROM version v"
            " JOIN original o ON o.id = v.original WHERE %s"
            " ORDER BY IFNULL(o.taken, '9999'), v.name, v.id", where);
  s = prep (c, sql);
  if (!s)
    return -1;
  sqlite3_bind_int64 (s, 1, src->id);
  sqlite3_bind_int (s, 2, src->min_rating);
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
      char text[1024];
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
      : "SELECT a.id, a.name, (SELECT COUNT(*) FROM album_item i WHERE i.album = a.id)"
        " FROM album a ORDER BY a.name COLLATE NOCASE, a.id");
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
      n++;
    }
  sqlite3_finalize (s);
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
                             " o.model, o.exposure, o.fnumber, o.focal, v.project FROM version v"
                             " JOIN original o ON o.id = v.original WHERE v.id = ?1");
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
      r = 0;
    }
  sqlite3_finalize (s);
  return r;
}
