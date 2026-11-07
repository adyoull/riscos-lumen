/* Copyright (c) 2026 Andrew Youll
   Released under the MIT licence: see the LICENSE file.
   SPDX-License-Identifier: MIT */
/* fsys on Linux, for the tests. Filetypes come from a ",xxx" suffix
   (riscos-crossdev's convention for typed files) or a known extension. */
#include <dirent.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>
#include <unistd.h>
#include <utime.h>
#include "fsys.h"
#if defined(__arm__) && defined(__linux__)
#include <sys/auxv.h>
#endif

struct fsys_dir
{
  DIR *d;
  char path[1024];
};

static int type_from_name (const char *name)
{
  const char *comma = strrchr (name, ','), *dot = strrchr (name, '.');
  if (comma && strlen (comma) == 4)
    return (int) strtol (comma + 1, NULL, 16);
  if (dot)
    {
      if (!strcasecmp (dot, ".jpg") || !strcasecmp (dot, ".jpeg"))
        return FT_JPEG;
      if (!strcasecmp (dot, ".png"))
        return FT_PNG;
      if (!strcasecmp (dot, ".tif") || !strcasecmp (dot, ".tiff"))
        return FT_TIFF;
    }
  return FT_NONE;
}

static void fill (fsys_entry *e, const char *leaf, const struct stat *st)
{
  snprintf (e->name, sizeof e->name, "%s", leaf);
  e->is_dir = S_ISDIR (st->st_mode);
  e->filetype = e->is_dir ? FT_DIR : type_from_name (leaf);
  e->size = (long long) st->st_size;
  /* RISC OS time is centiseconds since 1 Jan 1900: 2208988800 s before 1970. */
  e->mtime = ((long long) st->st_mtime + 2208988800LL) * 100;
}

fsys_dir *fsys_opendir (const char *path)
{
  fsys_dir *d = calloc (1, sizeof *d);
  if (!d)
    return NULL;
  d->d = opendir (path);
  if (!d->d)
    {
      free (d);
      return NULL;
    }
  snprintf (d->path, sizeof d->path, "%s", path);
  return d;
}

int fsys_readdir (fsys_dir *d, fsys_entry *e)
{
  struct dirent *de;
  while ((de = readdir (d->d)) != NULL)
    {
      char full[1300];
      struct stat st;
      if (de->d_name[0] == '.')
        continue;                    /* ., .. and hidden files */
      fsys_join (full, sizeof full, d->path, de->d_name);
      if (stat (full, &st) != 0)
        continue;
      fill (e, de->d_name, &st);
      return 1;
    }
  return 0;
}

void fsys_closedir (fsys_dir *d)
{
  if (d)
    {
      closedir (d->d);
      free (d);
    }
}

int fsys_stat (const char *path, fsys_entry *e)
{
  struct stat st;
  if (stat (path, &st) != 0)
    return -1;
  fill (e, fsys_leaf (path), &st);
  return 0;
}

void fsys_join (char *out, size_t n, const char *dir, const char *leaf)
{
  size_t l = strlen (dir);
  snprintf (out, n, "%s%s%s", dir, (l && dir[l - 1] == '/') ? "" : "/", leaf);
}

const char *fsys_leaf (const char *path)
{
  const char *s = strrchr (path, '/');
  return s ? s + 1 : path;
}

int fsys_mkdirs (const char *path)
{
  char tmp[1024];
  size_t i, l;
  snprintf (tmp, sizeof tmp, "%s", path);
  l = strlen (tmp);
  for (i = 1; i <= l; i++)
    if (tmp[i] == '/' || tmp[i] == '\0')
      {
        char c = tmp[i];
        tmp[i] = '\0';
        if (mkdir (tmp, 0777) != 0 && errno != EEXIST)
          return -1;
        tmp[i] = c;
      }
  return 0;
}

int fsys_canonical (const char *path, char *out, size_t n)
{
  snprintf (out, n, "%s", path);
  return 0;
}

int fsys_neon (void)
{
#if defined(__arm__) && defined(__linux__)
  return (getauxval (AT_HWCAP) & (1UL << 12)) != 0;   /* HWCAP_NEON */
#else
  return 0;
#endif
}

int fsys_touch (const char *path)
{
  return utime (path, NULL);
}

int fsys_settype (const char *path, int filetype)
{
  (void) path; (void) filetype;
  return 0;
}

/* Every entry, hidden ones too: a copy is a copy. */
int fsys_copy_tree (const char *src, const char *dst)
{
  DIR *d;
  struct dirent *de;
  struct stat st;
  int rc = 0;
  if (stat (src, &st) != 0)
    return -1;
  if (!S_ISDIR (st.st_mode))
    {
      FILE *in = fopen (src, "rb"), *out;
      char buf[8192];
      size_t got;
      if (!in)
        return -1;
      out = fopen (dst, "wb");
      if (!out)
        {
          fclose (in);
          return -1;
        }
      while ((got = fread (buf, 1, sizeof buf, in)) > 0)
        if (fwrite (buf, 1, got, out) != got)
          rc = -1;
      fclose (in);
      if (fclose (out) != 0)
        rc = -1;
      chmod (dst, st.st_mode & 0777);
      return rc;
    }
  if (mkdir (dst, 0777) != 0 && errno != EEXIST)
    return -1;
  if (!(d = opendir (src)))
    return -1;
  while (rc == 0 && (de = readdir (d)) != NULL)
    {
      char a[1300], b[1300];
      if (!strcmp (de->d_name, ".") || !strcmp (de->d_name, ".."))
        continue;
      fsys_join (a, sizeof a, src, de->d_name);
      fsys_join (b, sizeof b, dst, de->d_name);
      rc = fsys_copy_tree (a, b);
    }
  closedir (d);
  return rc;
}

int fsys_remove_tree (const char *path)
{
  DIR *d;
  struct dirent *de;
  struct stat st;
  int rc = 0;
  if (lstat (path, &st) != 0)
    return -1;
  if (!S_ISDIR (st.st_mode))
    return unlink (path) == 0 ? 0 : -1;
  if (!(d = opendir (path)))
    return -1;
  while ((de = readdir (d)) != NULL)
    {
      char a[1300];
      if (!strcmp (de->d_name, ".") || !strcmp (de->d_name, ".."))
        continue;
      fsys_join (a, sizeof a, path, de->d_name);
      if (fsys_remove_tree (a) != 0)
        rc = -1;
    }
  closedir (d);
  if (rmdir (path) != 0)
    rc = -1;
  return rc;
}

/* --- MimeMap: a small table of its own on Linux (tests add to it) */
/* named: the desktop has a name for the type (File$Type_XXX); types tests
   add have only MimeMap's. */
typedef struct { char mime[64]; int type; char name[12]; char exts[64]; int named; } mime_line;
static mime_line mimes[32] = {
  { "image/jpeg", 0xC85, "JPEG", "jpg jpeg jfif jpe", 1 },
  { "image/png", 0xB60, "PNG", "png", 1 },
  { "image/tiff", 0xFF0, "TIFF", "tif tiff", 1 },
  { "text/plain", 0xFFF, "Text", "txt", 1 },
};
static int nmimes = 4;

void fsys_mimemap_add (const char *mime, int filetype, const char *name, const char *exts)
{
  if (nmimes < (int) (sizeof mimes / sizeof mimes[0]))
    {
      snprintf (mimes[nmimes].mime, sizeof mimes[0].mime, "%s", mime);
      mimes[nmimes].type = filetype;
      snprintf (mimes[nmimes].name, sizeof mimes[0].name, "%s", name ? name : "");
      snprintf (mimes[nmimes].exts, sizeof mimes[0].exts, "%s", exts ? exts : "");
      nmimes++;
    }
}

static int has_word (const char *list, const char *w)
{
  size_t n = strlen (w);
  while (*list)
    {
      size_t k = 0;
      while (*list == ' ')
        list++;
      while (list[k] && list[k] != ' ')
        k++;
      if (k == n && !strncasecmp (list, w, n))
        return 1;
      list += k;
    }
  return 0;
}

int fsys_mime_of_type (int filetype, char *mime, size_t n)
{
  int i;
  for (i = 0; i < nmimes; i++)
    if (mimes[i].type == filetype && filetype != FT_DATA)
      {
        snprintf (mime, n, "%s", mimes[i].mime);
        return 0;
      }
  return -1;
}

int fsys_mime_of_ext (const char *ext, char *mime, size_t n)
{
  int i;
  for (i = 0; ext && *ext && i < nmimes; i++)
    if (has_word (mimes[i].exts, ext))
      {
        snprintf (mime, n, "%s", mimes[i].mime);
        return 0;
      }
  return -1;
}

int fsys_type_of_mime (const char *mime)
{
  int i;
  for (i = 0; mime && i < nmimes; i++)
    if (!strcasecmp (mimes[i].mime, mime))
      return mimes[i].type;
  return -1;
}

void fsys_mimemap_name (const char *mime, char *out, size_t n)
{
  int i;
  out[0] = '\0';
  for (i = 0; mime && i < nmimes; i++)
    if (!strcasecmp (mimes[i].mime, mime))
      {
        snprintf (out, n, "%s", mimes[i].name);
        return;
      }
}

void fsys_type_name (int filetype, char *out, size_t n)
{
  int i;
  out[0] = '\0';
  for (i = 0; i < nmimes; i++)
    if (mimes[i].type == filetype && mimes[i].named)
      {
        snprintf (out, n, "%s", mimes[i].name);
        return;
      }
}
