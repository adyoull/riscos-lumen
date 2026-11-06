/* Copyright (c) 2026 Andrew Youll
   Released under the MIT licence: see the LICENSE file.
   SPDX-License-Identifier: MIT */
/* prefs.c: see prefs.h. */
#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "fsys.h"
#include "prefs.h"

void prefs_default (prefs *p)
{
  memset (p, 0, sizeof *p);
  p->export_format = 0;
  p->export_quality = 90;
  p->export_size = 0;
  p->export_box_w = 1920;
  p->export_box_h = 1080;
  snprintf (p->export_names, sizeof p->export_names, "{name}");
  p->thumb_size = 1;
  p->verify = 1;
  p->watch_new = 0;
  p->albums_new = 0;
  p->watch_minutes = 5;
  p->previews = 1;                     /* raw files */
  p->open_mode = -1;                   /* File types: not chosen yet (filetypes.c decides) */
  p->open_data = -1;
}

static int clamp (int v, int lo, int hi)
{
  return v < lo ? lo : v > hi ? hi : v;
}

int prefs_load (const char *path, prefs *p)
{
  char line[700];
  FILE *f;
  prefs_default (p);
  f = fopen (path, "r");
  if (!f)
    return -1;
  while (fgets (line, sizeof line, f))
    {
      char *v, *end;
      size_t l = strlen (line);
      while (l && (line[l - 1] == '\n' || line[l - 1] == '\r'))
        line[--l] = '\0';
      if (line[0] == '#' || !(v = strchr (line, ' ')))
        continue;
      *v++ = '\0';
      if (!strcmp (line, "library"))
        snprintf (p->library, sizeof p->library, "%s", v);
      else if (!strcmp (line, "open_types"))
        snprintf (p->open_types, sizeof p->open_types, "%s", v);
      else if (!strcmp (line, "export_names"))
        snprintf (p->export_names, sizeof p->export_names, "%s", v);
      else
        {
          long n = strtol (v, &end, 10);
          if (end == v)
            continue;
          if (!strcmp (line, "export_format"))
            p->export_format = clamp ((int) n, 0, 2);
          else if (!strcmp (line, "export_quality"))
            p->export_quality = clamp ((int) n, 1, 100);
          else if (!strcmp (line, "export_size"))
            p->export_size = n == 3840 || n == 2048 || n == 1024 || n == -1 ? (int) n : 0;
          else if (!strcmp (line, "export_box_w"))
            p->export_box_w = clamp ((int) n, 16, 65535);
          else if (!strcmp (line, "export_box_h"))
            p->export_box_h = clamp ((int) n, 16, 65535);
          else if (!strcmp (line, "export_colour"))
            p->export_colour = clamp ((int) n, 0, 2);
          else if (!strcmp (line, "export_sharpen"))
            p->export_sharpen = clamp ((int) n, 0, 3);
          else if (!strcmp (line, "thumb_size"))
            p->thumb_size = clamp ((int) n, 0, 2);
          else if (!strcmp (line, "verify"))
            p->verify = n != 0;
          else if (!strcmp (line, "watch_new"))
            p->watch_new = n != 0;
          else if (!strcmp (line, "albums_new"))
            p->albums_new = n != 0;
          else if (!strcmp (line, "watch_minutes"))
            p->watch_minutes = clamp ((int) n, 0, 1440);
          else if (!strcmp (line, "previews"))
            p->previews = clamp ((int) n, 0, 2);
          else if (!strcmp (line, "raw_noise"))
            p->raw_noise = clamp ((int) n, 0, 2);
          else if (!strcmp (line, "open_mode"))
            p->open_mode = clamp ((int) n, 0, 2);
          else if (!strcmp (line, "open_data"))
            p->open_data = n != 0;
        }
    }
  fclose (f);
  return 0;
}

int prefs_save (const char *path, const prefs *p)
{
  char tmp[1100];
  FILE *f;
  snprintf (tmp, sizeof tmp, "%s_t", path);
  f = fopen (tmp, "w");
  if (!f)
    {
      char parent[1100];
      const char *leaf = fsys_leaf (path);
      size_t l = (size_t) (leaf - path);
      if (l > 1 && l < sizeof parent)
        {
          memcpy (parent, path, l - 1);
          parent[l - 1] = '\0';
          fsys_mkdirs (parent);
        }
      if (!(f = fopen (tmp, "w")))
        return -1;
    }
  fprintf (f, "# Lumen's Choices\n");
  if (p->library[0])
    fprintf (f, "library %s\n", p->library);
  fprintf (f, "export_format %d\nexport_quality %d\nexport_size %d\nthumb_size %d\nverify %d\n",
           p->export_format, p->export_quality, p->export_size, p->thumb_size, p->verify);
  fprintf (f, "watch_new %d\nalbums_new %d\nwatch_minutes %d\n", p->watch_new, p->albums_new, p->watch_minutes);
  fprintf (f, "previews %d\n", p->previews);
  fprintf (f, "raw_noise %d\n", p->raw_noise);
  fprintf (f, "export_box_w %d\nexport_box_h %d\nexport_colour %d\nexport_sharpen %d\n", p->export_box_w,
           p->export_box_h, p->export_colour, p->export_sharpen);
  if (p->export_names[0])
    fprintf (f, "export_names %s\n", p->export_names);
  if (p->open_mode >= 0)
    fprintf (f, "open_mode %d\nopen_data %d\nopen_types %s\n", p->open_mode, p->open_data > 0,
             p->open_types[0] ? p->open_types : "-");
  if (fclose (f) != 0)
    return -1;
  remove (path);
  if (rename (tmp, path) != 0)
    return -1;
  fsys_settype (path, 0xFFF);
  return 0;
}
