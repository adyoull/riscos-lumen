/* fsys on RISC OS: OS_GBPB 10 to read directories, OS_File 17/8 for
   single objects, OS_FSControl 37 to canonicalise. Block layouts from the
   PRMs (OS_GBPB 10 Buffer, OS_File 23 and 17, OS_File 8). */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <kernel.h>
#include <swis.h>
#include "fsys.h"

#define BUF_WORDS 512                 /* 2 KB: several entries per call */

struct fsys_dir
{
  char path[1024];
  int offset;                         /* OS_GBPB 10's R4; -1 when done */
  int count, next;                    /* entries in buf, the next to hand out */
  int pos;                            /* byte offset of the next entry in buf */
  int buf[BUF_WORDS];
};

/* Filetype and time from load and exec addresses: typed files have &FFF in
   the top 12 bits of the load address. */
static void decode (fsys_entry *e, unsigned load, unsigned exec, unsigned len, int objtype)
{
  e->is_dir = (objtype == 2);
  e->size = len;
  if ((load & 0xFFF00000u) == 0xFFF00000u)
    {
      e->filetype = (int) ((load >> 8) & 0xFFF);
      e->mtime = ((long long) (load & 0xFF) << 32) | exec;
    }
  else
    {
      e->filetype = FT_NONE;
      e->mtime = 0;
    }
  if (objtype == 2)                   /* image files (type 3, e.g. zips in SparkFS) stay files */
    e->filetype = FT_DIR;
}

fsys_dir *fsys_opendir (const char *path)
{
  fsys_dir *d = calloc (1, sizeof *d);
  int type = 0;
  if (!d)
    return NULL;
  /* Must exist and be a directory. */
  if (_swix (OS_File, _INR (0, 1) | _OUT (0), 17, path, &type) || type != 2)
    {
      free (d);
      return NULL;
    }
  snprintf (d->path, sizeof d->path, "%s", path);
  return d;
}

int fsys_readdir (fsys_dir *d, fsys_entry *e)
{
  const unsigned char *rec;
  size_t namelen;
  while (d->next >= d->count)
    {
      int read = 0, next = 0;
      if (d->offset == -1)
        return 0;
      if (_swix (OS_GBPB, _INR (0, 6) | _OUTR (3, 4), 10, d->path, d->buf,
                 64, d->offset, (int) sizeof d->buf, 0, &read, &next))
        return -1;
      d->count = read;
      d->next = 0;
      d->pos = 0;
      d->offset = next;
    }
  {
    /* Records are word aligned (PRM: OS_GBPB 10 Buffer), so pos is a
       multiple of 4 and the record starts at buf[pos / 4]. */
    const int *w = d->buf + d->pos / 4;
    rec = (const unsigned char *) w;
    decode (e, (unsigned) w[0], (unsigned) w[1], (unsigned) w[2], w[4]);
  }
  snprintf (e->name, sizeof e->name, "%s", (const char *) rec + 20);
  namelen = strlen ((const char *) rec + 20);
  d->pos += (20 + (int) namelen + 1 + 3) & ~3;
  d->next++;
  return 1;
}

void fsys_closedir (fsys_dir *d)
{
  free (d);
}

int fsys_stat (const char *path, fsys_entry *e)
{
  int type = 0;
  unsigned load = 0, exec = 0, len = 0;
  if (_swix (OS_File, _INR (0, 1) | _OUT (0) | _OUTR (2, 4), 17, path, &type, &load, &exec, &len)
      || type == 0)
    return -1;
  decode (e, load, exec, len, type);
  snprintf (e->name, sizeof e->name, "%s", fsys_leaf (path));
  return 0;
}

void fsys_join (char *out, size_t n, const char *dir, const char *leaf)
{
  snprintf (out, n, "%s.%s", dir, leaf);
}

const char *fsys_leaf (const char *path)
{
  const char *s = strrchr (path, '.'), *c = strrchr (path, ':');
  if (c && (!s || c > s))
    s = c;
  return s ? s + 1 : path;
}

int fsys_mkdirs (const char *path)
{
  char tmp[1024];
  size_t i, l;
  int type = 0;
  if (_swix (OS_File, _INR (0, 1) | _OUT (0), 17, path, &type) == NULL && type == 2)
    return 0;
  snprintf (tmp, sizeof tmp, "%s", path);
  l = strlen (tmp);
  /* Create each level after the root ("$" or the filing system part). */
  for (i = 0; i <= l; i++)
    if (tmp[i] == '.' || tmp[i] == '\0')
      {
        char c = tmp[i];
        tmp[i] = '\0';
        if (strchr (tmp, '$') != NULL && strcmp (fsys_leaf (tmp), "$") != 0)
          {
            if (_swix (OS_File, _INR (0, 1) | _OUT (0), 17, tmp, &type) || type == 0)
              {
                if (_swix (OS_File, _INR (0, 1) | _IN (4), 8, tmp, 0))
                  return -1;
              }
          }
        tmp[i] = c;
      }
  return 0;
}

int fsys_canonical (const char *path, char *out, size_t n)
{
  int spare = 0;
  if (_swix (OS_FSControl, _INR (0, 5) | _OUT (5), 37, path, out, 0, 0, (int) n, &spare)
      || spare <= 0)
    return -1;
  return 0;
}

int fsys_settype (const char *path, int filetype)
{
  return _swix (OS_File, _INR (0, 2), 18, path, filetype) ? -1 : 0;
}
