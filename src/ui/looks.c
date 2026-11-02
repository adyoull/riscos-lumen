/* looks.c: a look is the tone and colour part of a recipe (no geometry),
   as text. Copy and Paste carry one between photos; a preset is a named
   one kept in the catalogue. */
#include <stdlib.h>
#include <string.h>
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

int looks_presets (cat_preset **out)
{
  int n = 0;
  free (presets);
  presets = NULL;
  if (cat_presets (lib_cat (app_library ()), &presets, &n))
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
  return cat_preset_save (lib_cat (app_library ()), name, text) > 0 ? 0 : -1;
}

void looks_delete_preset (cat_id id)
{
  cat_preset_delete (lib_cat (app_library ()), id);
}
