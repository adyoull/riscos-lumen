/* Copyright (c) 2026 Andrew Youll
   Released under the MIT licence: see the LICENSE file.
   SPDX-License-Identifier: MIT */
/* plugins.c: see plugins.h. */
#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "fsys.h"
#include "plugins.h"

static void trim (char *s)
{
  size_t l = strlen (s);
  while (l && isspace ((unsigned char) s[l - 1]))
    s[--l] = '\0';
  l = 0;
  while (s[l] && isspace ((unsigned char) s[l]))
    l++;
  if (l)
    memmove (s, s + l, strlen (s + l) + 1);
}

/* A file in dir by its RISC OS name, or with a ",xxx" type suffix (as it
   is kept off RISC OS). 0 and its path in out if it is there. */
static int find_file (const char *dir, const char *leaf, const char *suffix, char *out, size_t n)
{
  fsys_entry e;
  char typed[96];
  fsys_join (out, n, dir, leaf);
  if (fsys_stat (out, &e) == 0 && !e.is_dir)
    return 0;
  snprintf (typed, sizeof typed, "%s%s", leaf, suffix);
  fsys_join (out, n, dir, typed);
  if (fsys_stat (out, &e) == 0 && !e.is_dir)
    return 0;
  out[0] = '\0';
  return -1;
}

static void set (char *dst, size_t n, const char *v)
{
  snprintf (dst, n, "%s", v);
}

int plugins_read (const char *path, plugin_info *p)
{
  char file[1100], line[400], prog[64] = "";
  fsys_entry e;
  FILE *f;
  memset (p, 0, sizeof *p);
  if (fsys_stat (path, &e) != 0 || !e.is_dir)
    return -1;
  set (p->dir, sizeof p->dir, fsys_leaf (path));
  if (find_file (path, "Plugin", ",fff", file, sizeof file) == 0 && (f = fopen (file, "r")))
    {
      while (fgets (line, sizeof line, f))
        {
          char *colon = strchr (line, ':');
          if (!colon || line[0] == '#')
            continue;
          *colon = '\0';
          trim (line);
          trim (colon + 1);
          if (!strcmp (line, "Name"))
            set (p->name, sizeof p->name, colon + 1);
          else if (!strcmp (line, "Version"))
            set (p->version, sizeof p->version, colon + 1);
          else if (!strcmp (line, "Kind"))
            set (p->kind, sizeof p->kind, colon + 1);
          else if (!strcmp (line, "Program"))
            set (prog, sizeof prog, colon + 1);
          else if (!strcmp (line, "Package"))
            set (p->package, sizeof p->package, colon + 1);
          else if (!strcmp (line, "Description"))
            set (p->description, sizeof p->description, colon + 1);
        }
      fclose (f);
      if (!p->name[0])
        set (p->name, sizeof p->name, p->dir);
    }
  else if (find_file (path, "LumenRaw", ",ff8", file, sizeof file) == 0)
    {
      /* The first LibRaw plug-in, before plug-ins described themselves. */
      set (p->name, sizeof p->name, "Camera raw (LibRaw)");
      set (p->kind, sizeof p->kind, "raw");
      set (prog, sizeof prog, "LumenRaw");
      set (p->package, sizeof p->package, "LumenRaw");
      set (p->description, sizeof p->description, "Reads camera raw files");
    }
  else
    return -1;
  if (prog[0] && strchr (prog, '.') == NULL && strchr (prog, '/') == NULL)
    find_file (path, prog, ",ff8", p->program, sizeof p->program);
  return 0;
}

/* The Choices file: lines "off <dir>". */
int plugins_enabled (const char *state, const char *dir)
{
  char line[200];
  FILE *f = state ? fopen (state, "r") : NULL;
  int on = 1;
  if (!f)
    return 1;
  while (fgets (line, sizeof line, f))
    {
      trim (line);
      if (!strncmp (line, "off ", 4))
        {
          char *d = line + 4;
          trim (d);
          if (!strcmp (d, dir))
            on = 0;
        }
    }
  fclose (f);
  return on;
}

int plugins_set_enabled (const char *state, const char *dir, int on)
{
  char lines[PLUGINS_MAX * 2][80], line[200], tmp[1100];
  int n = 0, k;
  FILE *f = fopen (state, "r");
  if (f)
    {
      while (n < (int) (sizeof lines / sizeof lines[0]) && fgets (line, sizeof line, f))
        {
          trim (line);
          if (!strncmp (line, "off ", 4))
            {
              char *d = line + 4;
              trim (d);
              if (*d && strcmp (d, dir) != 0)
                set (lines[n++], sizeof lines[0], d);
            }
        }
      fclose (f);
    }
  if (!on && n < (int) (sizeof lines / sizeof lines[0]))
    set (lines[n++], sizeof lines[0], dir);
  /* A copy of the whole file, then renamed over the old one. */
  snprintf (tmp, sizeof tmp, "%s_t", state);
  f = fopen (tmp, "w");
  if (!f)
    {
      /* The directory it goes in may not be there yet. */
      char parent[1100];
      const char *leaf = fsys_leaf (state);
      size_t l = (size_t) (leaf - state);
      if (l > 1 && l < sizeof parent)
        {
          memcpy (parent, state, l - 1);
          parent[l - 1] = '\0';
          fsys_mkdirs (parent);
        }
      f = fopen (tmp, "w");
      if (!f)
        return -1;
    }
  fputs ("# Lumen's plug-ins that are switched off\n", f);
  for (k = 0; k < n; k++)
    fprintf (f, "off %s\n", lines[k]);
  if (fclose (f) != 0)
    return -1;
  remove (state);
  if (rename (tmp, state) != 0)
    return -1;
  fsys_settype (state, 0xFFF);
  return 0;
}

static int by_name (const void *a, const void *b)
{
  const plugin_info *x = a, *y = b;
  const char *s = x->name, *t = y->name;
  while (*s && tolower ((unsigned char) *s) == tolower ((unsigned char) *t))
    s++, t++;
  return tolower ((unsigned char) *s) - tolower ((unsigned char) *t);
}

int plugins_scan (const char *plugins_dir, const char *state, plugin_info *out, int max)
{
  fsys_dir *d = fsys_opendir (plugins_dir);
  fsys_entry e;
  int n = 0;
  if (!d)
    return 0;
  while (n < max && fsys_readdir (d, &e) == 1)
    {
      char path[1100];
      if (!e.is_dir)
        continue;
      fsys_join (path, sizeof path, plugins_dir, e.name);
      if (plugins_read (path, &out[n]) == 0)
        {
          out[n].enabled = plugins_enabled (state, out[n].dir);
          n++;
        }
    }
  fsys_closedir (d);
  qsort (out, (size_t) n, sizeof *out, by_name);
  return n;
}

const plugin_info *plugins_find (const plugin_info *list, int n, const char *kind)
{
  int k;
  for (k = 0; k < n; k++)
    if (list[k].enabled && list[k].program[0] && !strcmp (list[k].kind, kind))
      return &list[k];
  return NULL;
}

int plugins_add (const char *plugins_dir, const char *src, int replace, char *err, size_t n)
{
  plugin_info p;
  char dst[1100];
  fsys_entry e;
  size_t l = strlen (plugins_dir);
  if (plugins_read (src, &p) != 0)
    {
      snprintf (err, n, "%s isn't a Lumen plug-in (it has no Plugin file).", fsys_leaf (src));
      return -1;
    }
  if (!strncmp (src, plugins_dir, l) && (src[l] == '.' || src[l] == '/'))
    {
      snprintf (err, n, "%s is in Lumen's plug-ins already.", p.name);
      return -1;
    }
  fsys_join (dst, sizeof dst, plugins_dir, p.dir);
  if (fsys_stat (dst, &e) == 0)
    {
      if (!replace)
        return 1;
      if (fsys_remove_tree (dst) != 0)
        {
          snprintf (err, n, "Lumen couldn't replace %s (is a file in it open, or locked?).", p.name);
          return -1;
        }
    }
  if (fsys_mkdirs (plugins_dir) != 0 || fsys_copy_tree (src, dst) != 0)
    {
      snprintf (err, n, "Lumen couldn't copy %s into its Plugins directory.", p.name);
      fsys_remove_tree (dst);
      return -1;
    }
  return 0;
}

int plugins_remove (const char *plugins_dir, const char *dir, char *err, size_t n)
{
  char path[1100];
  fsys_entry e;
  if (!dir[0] || strchr (dir, '.') || strchr (dir, '/') || strchr (dir, ':'))
    {
      snprintf (err, n, "That isn't a plug-in's name.");
      return -1;
    }
  fsys_join (path, sizeof path, plugins_dir, dir);
  if (fsys_stat (path, &e) != 0)
    return 0;
  if (fsys_remove_tree (path) != 0)
    {
      snprintf (err, n, "Lumen couldn't delete %s (is it in use, or locked?).", dir);
      return -1;
    }
  return 0;
}
