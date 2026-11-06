/* Copyright (c) 2026 Andrew Youll
   Released under the MIT licence: see the LICENSE file.
   SPDX-License-Identifier: MIT */
/* libcheck.c: see libcheck.h. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include "sqlite3.h"
#include "fsys.h"
#include "libcheck.h"

enum { PH_DB, PH_THUMBS, PH_FILES, PH_DONE };
#define THUMB_ROWS 32

typedef struct { char *path; } spare;

struct libcheck
{
  library *l;
  int phase;
  cat_id after;                        /* thumbnails checked up to this version */
  int shard;                           /* Thumbs subdirectory being listed */
  cat_id *gone;                        /* versions whose thumbnail file has gone */
  int ngone, capgone;
  char **unused;                       /* thumbnail files of no version */
  int nunused, capunused;
  char **pv_unused;                    /* previews of no original, or out of date */
  int npv, cappv;
  libcheck_report r;
};

static sqlite3 *dbof (library *l)
{
  return cat_sqlite (lib_cat (l));
}

static long long one (library *l, const char *sql)
{
  sqlite3_stmt *s;
  long long v = 0;
  if (sqlite3_prepare_v2 (dbof (l), sql, -1, &s, NULL) != SQLITE_OK)
    return 0;
  if (sqlite3_step (s) == SQLITE_ROW)
    v = sqlite3_column_int64 (s, 0);
  sqlite3_finalize (s);
  return v;
}

/* The checks, as SQL counting each kind of problem. */
static const char Q_NO_PHOTO[] = "SELECT COUNT(*) FROM version WHERE original NOT IN (SELECT id FROM original)";
static const char Q_NO_PROJECT[] = "SELECT COUNT(*) FROM version WHERE original IN (SELECT id FROM original)"
                                   " AND project NOT IN (SELECT id FROM project)";
static const char Q_LONE[] = "SELECT COUNT(*) FROM original WHERE id NOT IN (SELECT original FROM version)";
static const char Q_ITEMS[] = "SELECT COUNT(*) FROM album_item WHERE version NOT IN (SELECT id FROM version)"
                              " OR album NOT IN (SELECT id FROM album)";
static const char Q_KW[] = "SELECT COUNT(*) FROM version_keyword WHERE version NOT IN (SELECT id FROM version)"
                           " OR keyword NOT IN (SELECT id FROM keyword)";
static const char Q_STACKS[] =
  "SELECT (SELECT COUNT(*) FROM stack s WHERE (SELECT COUNT(*) FROM version v WHERE v.stack = s.id) < 2"
  "   OR s.pick NOT IN (SELECT v.id FROM version v WHERE v.stack = s.id))"
  " + (SELECT COUNT(DISTINCT stack) FROM version WHERE stack IS NOT NULL AND stack NOT IN (SELECT id FROM stack))";

static void integrity (library *l, libcheck_report *r)
{
  sqlite3_stmt *s;
  r->damaged = 0;
  r->damage[0] = 0;
  if (sqlite3_prepare_v2 (dbof (l), "PRAGMA integrity_check(5)", -1, &s, NULL) != SQLITE_OK)
    {
      r->damaged = 1;
      snprintf (r->damage, sizeof r->damage, "%s", sqlite3_errmsg (dbof (l)));
      return;
    }
  while (sqlite3_step (s) == SQLITE_ROW)
    {
      const char *t = (const char *) sqlite3_column_text (s, 0);
      if (t && strcmp (t, "ok"))
        {
          if (!r->damaged)
            snprintf (r->damage, sizeof r->damage, "%s", t);
          r->damaged++;
        }
    }
  sqlite3_finalize (s);
}

/* Thumbnails that failed, of photos whose files are there: trying again
   may work (a plug-in installed since). The others are missing photos. */
static int failed_thumbs (library *l)
{
  sqlite3_stmt *s;
  int n = 0;
  if (sqlite3_prepare_v2 (dbof (l), "SELECT o.path FROM version v JOIN original o ON o.id = v.original"
                                    " WHERE v.thumb = 2", -1, &s, NULL) != SQLITE_OK)
    return 0;
  while (sqlite3_step (s) == SQLITE_ROW)
    {
      fsys_entry e;
      const char *p = (const char *) sqlite3_column_text (s, 0);
      if (p && fsys_stat (p, &e) == 0)
        n++;
    }
  sqlite3_finalize (s);
  return n;
}

static void count_rows (library *l, libcheck_report *r)
{
  r->versions_no_photo = (int) one (l, Q_NO_PHOTO);
  r->versions_no_project = (int) one (l, Q_NO_PROJECT);
  r->lone_originals = (int) one (l, Q_LONE);
  r->bad_album_items = (int) one (l, Q_ITEMS);
  r->bad_keyword_links = (int) one (l, Q_KW);
  r->bad_stacks = (int) one (l, Q_STACKS);
  r->thumbs_failed = failed_thumbs (l);
  r->originals_missing = (int) one (l, "SELECT COUNT(*) FROM original WHERE missing != 0");
  r->versions = (int) one (l, "SELECT COUNT(*) FROM version");
  r->originals = (int) one (l, "SELECT COUNT(*) FROM original");
}

libcheck *libcheck_start (library *l)
{
  libcheck *k = calloc (1, sizeof *k);
  if (k)
    k->l = l;
  return k;
}

static int is_number (const char *s)
{
  if (!*s)
    return 0;
  for (; *s; s++)
    if (*s < '0' || *s > '9')
      return 0;
  return 1;
}

static int is_number_prefix (const char *s)
{
  return *s >= '0' && *s <= '9';
}

static void thumbs_step (libcheck *k)
{
  sqlite3_stmt *s;
  int rows = 0;
  if (sqlite3_prepare_v2 (dbof (k->l), "SELECT id FROM version WHERE thumb = 1 AND id > ?1 ORDER BY id LIMIT ?2",
                          -1, &s, NULL) != SQLITE_OK)
    {
      k->phase = PH_FILES;
      return;
    }
  sqlite3_bind_int64 (s, 1, k->after);
  sqlite3_bind_int (s, 2, THUMB_ROWS);
  while (sqlite3_step (s) == SQLITE_ROW)
    {
      char path[1100];
      fsys_entry e;
      cat_id v = sqlite3_column_int64 (s, 0);
      rows++;
      k->after = v;
      lib_thumb_path (k->l, v, path, sizeof path);
      if (fsys_stat (path, &e) == 0)
        continue;
      if (k->ngone == k->capgone)
        {
          int cap = k->capgone ? k->capgone * 2 : 64;
          cat_id *g = realloc (k->gone, sizeof *g * (size_t) cap);
          if (!g)
            continue;
          k->gone = g;
          k->capgone = cap;
        }
      k->gone[k->ngone++] = v;
      k->r.thumbs_missing++;
    }
  sqlite3_finalize (s);
  if (rows < THUMB_ROWS)
    k->phase = PH_FILES;
}

static void keep_path (char ***list, int *n, int *cap, const char *path)
{
  if (*n == *cap)
    {
      int c = *cap ? *cap * 2 : 64;
      char **u = realloc (*list, sizeof *u * (size_t) c);
      if (!u)
        return;
      *list = u;
      *cap = c;
    }
  if (((*list)[*n] = malloc (strlen (path) + 1)) != NULL)
    strcpy ((*list)[(*n)++], path);
}

/* Is a file in Previews the up-to-date preview of an original? */
static int preview_in_use (library *l, const char *leaf)
{
  char want[40];
  size_t k;
  if (!is_number_prefix (leaf) || !lib_preview_leaf (l, strtoll (leaf, NULL, 10), want, sizeof want))
    return 0;
  k = strlen (want);
  /* the plain one, or one with the raw's own noise reduction (n1, n2) */
  return strncmp (want, leaf, k) == 0 && (leaf[k] == '\0' || (leaf[k] == 'n' && (leaf[k + 1] == '1' || leaf[k + 1] == '2') && !leaf[k + 2]));
}

/* One Previews subdirectory: files no original uses as its preview now. */
static void previews_shard (libcheck *k, int shard_no)
{
  char dir[1100], top[1100], shard[8], path[1300];
  fsys_dir *d;
  fsys_entry e;
  fsys_join (top, sizeof top, lib_root (k->l), "Previews");
  snprintf (shard, sizeof shard, "%02x", (unsigned) shard_no);
  fsys_join (dir, sizeof dir, top, shard);
  if (!(d = fsys_opendir (dir)))
    return;
  while (fsys_readdir (d, &e) == 1)
    {
      if (e.is_dir)
        continue;
      k->r.previews++;
      k->r.previews_bytes += e.size;
      if (preview_in_use (k->l, e.name))
        continue;
      fsys_join (path, sizeof path, dir, e.name);
      keep_path (&k->pv_unused, &k->npv, &k->cappv, path);
      k->r.previews_unused++;
      k->r.previews_unused_bytes += e.size;
    }
  fsys_closedir (d);
}

static void files_step (libcheck *k)
{
  char dir[1100], thumbs[1100], shard[8];
  fsys_dir *d;
  fsys_entry e;
  sqlite3_stmt *s;
  if (k->shard >= 512)
    {
      k->phase = PH_DONE;
      return;
    }
  if (k->shard >= 256)
    {
      previews_shard (k, k->shard++ - 256);
      return;
    }
  fsys_join (thumbs, sizeof thumbs, lib_root (k->l), "Thumbs");
  snprintf (shard, sizeof shard, "%02x", (unsigned) k->shard++);
  fsys_join (dir, sizeof dir, thumbs, shard);
  if (!(d = fsys_opendir (dir)))
    return;                            /* none made in this one yet */
  if (sqlite3_prepare_v2 (dbof (k->l), "SELECT 1 FROM version WHERE id = ?1", -1, &s, NULL) != SQLITE_OK)
    {
      fsys_closedir (d);
      return;
    }
  while (fsys_readdir (d, &e) == 1)
    {
      int known;
      if (e.is_dir || !is_number (e.name))
        continue;                      /* only Lumen's own: a number, the version */
      sqlite3_bind_int64 (s, 1, strtoll (e.name, NULL, 10));
      known = sqlite3_step (s) == SQLITE_ROW;
      sqlite3_reset (s);
      if (known)
        continue;
      if (k->nunused == k->capunused)
        {
          int cap = k->capunused ? k->capunused * 2 : 64;
          char **u = realloc (k->unused, sizeof *u * (size_t) cap);
          if (!u)
            continue;
          k->unused = u;
          k->capunused = cap;
        }
      {
        char path[1100];
        fsys_join (path, sizeof path, dir, e.name);
        if ((k->unused[k->nunused] = malloc (strlen (path) + 1)) != NULL)
          {
            strcpy (k->unused[k->nunused++], path);
            k->r.thumbs_unused++;
            k->r.thumbs_unused_bytes += e.size;
          }
      }
    }
  sqlite3_finalize (s);
  fsys_closedir (d);
}

int libcheck_step (libcheck *k, unsigned (*now) (void), unsigned deadline)
{
  int steps = 0;
  while (k->phase != PH_DONE && (steps++ == 0 || (int) (now () - deadline) < 0))
    {
      if (k->phase == PH_DB)
        {
          integrity (k->l, &k->r);
          count_rows (k->l, &k->r);
          k->phase = PH_THUMBS;
          break;                       /* the integrity check can be long: a step of its own */
        }
      if (k->phase == PH_THUMBS)
        thumbs_step (k);
      else if (k->phase == PH_FILES)
        files_step (k);
    }
  return k->phase == PH_DONE;
}

int libcheck_percent (libcheck *k)
{
  switch (k->phase)
    {
    case PH_DB: return 0;
    case PH_THUMBS:
      {
        long long most = one (k->l, "SELECT IFNULL(MAX(id), 0) FROM version");
        return most > 0 ? 5 + (int) (k->after * 45 / most) : 50;
      }
    case PH_FILES: return 50 + k->shard * 50 / 512;
    }
  return 100;
}

const libcheck_report *libcheck_result (libcheck *k)
{
  return &k->r;
}

int libcheck_problems (const libcheck_report *r)
{
  return r->damaged + r->versions_no_photo + r->versions_no_project + r->lone_originals + r->bad_album_items
         + r->bad_keyword_links + r->bad_stacks + r->thumbs_missing + r->thumbs_failed + r->thumbs_unused
         + r->previews_unused;
}

static void add (char *out, size_t n, int *first, int count, const char *one_, const char *many)
{
  size_t len = strlen (out);
  if (count <= 0 || len + 2 >= n)
    return;
  snprintf (out + len, n - len, "%s%d %s", *first ? "" : "; ", count, count == 1 ? one_ : many);
  *first = 0;
}

void libcheck_describe (const libcheck_report *r, char *out, size_t n)
{
  int first = 1;
  size_t len;
  out[0] = 0;
  if (r->damaged)
    {
      snprintf (out, n, "The catalogue file is damaged (%s). ", r->damage);
    }
  if (!libcheck_problems (r) || (r->damaged && libcheck_problems (r) == r->damaged))
    {
      len = strlen (out);
      if (!r->damaged)
        snprintf (out + len, n - len, "All is well: %d photo%s and %d version%s checked, with their thumbnails%s.",
                  r->originals, r->originals == 1 ? "" : "s", r->versions, r->versions == 1 ? "" : "s",
                  r->previews ? " and previews" : "");
    }
  else
    {
      len = strlen (out);
      snprintf (out + len, n - len, "Found: ");
      add (out, n, &first, r->versions_no_photo, "version whose photo has gone from the catalogue",
           "versions whose photo has gone from the catalogue");
      add (out, n, &first, r->versions_no_project, "version whose project has gone", "versions whose project has gone");
      add (out, n, &first, r->lone_originals, "photo with no version, so never shown", "photos with no version, so never shown");
      add (out, n, &first, r->bad_album_items, "album entry for a photo that has gone", "album entries for photos that have gone");
      add (out, n, &first, r->bad_keyword_links, "keyword on a photo that has gone", "keywords on photos that have gone");
      add (out, n, &first, r->bad_stacks, "stack that isn't whole", "stacks that aren't whole");
      add (out, n, &first, r->thumbs_missing, "thumbnail file missing", "thumbnail files missing");
      add (out, n, &first, r->thumbs_failed, "thumbnail that couldn't be made", "thumbnails that couldn't be made");
      if (r->thumbs_unused)
        {
          char t[80];
          snprintf (t, sizeof t, "thumbnail file%s no photo uses (%lld KB)", r->thumbs_unused == 1 ? "" : "s",
                    (r->thumbs_unused_bytes + 1023) / 1024);
          add (out, n, &first, r->thumbs_unused, t, t);
        }
      if (r->previews_unused)
        {
          char t[80];
          snprintf (t, sizeof t, "preview%s out of date or of no photo (%lld KB)", r->previews_unused == 1 ? "" : "s",
                    (r->previews_unused_bytes + 1023) / 1024);
          add (out, n, &first, r->previews_unused, t, t);
        }
      len = strlen (out);
      if (len + 2 < n)
        snprintf (out + len, n - len, ".");
    }
  if (r->originals_missing)
    {
      len = strlen (out);
      snprintf (out + len, n - len, " %d original%s can't be found where %s were (Show > Missing; repair doesn't change that).",
                r->originals_missing, r->originals_missing == 1 ? "" : "s", r->originals_missing == 1 ? "it" : "they");
    }
}

void libcheck_free (libcheck *k)
{
  int i;
  if (!k)
    return;
  for (i = 0; i < k->nunused; i++)
    free (k->unused[i]);
  free (k->unused);
  for (i = 0; i < k->npv; i++)
    free (k->pv_unused[i]);
  free (k->pv_unused);
  free (k->gone);
  free (k);
}

/* --- repair ------------------------------------------------------------- */

static int exec (library *l, const char *sql, char *err, size_t en)
{
  char *msg = NULL;
  if (sqlite3_exec (dbof (l), sql, NULL, NULL, &msg) != SQLITE_OK)
    {
      snprintf (err, en, "%s", msg ? msg : sqlite3_errmsg (dbof (l)));
      sqlite3_free (msg);
      return -1;
    }
  return 0;
}

/* The catalogue as it is now, to Catalogue-backup: written as
   Catalogue-backup-new and only then put in place, so an older backup is
   never lost to one that couldn't be made. */
static int backup (library *l, char *err, size_t en)
{
  char path[1100], tmp[1100];
  sqlite3_stmt *s;
  int rc;
  fsys_join (path, sizeof path, lib_root (l), "Catalogue-backup");
  fsys_join (tmp, sizeof tmp, lib_root (l), "Catalogue-backup-new");
  remove (tmp);
  if (sqlite3_prepare_v2 (dbof (l), "VACUUM INTO ?1", -1, &s, NULL) != SQLITE_OK)
    {
      snprintf (err, en, "%s", sqlite3_errmsg (dbof (l)));
      return -1;
    }
  sqlite3_bind_text (s, 1, tmp, -1, SQLITE_TRANSIENT);
  rc = sqlite3_step (s);
  sqlite3_finalize (s);
  if (rc != SQLITE_DONE)
    {
      snprintf (err, en, "Can't keep a copy of the catalogue: %s", sqlite3_errmsg (dbof (l)));
      remove (tmp);
      return -1;
    }
  remove (path);
  if (rename (tmp, path))
    {
      snprintf (err, en, "Can't keep a copy of the catalogue (it is left as Catalogue-backup-new)");
      return -1;
    }
  return 0;
}

/* Versions with no project, and photos with no version: into "Recovered". */
static int recover (library *l, char *err, size_t en)
{
  sqlite3 *db = dbof (l);
  sqlite3_stmt *s, *ins;
  cat_id proj;
  int need = (int) one (l, Q_NO_PROJECT) + (int) one (l, Q_LONE);
  if (!need)
    return 0;
  proj = cat_project_ensure (lib_cat (l), "Recovered");
  if (proj <= 0)
    {
      snprintf (err, en, "%s", cat_error (lib_cat (l)));
      return -1;
    }
  {
    char sql[200];
    snprintf (sql, sizeof sql, "UPDATE version SET project = %lld WHERE original IN (SELECT id FROM original)"
              " AND project NOT IN (SELECT id FROM project)", proj);
    if (exec (l, sql, err, en))
      return -1;
  }
  if (sqlite3_prepare_v2 (db, "SELECT id, path FROM original WHERE id NOT IN (SELECT original FROM version)",
                          -1, &s, NULL) != SQLITE_OK)
    return -1;
  if (sqlite3_prepare_v2 (db, "INSERT INTO version (original, project, name, created, modified)"
                              " VALUES (?1, ?2, ?3, ?4, ?4)", -1, &ins, NULL) != SQLITE_OK)
    {
      sqlite3_finalize (s);
      return -1;
    }
  while (sqlite3_step (s) == SQLITE_ROW)
    {
      const char *path = (const char *) sqlite3_column_text (s, 1);
      sqlite3_bind_int64 (ins, 1, sqlite3_column_int64 (s, 0));
      sqlite3_bind_int64 (ins, 2, proj);
      sqlite3_bind_text (ins, 3, fsys_leaf (path ? path : ""), -1, SQLITE_TRANSIENT);
      sqlite3_bind_int64 (ins, 4, (sqlite3_int64) time (NULL));
      sqlite3_step (ins);
      sqlite3_reset (ins);
    }
  sqlite3_finalize (s);
  sqlite3_finalize (ins);
  return 0;
}

int libcheck_repair (libcheck *k, char *fixed, size_t fn, char *err, size_t en)
{
  library *l = k->l;
  const libcheck_report *r = &k->r;
  int i, removed_files = 0;
  char why[300] = "";
  fixed[0] = 0;
  err[0] = 0;
  if (k->phase != PH_DONE)
    {
      snprintf (err, en, "The check hasn't finished.");
      return -1;
    }
  if (backup (l, why, sizeof why))
    {
      /* Nothing is changed without a copy to go back to. */
      snprintf (err, en, "%s. Nothing was changed.%s", why,
                r->damaged ? " Export library... (on the icon bar menu) can still save what can be read into a new library." : "");
      return -1;
    }
  if (r->damaged)
    {
      libcheck_report again;
      if (exec (l, "REINDEX", err, en))
        return -1;
      memset (&again, 0, sizeof again);
      integrity (l, &again);
      if (again.damaged)
        {
          snprintf (err, en, "The catalogue is damaged beyond what Lumen can mend (%s). %s Export the library "
                    "(Export library... on the icon bar menu) to save what can still be read into a new one.",
                    again.damage, "A copy of it was kept as Catalogue-backup.");
          return -1;
        }
    }
  if (exec (l, "BEGIN", err, en))
    return -1;
  if (recover (l, err, en)
      || exec (l,
               "DELETE FROM version WHERE original NOT IN (SELECT id FROM original);"
               "DELETE FROM album_item WHERE version NOT IN (SELECT id FROM version) OR album NOT IN (SELECT id FROM album);"
               "DELETE FROM version_keyword WHERE version NOT IN (SELECT id FROM version)"
               "  OR keyword NOT IN (SELECT id FROM keyword);"
               "UPDATE version SET stack = NULL WHERE stack IS NOT NULL AND stack NOT IN (SELECT id FROM stack);"
               "UPDATE version SET stack = NULL WHERE stack IN"
               "  (SELECT s.id FROM stack s WHERE (SELECT COUNT(*) FROM version v WHERE v.stack = s.id) < 2);"
               "DELETE FROM stack WHERE id NOT IN (SELECT stack FROM version WHERE stack IS NOT NULL);"
               "UPDATE stack SET pick = (SELECT MIN(v.id) FROM version v WHERE v.stack = stack.id)"
               "  WHERE pick NOT IN (SELECT v.id FROM version v WHERE v.stack = stack.id);"
               "UPDATE version SET thumb = 0 WHERE thumb = 2;", err, en))
    {
      sqlite3_exec (dbof (l), "ROLLBACK", NULL, NULL, NULL);
      return -1;
    }
  for (i = 0; i < k->ngone; i++)
    {
      char sql[80];
      snprintf (sql, sizeof sql, "UPDATE version SET thumb = 0 WHERE id = %lld", k->gone[i]);
      exec (l, sql, err, en);
    }
  if (exec (l, "COMMIT", err, en))
    {
      sqlite3_exec (dbof (l), "ROLLBACK", NULL, NULL, NULL);
      return -1;
    }
  err[0] = 0;
  /* Thumbnail files no version uses (checked again: one may be in use now). */
  {
    sqlite3_stmt *s;
    if (sqlite3_prepare_v2 (dbof (l), "SELECT 1 FROM version WHERE id = ?1", -1, &s, NULL) == SQLITE_OK)
      {
        for (i = 0; i < k->nunused; i++)
          {
            int used;
            sqlite3_bind_int64 (s, 1, strtoll (fsys_leaf (k->unused[i]), NULL, 10));
            used = sqlite3_step (s) == SQLITE_ROW;
            sqlite3_reset (s);
            if (!used && remove (k->unused[i]) == 0)
              removed_files++;
          }
        sqlite3_finalize (s);
      }
  }
  /* Previews no original uses (checked again likewise). */
  for (i = 0; i < k->npv; i++)
    if (!preview_in_use (l, fsys_leaf (k->pv_unused[i])) && remove (k->pv_unused[i]) == 0)
      removed_files++;
  lib_thumbs_changed (l);
  {
    int fixes = libcheck_problems (r) - r->thumbs_unused - r->previews_unused + removed_files;
    snprintf (fixed, fn, "Repaired: %d problem%s put right%s%s%s", fixes, fixes == 1 ? "" : "s",
              r->versions_no_project + r->lone_originals ? "; photos that had lost their place are in the project Recovered" : "",
              r->thumbs_missing + r->thumbs_failed ? "; thumbnails are being made again" : "",
              ". The catalogue as it was is kept as Catalogue-backup.");
  }
  k->phase = PH_DONE;
  return 0;
}
