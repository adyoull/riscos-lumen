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
