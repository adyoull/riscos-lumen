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
#include <stdint.h>
#include <stddef.h>

/* The longest a recipe's text can be (with retouching spots and brush
   strokes); buffers that hold one are this size. */
#define RECIPE_TEXT_MAX 32768

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

/* Retouching spots: a circle at (x, y), radius r, filled from a circle the
   same size at (sx, sy), healed (its colour and light matched to the
   surroundings) or cloned (copied as it is). Positions are fractions of
   the photo as it is turned upright (before any crop or straightening),
   so spots stay on what they cover; r is a fraction of the longer side.
   Text form, one line each:  spot x=0.4120 y=0.3310 r=0.0150 sx=0.4420 sy=0.3310 clone=0 f=0.30
   (f, the feathered part of the radius, is 0.30 when missing). */
#define RECIPE_SPOTS 48
typedef struct { float x, y, r, sx, sy; int clone; float f; } recipe_spot;   /* f: feather, 0..1 */

/* Brushed adjustments: strokes painted on the photo, each lightening
   (dodge) or darkening (burn) it, or strengthening or weakening its
   colour, under a brush of radius r (a fraction of the longer side) whose
   outer part f fades. amount, 0..1: for dodge and burn, 2 stops at 1;
   for the colour, double or none at 1. A stroke's points are fractions
   of the photo upright (as spots), kept in 1/10000ths. Where one stroke
   crosses itself it counts once; strokes over each other add up.
   Text form, one line each:
     brush k=dodge a=0.25 r=0.0300 f=0.50 p=4120,3310;4180,3325;... */
enum { BRUSH_DODGE, BRUSH_BURN, BRUSH_SATURATE, BRUSH_DESATURATE, BRUSH_KINDS };
extern const char *const recipe_brush_keys[BRUSH_KINDS];   /* "dodge", "burn", ... */
#define RECIPE_STROKES 32
#define RECIPE_STROKE_POINTS 1024      /* all strokes' points together */
#define RECIPE_STROKE_MAX 256          /* one stroke's */
typedef struct { int kind, first, n; float r, f, amount; } recipe_stroke;

typedef struct
{
  float v[P_COUNT];
  recipe_curve curve[CURVE_COUNT];
  int nspots;
  recipe_spot spot[RECIPE_SPOTS];
  int nstrokes, npoints;
  recipe_stroke stroke[RECIPE_STROKES];
  uint16_t pt[RECIPE_STROKE_POINTS][2];   /* x, y in 1/10000ths */
} recipe;

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
/* Spots: added (its index, -1 when full), and taken away. */
int recipe_spot_add (recipe *r, const recipe_spot *sp);
void recipe_spot_remove (recipe *r, int i);
int recipe_spots_equal (const recipe *a, const recipe *b);
/* Strokes: added from n points (xy: x, y fractions), its index or -1
   when there's no room; taken away; compared. */
int recipe_stroke_add (recipe *r, int kind, float radius, float feather, float amount, const float *xy, int n);
void recipe_stroke_remove (recipe *r, int i);
int recipe_strokes_equal (const recipe *a, const recipe *b);
/* Formats only the look (no geometry lines): what a preset stores. */
int recipe_format_look (const recipe *r, char *out, size_t n);

#endif
