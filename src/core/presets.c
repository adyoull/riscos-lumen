/* Copyright (c) 2026 Andrew Youll
   Released under the MIT licence: see the LICENSE file.
   SPDX-License-Identifier: MIT */
/* presets.c: see presets.h. */
#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include "presets.h"
#include "fsys.h"

static char dir[1024];

void presets_set_dir (const char *d)
{
  snprintf (dir, sizeof dir, "%s", d ? d : "");
}

const char *presets_dir (void)
{
  return dir;
}

/* A leaf for a preset's name: what the filing system doesn't allow
   becomes _, and spaces hard spaces on RISC OS (as the Filer shows them). */
void presets_leaf (const char *name, char *out, size_t n)
{
  size_t k = 0;
  for (; *name && k + 1 < n && k < 60; name++)
    {
      unsigned char c = (unsigned char) *name;
      if (c < 32 || strchr (".:*#$&@^%\\|\"<>/,", c))
        c = '_';
#if defined(__riscos__) || defined(__riscos)
      else if (c == ' ')
        c = 0xA0;
#endif
      out[k++] = (char) c;
    }
  out[k] = '\0';
  if (!out[0])
    snprintf (out, n, "Preset");
}

int presets_write_file (const char *path, const char *name, const char *look)
{
  FILE *f = fopen (path, "w");
  int ok;
  if (!f)
    return -1;
  fprintf (f, "%s\nName: %s\n%s", PRESET_FIRST_LINE, name, look);
  if (look[0] && look[strlen (look) - 1] != '\n')
    fputc ('\n', f);
  ok = !ferror (f);
  if (fclose (f) || !ok)
    return -1;
  fsys_settype (path, FT_TEXT);
  return 0;
}

int presets_read_file (const char *path, char *name, size_t namelen, char *look, size_t looklen)
{
  FILE *f = fopen (path, "r");
  char line[512];
  size_t used = 0;
  int ok = 0;
  if (!f)
    return -1;
  name[0] = look[0] = '\0';
  if (fgets (line, sizeof line, f) && !strncmp (line, PRESET_FIRST_LINE, strlen (PRESET_FIRST_LINE))
      && fgets (line, sizeof line, f) && !strncmp (line, "Name:", 5))
    {
      char *v = line + 5;
      size_t k;
      while (*v == ' ' || *v == '\t')
        v++;
      k = strcspn (v, "\r\n");
      if (k >= namelen)
        k = namelen - 1;
      memcpy (name, v, k);
      name[k] = '\0';
      while (k > 0 && name[k - 1] == ' ')
        name[--k] = '\0';
      ok = name[0] != '\0';
      while (ok && fgets (line, sizeof line, f))
        {
          size_t l = strcspn (line, "\r\n");
          if (used + l + 2 > looklen)
            break;
          memcpy (look + used, line, l);
          used += l;
          look[used++] = '\n';
          look[used] = '\0';
        }
    }
  fclose (f);
  return ok ? 0 : -1;
}

/* The file in the directory holding the preset called name: 0 and its
   path, or -1. */
static int find (const char *name, char *path, size_t n)
{
  fsys_dir *d = dir[0] ? fsys_opendir (dir) : NULL;
  fsys_entry e;
  int found = -1;
  if (!d)
    return -1;
  while (found && fsys_readdir (d, &e) == 1)
    {
      char p[1300], nm[48], look[16];
      if (e.is_dir)
        continue;
      fsys_join (p, sizeof p, dir, e.name);
      /* (a look longer than look[] is cut short: only the name matters) */
      if (presets_read_file (p, nm, sizeof nm, look, sizeof look) == 0 && !strcasecmp (nm, name))
        {
          snprintf (path, n, "%s", p);
          found = 0;
        }
    }
  fsys_closedir (d);
  return found;
}

static int by_name (const void *a, const void *b)
{
  return strcasecmp (((const cat_preset *) a)->name, ((const cat_preset *) b)->name);
}

int presets_list (cat_preset **out, int *count)
{
  fsys_dir *d;
  fsys_entry e;
  cat_preset *a = NULL;
  int n = 0, cap = 0, k;
  *out = NULL;
  *count = 0;
  if (!dir[0] || !(d = fsys_opendir (dir)))
    return 0;                          /* none yet */
  while (fsys_readdir (d, &e) == 1)
    {
      char p[1300];
      cat_preset one;
      if (e.is_dir)
        continue;
      fsys_join (p, sizeof p, dir, e.name);
      memset (&one, 0, sizeof one);
      if (presets_read_file (p, one.name, sizeof one.name, one.recipe, sizeof one.recipe))
        continue;                      /* not a preset file */
      for (k = 0; k < n && strcasecmp (a[k].name, one.name); k++)
        ;
      if (k < n)
        continue;                      /* two files of one name: the first */
      if (n == cap)
        {
          cat_preset *b = realloc (a, sizeof *a * (size_t) (cap ? cap * 2 : 16));
          if (!b)
            break;
          a = b;
          cap = cap ? cap * 2 : 16;
        }
      a[n++] = one;
    }
  fsys_closedir (d);
  if (n)
    qsort (a, (size_t) n, sizeof *a, by_name);
  for (k = 0; k < n; k++)
    a[k].id = k + 1;
  *out = a;
  *count = n;
  return 0;
}

int presets_save (const char *name, const char *look)
{
  char path[1300], leaf[80];
  if (!dir[0] || !name || !name[0])
    return -1;
  fsys_mkdirs (dir);
  if (find (name, path, sizeof path))
    {
      /* A new file: its leaf from the name, not one in use already. */
      fsys_entry e;
      int k;
      presets_leaf (name, leaf, sizeof leaf);
      fsys_join (path, sizeof path, dir, leaf);
      for (k = 2; fsys_stat (path, &e) == 0 && k < 100; k++)
        {
          char l2[96];
          snprintf (l2, sizeof l2, "%.60s%d", leaf, k);
          fsys_join (path, sizeof path, dir, l2);
        }
    }
  return presets_write_file (path, name, look);
}

int presets_delete (const char *name)
{
  char path[1300];
  if (find (name, path, sizeof path))
    return -1;
  return remove (path) == 0 ? 0 : -1;
}

int presets_add_file (const char *path)
{
  char name[48], look[2048];
  if (presets_read_file (path, name, sizeof name, look, sizeof look))
    return -1;
  return presets_save (name, look);
}

int presets_take_from (cat *c)
{
  cat_preset *ps;
  int n = 0, k, added = 0;
  if (!c || !dir[0] || cat_presets (c, &ps, &n) || n == 0)
    return 0;
  for (k = 0; k < n; k++)
    {
      char path[1300];
      if (find (ps[k].name, path, sizeof path) && presets_save (ps[k].name, ps[k].recipe) == 0)
        added++;
      /* Only once it's safely in a file (or one of its name is there). */
      if (find (ps[k].name, path, sizeof path) == 0)
        cat_preset_delete (c, ps[k].id);
    }
  free (ps);
  return added;
}
