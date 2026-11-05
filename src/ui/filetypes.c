/* Copyright (c) 2026 Andrew Youll
   Released under the MIT licence: see the LICENSE file.
   SPDX-License-Identifier: MIT */
/* filetypes.c: which double-clicks in the Filer open in Lumen, as Iris
   and Reel do it (File types, on the icon bar menu):

     Never           Lumen claims no double-click;
     While running   it claims a ticked type's double-click while it's
                     running (Message_DataOpen, PRM p1007), and sets no
                     Run aliases;
     Always          also when it isn't: a ticked type's
                     Alias$@RunType_XXX starts Lumen with the photo, set
                     now and at every start-up by Choices:Lumen.Types, an
                     Obey file !Boot runs.

   The types: JPEG, PNG and TIFF, and every type MimeMap gives a MIME type
   an installed format plug-in reads (its MIME lines: plugins.h). And Data
   files named like a photo (while Lumen runs). A ticked type that the
   desktop has no name or icon for gets them from Lumen: File$Type_XXX
   from MimeMap's name for it, and Lumen's file icon (FileIcons, made by
   tools/mkfileicon.py) as file_xxx and small_xxx, kept in
   Choices:Lumen.Sprites. Another program's settings are never changed:
   only aliases that name Lumen are taken away, and only names and icons
   nobody else gave are added. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <kernel.h>
#include <swis.h>
#include "ui.h"
#include "fsys.h"
#include "plugins.h"
#include "prefs.h"
#include "rawjob.h"
#include "exif.h"

#define TYPES_MAX 24

typedef struct
{
  int type;                            /* RISC OS filetype */
  char label[64];                      /* for the menu: "JPEG", "HEIF photo (HEIC)" */
  char name[16];                       /* MimeMap's name for it, for File$Type_XXX */
  int builtin;                         /* JPEG, PNG, TIFF */
  int unnamed;                         /* the desktop had no name for it (File$Type_XXX): MimeMap's is given */
  int on;                              /* ticked */
} ftype;

static ftype types[TYPES_MAX];
static int ntypes, mode = OPEN_RUNNING, data_on = 1, set_up;

/* --- system variables */

static int var_read (const char *name, char *val, int n)
{
  int len = 0;
  if (_swix (OS_ReadVarVal, _INR (0, 4) | _OUT (2), name, val, n - 1, 0, 0, &len) || len < 0 || len >= n)
    return -1;
  val[len] = '\0';
  return 0;
}

static void var_set (const char *name, const char *val)   /* NULL: take it away */
{
  _swix (OS_SetVarVal, _INR (0, 4), name, val ? val : "", val ? (int) strlen (val) : -1, 0, 4);
}

static const char *lumen_dir (void)
{
  static char dir[600];
  const char *v = getenv ("Lumen$Dir");
  snprintf (dir, sizeof dir, "%s", v ? v : "");
  return dir;
}

/* Does an alias's value name Lumen's directory (and not one that only
   starts the same)? */
static int names_lumen (const char *val)
{
  const char *dir = lumen_dir (), *p;
  size_t n = strlen (dir);
  if (!n)
    return 0;
  for (p = strstr (val, dir); p; p = strstr (p + 1, dir))
    if (!p[n] || p[n] == ' ' || p[n] == '.')
      return 1;
  return 0;
}

static int alias_ours (int type)
{
  char name[32], val[700];
  snprintf (name, sizeof name, "Alias$@RunType_%03X", type);
  return var_read (name, val, sizeof val) == 0 && names_lumen (val);
}

/* --- the list */

static ftype *find (int type)
{
  int i;
  for (i = 0; i < ntypes; i++)
    if (types[i].type == type)
      return &types[i];
  return NULL;
}

static ftype *add (int type, const char *label, int builtin, const char *mime)
{
  ftype *t;
  if (type < 0 || type >= FT_DATA || type == FT_DOS || ntypes >= TYPES_MAX)
    return NULL;
  if ((t = find (type)))
    return t;
  t = &types[ntypes++];
  memset (t, 0, sizeof *t);
  t->type = type;
  t->builtin = builtin;
  fsys_type_name (type, t->name, sizeof t->name);
  if (!t->name[0])
    {
      t->unnamed = 1;
      fsys_mimemap_name (mime, t->name, sizeof t->name);   /* no File$Type name: MimeMap's */
    }
  if (builtin || !t->name[0])
    snprintf (t->label, sizeof t->label, "%s", label);
  else
    {
      char nm[16];
      memcpy (nm, t->name, sizeof nm);
      snprintf (t->label, sizeof t->label, "%s (%s)", label, nm);
    }
  if (!t->label[0])
    snprintf (t->label, sizeof t->label, "&%03X", type);
  return t;
}

static void find_types (void)
{
  const plugin_info *pl;
  int n = app_plugins (&pl), k, f;
  ntypes = 0;
  add (FT_JPEG, "JPEG", 1, "image/jpeg");
  add (FT_PNG, "PNG", 1, "image/png");
  add (FT_TIFF, "TIFF", 1, "image/tiff");
  for (k = 0; k < n; k++)
    {
      if (strcmp (pl[k].kind, "format") || !pl[k].enabled)
        continue;
      for (f = 0; f < pl[k].nformats; f++)
        {
          const char *w = pl[k].formats[f].mime;
          while (*w)
            {
              char mime[64];
              size_t l = 0;
              while (*w == ' ')
                w++;
              while (w[l] && w[l] != ' ')
                l++;
              if (l && l < sizeof mime)
                {
                  memcpy (mime, w, l);
                  mime[l] = '\0';
                  add (fsys_type_of_mime (mime), pl[k].formats[f].name, 0, mime);
                }
              w += l;
            }
        }
    }
}

/* The ticks from the Choices: "C85 B60 ..." ("-" none). Not chosen yet:
   the plug-ins' types ticked (no other program reads them), JPEG, PNG and
   TIFF not (Paint, ChangeFSI and the like may be the ones that should
   open them). */
static void load_choices (void)
{
  const prefs *p = app_prefs ();
  int i;
  mode = p->open_mode >= 0 ? p->open_mode : OPEN_RUNNING;
  data_on = p->open_data >= 0 ? p->open_data : 1;
  for (i = 0; i < ntypes; i++)
    {
      if (p->open_mode < 0)
        types[i].on = !types[i].builtin;
      else
        {
          char hex[8];
          const char *s = p->open_types, *q;
          snprintf (hex, sizeof hex, "%03X", types[i].type);
          types[i].on = 0;
          for (q = strstr (s, hex); q; q = strstr (q + 1, hex))
            if ((q == s || q[-1] == ' ') && (!q[3] || q[3] == ' '))
              types[i].on = 1;
        }
    }
}

static void save_choices (void)
{
  prefs p = *app_prefs ();
  int i;
  p.open_mode = mode;
  p.open_data = data_on;
  p.open_types[0] = '\0';
  for (i = 0; i < ntypes; i++)
    if (types[i].on && strlen (p.open_types) + 5 < sizeof p.open_types)
      {
        char hex[8];
        snprintf (hex, sizeof hex, "%s%03X", p.open_types[0] ? " " : "", types[i].type);
        strcat (p.open_types, hex);
      }
  if (!p.open_types[0])
    snprintf (p.open_types, sizeof p.open_types, "-");
  app_keep_prefs (&p);
}

/* --- names and icons for types the desktop has none for */

static int wimp_has_sprite (const char *name)
{
  int sz = 0;
  return _swix (Wimp_SpriteOp, _INR (0, 2) | _OUT (3), 40, 0, name, &sz) == NULL;
}

static char sprites_made[TYPES_MAX * 4 + 1];   /* the types given Lumen's icon: 3 hex digits each */

static int icon_ours (int type)
{
  char hex[4];
  int i;
  snprintf (hex, sizeof hex, "%03X", type);
  for (i = 0; sprites_made[i]; i += 3)
    if (!strncmp (sprites_made + i, hex, 3))
      return 1;
  return 0;
}

static void choices_file (char *out, size_t n, const char *leaf)
{
  app_choices_file (out, n, leaf);
}

/* Lumen's file icon as file_xxx and small_xxx for each type in want (an
   OS_SpriteOp user area made from !Lumen.FileIcons), saved and given to
   the Wimp (*IconSprites). 0 if made. */
static int make_icons (const int *want, int n, const char *file)
{
  char src[700];
  fsys_entry e;
  int *area, i, size;
  if (n <= 0)
    return -1;
  fsys_join (src, sizeof src, lumen_dir (), "FileIcons");
  if (fsys_stat (src, &e) || e.size <= 16 || e.size > 64 * 1024)
    return -1;
  size = (int) e.size * (n + 1) + 256;
  if (!(area = malloc ((size_t) size)))
    return -1;
  area[0] = size;
  area[2] = 16;
  if (_swix (OS_SpriteOp, _INR (0, 1), 256 + 9, area)
      || _swix (OS_SpriteOp, _INR (0, 2), 256 + 10, area, src))
    {
      free (area);
      return -1;
    }
  for (i = 0; i < n; i++)
    {
      char big[16], small[16];
      snprintf (big, sizeof big, "file_%03x", want[i]);
      snprintf (small, sizeof small, "small_%03x", want[i]);
      if (_swix (OS_SpriteOp, _INR (0, 3), 256 + 27, area, "lmnfile", big)
          || _swix (OS_SpriteOp, _INR (0, 3), 256 + 27, area, "lmnsmall", small))
        {
          free (area);
          return -1;
        }
    }
  _swix (OS_SpriteOp, _INR (0, 2), 256 + 25, area, "lmnfile");
  _swix (OS_SpriteOp, _INR (0, 2), 256 + 25, area, "lmnsmall");
  if (_swix (OS_SpriteOp, _INR (0, 2), 256 + 12, area, file))
    {
      free (area);
      return -1;
    }
  free (area);
  {
    char cmd[760];
    snprintf (cmd, sizeof cmd, "IconSprites %s", file);
    _swix (OS_CLI, _IN (0), cmd);
  }
  return 0;
}

/* --- applying the choice */

static int claimed (const ftype *t)
{
  return mode != OPEN_NEVER && t->on;
}

/* Run aliases, names and icons as the choice says; then the Types file
   !Boot runs at the next start-up. */
static void apply (void)
{
  char path[700], spr[700], name[40], val[700];
  int want[TYPES_MAX], nwant = 0, i;
  FILE *f;
  const char *dir = lumen_dir ();
  for (i = 0; i < ntypes; i++)
    {
      ftype *t = &types[i];
      int run = mode == OPEN_ALWAYS && t->on;
      snprintf (name, sizeof name, "Alias$@RunType_%03X", t->type);
      if (dir[0] && run != alias_ours (t->type))
        {
          if (run)
            {
              snprintf (val, sizeof val, "/%s %%*0", dir);
              var_set (name, val);
            }
          else
            var_set (name, NULL);
        }
      if (claimed (t) && t->unnamed && t->name[0])
        {
          snprintf (name, sizeof name, "File$Type_%03X", t->type);
          if (var_read (name, val, sizeof val) != 0 || !val[0])
            var_set (name, t->name);
        }
      if (claimed (t))
        {
          char sp[16];
          snprintf (sp, sizeof sp, "file_%03x", t->type);
          if (icon_ours (t->type) || !wimp_has_sprite (sp))
            want[nwant++] = t->type;
        }
    }
  choices_file (spr, sizeof spr, "Sprites");
  sprites_made[0] = '\0';
  if (nwant && make_icons (want, nwant, spr) == 0)
    for (i = 0; i < nwant; i++)
      {
        char hex[4];
        snprintf (hex, sizeof hex, "%03X", want[i]);
        strcat (sprites_made, hex);
      }
  if (!sprites_made[0])
    remove (spr);
  choices_file (path, sizeof path, "Types");
  if ((f = fopen (path, "w")))
    {
      fprintf (f, "| Lumen: the file types a double-click opens in Lumen (the icon bar\n"
                  "| menu's File types), with names and icons for those the desktop has\n"
                  "| none for. Run by !Lumen.!Boot; Lumen writes it again when they change.\n");
      if (sprites_made[0])
        fprintf (f, "IfThere Choices:Lumen.Sprites Then IconSprites Choices:Lumen.Sprites\n");
      for (i = 0; i < ntypes; i++)
        {
          ftype *t = &types[i];
          if (claimed (t) && t->unnamed && t->name[0])
            fprintf (f, "If \"<File$Type_%03X>\"=\"\" Then Set File$Type_%03X %s\n", t->type, t->type, t->name);
          if (mode == OPEN_ALWAYS && t->on && dir[0])
            fprintf (f, "Set Alias$@RunType_%03X /<Lumen$Dir> %%%%*0\n", t->type);
        }
      fclose (f);
      fsys_settype (path, 0xFEB);    /* Obey */
    }
}

/* --- the interface */

void filetypes_setup (void)
{
  find_types ();
  load_choices ();
  set_up = 1;
  apply ();
}

int filetypes_count (void)
{
  return ntypes;
}

const char *filetypes_label (int i)
{
  return i >= 0 && i < ntypes ? types[i].label : "";
}

int filetypes_type (int i)
{
  return i >= 0 && i < ntypes ? types[i].type : -1;
}

int filetypes_ticked (int i)
{
  return i >= 0 && i < ntypes && types[i].on;
}

int filetypes_mode (void)
{
  return mode;
}

int filetypes_data (void)
{
  return data_on;
}

void filetypes_select (int item)
{
  if (!set_up)
    filetypes_setup ();
  if (item >= 0 && item < 3)
    mode = item;
  else if (item - 3 >= 0 && item - 3 < ntypes)
    types[item - 3].on = !types[item - 3].on;
  else if (item - 3 == ntypes)
    data_on = !data_on;
  else
    return;
  save_choices ();
  apply ();
}

/* A name ending like a photo's: /jpg, /png, /tif, a raw file's, or one a
   plug-in claims. */
static int photo_name (const char *path)
{
  static const char *const ext[] = { "jpg", "jpeg", "jpe", "png", "tif", "tiff" };
  const char *dot = NULL, *c;
  size_t k;
  for (c = path; *c; c++)
    if (*c == '/' || *c == '.')
      dot = c;
  if (!dot)
    return 0;
  for (k = 0; k < sizeof ext / sizeof ext[0]; k++)
    if (!strcasecmp (dot + 1, ext[k]))
      return 1;
  return img_has_raw_extension (path) || rawjob_claims_name (path);
}

int filetypes_claims (int type, const char *path)
{
  ftype *t;
  if (!set_up)
    filetypes_setup ();
  if (mode == OPEN_NEVER)
    return 0;
  if (type == FT_DATA || type == FT_DOS || type == FT_NONE || type == 0x3000)   /* 0x3000: untyped */
    return data_on && photo_name (path);
  return (t = find (type)) && t->on;
}
