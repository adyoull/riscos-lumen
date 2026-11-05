/* Copyright (c) 2026 Andrew Youll
   Released under the MIT licence: see the LICENSE file.
   SPDX-License-Identifier: MIT */
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

int fsys_neon (void)
{
  _kernel_swi_regs r;
  r.r[0] = 0;
  return _kernel_swi (0x58EC8, &r, &r) == NULL && (r.r[1] & 0xF) == 2 && ((r.r[2] >> 8) & 0xF) != 0;
}

/* --- MimeMap. MimeMap_Translate: R0 the input's form, R1 it (a number or
   a string), R2 the output's form, R3 a buffer for a string (128 bytes or
   more); out, R3 the filetype for form 0. Forms: 0 filetype, 1 its name,
   2 MIME type, 3 extension (no dot). */
#define MimeMap_Translate 0x50B00

static int strncasecmp_ (const char *a, const char *b, size_t n)
{
  for (; n; n--, a++, b++)
    {
      int x = *a >= 'A' && *a <= 'Z' ? *a + 32 : *a, y = *b >= 'A' && *b <= 'Z' ? *b + 32 : *b;
      if (x != y)
        return x - y;
      if (!x)
        return 0;
    }
  return 0;
}

static void lower (char *s)
{
  for (; *s; s++)
    if (*s >= 'A' && *s <= 'Z')
      *s = (char) (*s + 32);
}

static int mime_out (int form, int in, char *mime, size_t n)
{
  char buf[256];
  _kernel_swi_regs r;
  r.r[0] = form;
  r.r[1] = in;
  r.r[2] = 2;
  r.r[3] = (int) buf;
  buf[0] = '\0';
  if (_kernel_swi (MimeMap_Translate | 0x20000, &r, &r) != NULL)
    return -1;
  buf[sizeof buf - 1] = '\0';
  lower (buf);
  /* (MimeMap's answers for "nothing known": a RISC OS type it has no line
     for comes back as application/riscos, seen on RISC OS 5.31) */
  if (!buf[0] || !strcmp (buf, "application/octet-stream") || !strcmp (buf, "application/riscos"))
    return -1;
  snprintf (mime, n, "%s", buf);
  return 0;
}

int fsys_mime_of_type (int filetype, char *mime, size_t n)
{
  if (filetype < 0 || filetype > 0xFFF || filetype == FT_DATA)
    return -1;
  return mime_out (0, filetype, mime, n);
}

int fsys_mime_of_ext (const char *ext, char *mime, size_t n)
{
  if (!ext || !*ext || strlen (ext) > 16)
    return -1;
  return mime_out (3, (int) ext, mime, n);
}

int fsys_type_of_mime (const char *mime)
{
  char buf[256];
  _kernel_swi_regs r;
  if (!mime || !*mime)
    return -1;
  r.r[0] = 2;
  r.r[1] = (int) mime;
  r.r[2] = 0;
  r.r[3] = (int) buf;
  if (_kernel_swi (MimeMap_Translate | 0x20000, &r, &r) != NULL)
    return -1;
  return r.r[3] >= 0 && r.r[3] < FT_DATA ? r.r[3] : -1;
}

/* MimeMap_Translate's names are the desktop's (File$Type_XXX: "&A3A" for
   one it has no name for), not the MimeMap file's own, so the name is read
   from the file, InetDBase:MimeMap: lines of a MIME type, a name, a
   filetype and extensions, separated by spaces or tabs; # starts a
   comment. */
void fsys_mimemap_name (const char *mime, char *out, size_t n)
{
  FILE *f;
  char line[300];
  size_t ml;
  out[0] = '\0';
  if (!mime || !*mime || !(f = fopen ("InetDBase:MimeMap", "r")))
    return;
  ml = strlen (mime);
  while (fgets (line, sizeof line, f))
    {
      char *p = line, *name;
      size_t k;
      while (*p == ' ' || *p == '\t')
        p++;
      if (*p == '#' || strncasecmp_ (p, mime, ml) || (p[ml] != ' ' && p[ml] != '\t'))
        continue;
      p += ml;
      while (*p == ' ' || *p == '\t')
        p++;
      name = p;
      for (k = 0; name[k] && name[k] != ' ' && name[k] != '\t' && name[k] != '\n' && name[k] != '\r'; k++)
        ;
      if (k && k < n && name[0] != '*')
        {
          memcpy (out, name, k);
          out[k] = '\0';
        }
      break;
    }
  fclose (f);
}

void fsys_type_name (int filetype, char *out, size_t n)
{
  _kernel_swi_regs r;
  char nm[9];
  int k = 8;
  out[0] = '\0';
  if (filetype < 0 || filetype > 0xFFF)
    return;
  r.r[0] = 18;                         /* OS_FSControl 18: a filetype's name */
  r.r[2] = filetype;
  if (_kernel_swi (OS_FSControl | 0x20000, &r, &r) != NULL || (!r.r[2] && !r.r[3]))
    return;
  memcpy (nm, &r.r[2], 4);
  memcpy (nm + 4, &r.r[3], 4);
  while (k > 0 && (nm[k - 1] == ' ' || nm[k - 1] == 0))
    k--;
  nm[k] = '\0';
  if (nm[0] != '&')                    /* "&A3A": a number, not a name */
    snprintf (out, n, "%s", nm);
}

void fsys_mimemap_add (const char *mime, int filetype, const char *name, const char *exts)
{
  (void) mime; (void) filetype; (void) name; (void) exts;
}

int fsys_settype (const char *path, int filetype)
{
  return _swix (OS_File, _INR (0, 2), 18, path, filetype) ? -1 : 0;
}

/* OS_FSControl 26 (copy) and 27 (wipe), PRM p359-360. Their flags are
   from the classic PRM (2-80, 2-82; the 2026 edition doesn't list them):
   bit 0 recurse, bit 1 force (overwrite or delete locked objects), bit 2
   look at the date range in R4-R7, bit 3 confirm, bit 4 verbose, bit 13
   use the buffer described at R8. Recurse and force, nothing else: no
   questions, no output, no date range. */
#define FSC_RECURSE 1
#define FSC_FORCE 2

int fsys_copy_tree (const char *src, const char *dst)
{
  return _swix (OS_FSControl, _INR (0, 8), 26, src, dst, FSC_RECURSE | FSC_FORCE, 0, 0, 0, 0, 0) ? -1 : 0;
}

int fsys_remove_tree (const char *path)
{
  return _swix (OS_FSControl, _INR (0, 7), 27, path, 0, FSC_RECURSE | FSC_FORCE, 0, 0, 0, 0) ? -1 : 0;
}
