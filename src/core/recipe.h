/* recipe: a version's adjustments. Originals are never changed; a version
   is an original plus a recipe, stored in the catalogue as text, one line
   per adjustment, only the ones that differ from their defaults:

     wb temp=12 tint=-3
     exposure ev=0.35
     levels black=2 white=98 mid=10
     enhance contrast=12 saturation=0 vibrance=18
     geometry rotate=1 angle=-1.5 vertical=0 horizontal=0
     crop x0=0.050 y0=0.000 x1=0.950 y1=1.000

   An empty recipe is the original. The parameter table also drives the
   adjustments panel (labels, ranges, steps), as plug-ins' parameters will
   in milestone 3. */
#ifndef LUMEN_RECIPE_H
#define LUMEN_RECIPE_H
#include <stddef.h>

enum
{
  P_TEMP, P_TINT,                      /* white balance */
  P_EV,                                /* exposure */
  P_BLACK, P_WHITE, P_MID,             /* levels */
  P_CONTRAST, P_SATURATION, P_VIBRANCE,/* enhance */
  P_ROTATE, P_ANGLE, P_PERSP_V, P_PERSP_H,  /* geometry: quarter turns, straighten, perspective */
  P_CROP_X0, P_CROP_Y0, P_CROP_X1, P_CROP_Y1, /* crop, as fractions of the straightened picture */
  P_COUNT,
  P_FIRST_GEOMETRY = P_ROTATE
};

typedef struct
{
  const char *group;                   /* the line's first word */
  const char *key;
  const char *label;                   /* for the panel */
  float min, max, def, step;
  int decimals;                        /* for showing the value */
  int hidden;                          /* no slider (set by buttons or the crop tool) */
} recipe_param;

extern const recipe_param recipe_params[P_COUNT];

/* The panel's groups, in order: name and first/last parameter. */
typedef struct { const char *name; int first, last; int panel; } recipe_group;
extern const recipe_group recipe_groups[];
extern const int recipe_ngroups;

typedef struct { float v[P_COUNT]; } recipe;

void recipe_default (recipe *r);
int recipe_is_default (const recipe *r);
int recipe_equal (const recipe *a, const recipe *b);
/* Sets one parameter, clamped to its range and rounded to its step. */
void recipe_set (recipe *r, int p, float value);
/* Text form. Unknown lines and keys are ignored (a newer Lumen's recipe
   still opens). Returns 0. */
int recipe_parse (recipe *r, const char *text);
/* Writes the text form ("" for the defaults). Returns the length, or -1 if
   n was too small. */
int recipe_format (const recipe *r, char *out, size_t n);

/* Geometry (rotation, straightening, perspective, crop) is all default. */
int recipe_geometry_is_default (const recipe *r);
/* The tone and colour adjustments of src into dst, keeping dst's geometry
   (presets and pasting carry looks, not crops). */
void recipe_copy_look (recipe *dst, const recipe *src);
/* Formats only the look (no geometry lines): what a preset stores. */
int recipe_format_look (const recipe *r, char *out, size_t n);

#endif
