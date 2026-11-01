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
  p->thumb_size = 1;
  p->verify = 1;
  p->watch_new = 0;
  p->albums_new = 0;
  p->watch_minutes = 5;
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
      else
        {
          long n = strtol (v, &end, 10);
          if (end == v)
            continue;
          if (!strcmp (line, "export_format"))
            p->export_format = clamp ((int) n, 0, 1);
          else if (!strcmp (line, "export_quality"))
            p->export_quality = clamp ((int) n, 1, 100);
          else if (!strcmp (line, "export_size"))
            p->export_size = n == 3840 || n == 2048 || n == 1024 ? (int) n : 0;
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
  if (fclose (f) != 0)
    return -1;
  remove (path);
  if (rename (tmp, path) != 0)
    return -1;
  fsys_settype (path, 0xFFF);
  return 0;
}
