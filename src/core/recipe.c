/* recipe.c: see recipe.h. */
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "recipe.h"

const recipe_param recipe_params[P_COUNT] = {
  { "wb",       "temp",       "Temperature", -100, 100, 0, 1, 0 },
  { "wb",       "tint",       "Tint",        -100, 100, 0, 1, 0 },
  { "exposure", "ev",         "Exposure",      -4,   4, 0, 0.05f, 2 },
  { "levels",   "black",      "Black point",    0,  25, 0, 0.5f, 1 },
  { "levels",   "white",      "White point",   75, 100, 100, 0.5f, 1 },
  { "levels",   "mid",        "Midtones",    -100, 100, 0, 1, 0 },
  { "enhance",  "contrast",   "Contrast",    -100, 100, 0, 1, 0 },
  { "enhance",  "saturation", "Saturation",  -100, 100, 0, 1, 0 },
  { "enhance",  "vibrance",   "Vibrancy",    -100, 100, 0, 1, 0 },
};

const recipe_group recipe_groups[] = {
  { "White Balance", P_TEMP, P_TINT },
  { "Exposure", P_EV, P_EV },
  { "Levels", P_BLACK, P_MID },
  { "Enhance", P_CONTRAST, P_VIBRANCE },
};
const int recipe_ngroups = (int) (sizeof recipe_groups / sizeof recipe_groups[0]);

void recipe_default (recipe *r)
{
  int i;
  for (i = 0; i < P_COUNT; i++)
    r->v[i] = recipe_params[i].def;
}

static int same (float a, float b)
{
  return fabsf (a - b) < 1e-4f;
}

int recipe_is_default (const recipe *r)
{
  int i;
  for (i = 0; i < P_COUNT; i++)
    if (!same (r->v[i], recipe_params[i].def))
      return 0;
  return 1;
}

int recipe_equal (const recipe *a, const recipe *b)
{
  int i;
  for (i = 0; i < P_COUNT; i++)
    if (!same (a->v[i], b->v[i]))
      return 0;
  return 1;
}

void recipe_set (recipe *r, int p, float value)
{
  const recipe_param *d = &recipe_params[p];
  if (value < d->min)
    value = d->min;
  if (value > d->max)
    value = d->max;
  value = d->min + roundf ((value - d->min) / d->step) * d->step;
  if (value > d->max)
    value = d->max;
  if (same (value, 0))
    value = 0;                         /* no "-0" in the text form */
  r->v[p] = value;
}

int recipe_parse (recipe *r, const char *text)
{
  recipe_default (r);
  while (text && *text)
    {
      char line[256], *tok, *save = NULL;
      const char *end = strchr (text, '\n');
      size_t len = end ? (size_t) (end - text) : strlen (text);
      if (len >= sizeof line)
        len = sizeof line - 1;
      memcpy (line, text, len);
      line[len] = '\0';
      text = end ? end + 1 : text + strlen (text);
      tok = strtok_r (line, " \t\r", &save);
      if (!tok)
        continue;
      {
        char group[32];
        snprintf (group, sizeof group, "%s", tok);
        while ((tok = strtok_r (NULL, " \t\r", &save)) != NULL)
          {
            char *eq = strchr (tok, '=');
            int p;
            if (!eq)
              continue;
            *eq = '\0';
            for (p = 0; p < P_COUNT; p++)
              if (strcmp (recipe_params[p].group, group) == 0 && strcmp (recipe_params[p].key, tok) == 0)
                recipe_set (r, p, strtof (eq + 1, NULL));
          }
      }
    }
  return 0;
}

int recipe_format (const recipe *r, char *out, size_t n)
{
  size_t used = 0;
  int g, p, k;
  if (n == 0)
    return -1;
  out[0] = '\0';
  for (g = 0; g < recipe_ngroups; g++)
    {
      int first = recipe_groups[g].first, last = recipe_groups[g].last, any = 0;
      for (p = first; p <= last; p++)
        if (!same (r->v[p], recipe_params[p].def))
          any = 1;
      if (!any)
        continue;
      k = snprintf (out + used, n - used, "%s", recipe_params[first].group);
      if (k < 0 || used + (size_t) k >= n)
        return -1;
      used += (size_t) k;
      for (p = first; p <= last; p++)
        {
          k = snprintf (out + used, n - used, " %s=%.*f", recipe_params[p].key,
                        recipe_params[p].decimals, (double) r->v[p]);
          if (k < 0 || used + (size_t) k >= n)
            return -1;
          used += (size_t) k;
        }
      if (used + 2 > n)
        return -1;
      out[used++] = '\n';
      out[used] = '\0';
    }
  return (int) used;
}
