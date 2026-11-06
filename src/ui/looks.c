/* Copyright (c) 2026 Andrew Youll
   Released under the MIT licence: see the LICENSE file.
   SPDX-License-Identifier: MIT */
/* looks.c: a look is the tone and colour part of a recipe (no geometry),
   as text. Copy and Paste carry one between photos; a preset is a named
   one, a file in Lumen's Presets directory (presets.h), shared by every
   library. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "fsys.h"
#include "presets.h"
#include "ui.h"

static char clip[2048];
static int have_clip;
static cat_preset *presets;

void looks_copy_viewer (void)
{
  if (!viewer_item ())
    return;
  viewer_look (clip, sizeof clip);
  have_clip = 1;
}

void looks_copy_selection (void)
{
  cat_id *ids;
  char text[RECIPE_TEXT_MAX];
  recipe r;
  int n = browser_selection (&ids);
  if (n > 0 && cat_get_recipe (lib_cat (app_library ()), ids[0], text, sizeof text) == 0)
    {
      recipe_parse (&r, text);
      recipe_format_look (&r, clip, sizeof clip);
      have_clip = 1;
    }
  free (ids);
}

const char *looks_clip (void)
{
  return have_clip ? clip : NULL;
}

void looks_apply_selection (const char *look)
{
  cat_id *ids;
  int n = browser_selection (&ids), k;
  if (n > 0 && cat_apply_look (lib_cat (app_library ()), ids, n, look) == 0)
    {
      lib_thumbs_changed (app_library ());
      browser_after_change ();
      for (k = 0; k < n; k++)
        viewer_reload (ids[k]);
    }
  free (ids);
}

void looks_presets_dir (void)
{
  char d[1024];
  app_choices_file (d, sizeof d, "Presets");
  presets_set_dir (d);
}

int looks_presets (cat_preset **out)
{
  int n = 0;
  if (!presets_dir ()[0])
    looks_presets_dir ();
  /* A library's own (from before 0.0.38, or an imported one's) move to
     the directory the first time they're seen. */
  if (app_library ())
    presets_take_from (lib_cat (app_library ()));
  free (presets);
  presets = NULL;
  if (presets_list (&presets, &n))
    n = 0;
  *out = presets;
  return n;
}

int looks_save_preset (const char *name)
{
  char text[RECIPE_TEXT_MAX];
  if (!name[0] || !viewer_item ())
    return -1;
  viewer_look (text, sizeof text);
  if (!presets_dir ()[0])
    looks_presets_dir ();
  return presets_save (name, text);
}

void looks_delete_preset (const char *name)
{
  presets_delete (name);
}

int looks_preset_dropped (const char *path, int type)
{
  char name[48], look[16], msg[160];
  if (type != FT_TEXT || presets_read_file (path, name, sizeof name, look, sizeof look))
    return 0;
  if (!presets_dir ()[0])
    looks_presets_dir ();
  if (presets_add_file (path))
    app_report ("The preset couldn't be saved in Lumen's Choices.");
  else
    {
      snprintf (msg, sizeof msg, "Preset '%s' added: it's on the Presets menu in the Viewer.", name);
      activity_log (ACT_OTHER, "Added preset %s", name);
      app_inform (msg);
    }
  return 1;
}
