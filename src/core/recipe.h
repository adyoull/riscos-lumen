/* recipe: a version's adjustments. Originals are never changed; a version
   is an original plus a recipe, stored in the catalogue as text, one line
   per adjustment, only the ones that differ from their defaults:

     wb temp=12 tint=-3
     exposure ev=0.35
     hs highlights=30 shadows=45
     levels black=2 white=98 mid=10
     enhance contrast=12 saturation=0 vibrance=18
     tone shadow_hue=210 shadows=30 highlight_hue=40 highlights=25
     hsl blue_hue=-20 blue_saturation=30 green_luminance=-15
     bw amount=100 red=60 green=30 blue=10
     noise luminance=30 colour=50
     sharpen amount=40 radius=1.0
     vignette amount=-40 midpoint=50
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
  P_HIGHLIGHTS, P_SHADOWS,             /* highlights & shadows (local, see develop.h) */
  P_BLACK, P_WHITE, P_MID,             /* levels */
  P_CONTRAST, P_SATURATION, P_VIBRANCE,/* enhance */
  P_HSL,                               /* colour: 8 hue bands x hue, saturation, luminance (HSL_*) */
  P_HSL_END = P_HSL + 8 * 3 - 1,
  P_BW_AMOUNT, P_BW_RED, P_BW_GREEN, P_BW_BLUE,   /* black & white: the mix of channels, and how much */
  P_TONE_SH_HUE, P_TONE_SH, P_TONE_HI_HUE, P_TONE_HI,   /* split toning: after saturation, so it tints black and white too */
  P_NOISE_LUM, P_NOISE_COL,            /* noise reduction (after develop, before sharpening) */
  P_SHARPEN, P_SHARP_RADIUS,           /* sharpen (applied after develop, at the output's size) */
  P_VIG_AMOUNT, P_VIG_MID,             /* vignette: darker (or lighter) towards the corners */
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
typedef struct { const char *name; int first, last; int panel; int curves; int hsl; } recipe_group;

/* The colour (HSL) bands: parameter P_HSL + band * 3 + HSL_HUE etc. */
#define HSL_BANDS 8
enum { HSL_RED, HSL_ORANGE, HSL_YELLOW, HSL_GREEN, HSL_AQUA, HSL_BLUE, HSL_PURPLE, HSL_MAGENTA };
enum { HSL_HUE, HSL_SAT, HSL_LUM };
extern const char *const recipe_hsl_bands[HSL_BANDS];   /* "Red", "Orange"... */
extern const float recipe_hsl_centre[HSL_BANDS];        /* each band's hue, degrees */
#define HSL_PARAM(band, what) (P_HSL + (band) * 3 + (what))
extern const recipe_group recipe_groups[];
extern const int recipe_ngroups;

/* Curves: for RGB together and for red, green and blue, up to
   CURVE_POINTS points (x, y in 0..1, sorted by x, the first and last
   are the ends). No points: a straight line. Text form, one line:
     curves rgb=0.000:0.000,0.250:0.180,1.000:1.000 red=...  */
#define CURVE_POINTS 10
enum { CURVE_RGB, CURVE_RED, CURVE_GREEN, CURVE_BLUE, CURVE_COUNT };
typedef struct { int n; float x[CURVE_POINTS], y[CURVE_POINTS]; } recipe_curve;

typedef struct { float v[P_COUNT]; recipe_curve curve[CURVE_COUNT]; } recipe;

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
/* A curve with no effect (no points, or points on the diagonal)? */
int recipe_curve_is_identity (const recipe_curve *c);
/* The curve's value at x (monotone cubic through the points). */
double recipe_curve_eval (const recipe_curve *c, double x);
/* 0..1 in, 0..1 out, n entries (n >= 2), for develop's tables. */
void recipe_curve_table (const recipe_curve *c, float *out, int n);
/* Adds a point at x (on the curve), keeping the order; its index, or -1. */
int recipe_curve_add (recipe_curve *c, float x);
void recipe_curve_remove (recipe_curve *c, int i);
/* Moves point i, kept between its neighbours. */
void recipe_curve_move (recipe_curve *c, int i, float x, float y);
/* Formats only the look (no geometry lines): what a preset stores. */
int recipe_format_look (const recipe *r, char *out, size_t n);

#endif
