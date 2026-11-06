/* Copyright (c) 2026 Andrew Youll
   Released under the MIT licence: see the LICENSE file.
   SPDX-License-Identifier: MIT */
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
     noise luminance=30 colour=50 raw=1
     sharpen amount=40 radius=1.0
     vignette amount=-40 midpoint=50
     glow amount=25 spread=40 highlights=60 warmth=20
     geometry rotate=1 angle=-1.5 vertical=0 horizontal=0
     crop x0=0.050 y0=0.000 x1=0.950 y1=1.000
     lenscorr distortion=100 fringing=50 vignetting=100 manual=-20
     lens d=p:0.01589,-0.08067,0.06712 t=3:1.00006,0,0,1.00033,0,0 v=-0.49,0.016,-0.19 rh=1.8028 rv=1.0000 focal=16 aperture=4.0 type=r
     lensname Nikon AF-S Nikkor 16-35mm f/4G ED VR
     lenscamera Nikon D750                (a camera chosen to stand in for the photo's)

   An empty recipe is the original. The parameter table also drives the
   adjustments panel (labels, ranges, steps), as plug-ins' parameters will
   in milestone 3. */
#ifndef LUMEN_RECIPE_H
#define LUMEN_RECIPE_H
#include <stdint.h>
#include <stddef.h>

/* The longest a recipe's text can be (with retouching spots, masks and
   their strokes); buffers that hold one are this size. */
#define RECIPE_TEXT_MAX 49152

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
  P_NOISE_RAW,                         /* a raw photo's own, in the raw plug-in (0 off, 1 light, 2 strong) */
  P_SHARPEN, P_SHARP_RADIUS,           /* sharpen (applied after develop, at the output's size) */
  P_VIG_AMOUNT, P_VIG_MID,             /* vignette: darker (or lighter) towards the corners */
  P_GLOW_AMOUNT, P_GLOW_SPREAD, P_GLOW_HIGH, P_GLOW_WARM,   /* glow: light from the bright parts spread round them */
  P_ROTATE, P_ANGLE, P_PERSP_V, P_PERSP_H,  /* geometry: quarter turns, straighten, perspective */
  P_CROP_X0, P_CROP_Y0, P_CROP_X1, P_CROP_Y1, /* crop, as fractions of the straightened picture */
  P_LENS_DIST, P_LENS_TCA, P_LENS_VIG, /* lens correction: how much of the lens profile's (percent) */
  P_LENS_MANUAL,                       /* and barrel (+) or pincushion (-) distortion corrected by hand */
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
   surroundings, seamlessly) or cloned (copied as it is). Positions are
   fractions of the photo as it is turned upright (before any crop or
   straightening), so spots stay on what they cover; r is a fraction of
   the longer side. A spot can also be painted: a path of points (n of
   them from first in spt, 1/10000ths; the first is (x, y)), the brush of
   radius r along it, its source the same shape moved by (sx - x, sy - y).
   Text form, one line each:
     spot x=0.4120 y=0.3310 r=0.0150 sx=0.4420 sy=0.3310 clone=0 f=0.30
     spot x=... clone=0 f=0.30 p=4120,3310;4180,3325;...      (painted)
   (f, the feathered part of the radius, is 0.30 when missing). */
#define RECIPE_SPOTS 48
#define RECIPE_SPOT_POINTS 512         /* all painted spots' points together */
typedef struct { float x, y, r, sx, sy; int clone; float f; int first, n; } recipe_spot;   /* f: feather, 0..1; n: 0 or 1 a circle */

/* Masks: adjustments of their own on part of the photo, like layers. A
   mask is painted (brush), a graduated filter (gradient: all of it from
   (x0, y0), fading to none at (x1, y1), and beyond) or an oval (radial:
   centre (x0, y0), half-widths x1 of the width and y1 of the height, its
   outer part f fading; inverted, everything but the oval). Any of them
   can be painted on (add) or off (erase) with the brush. Each has its own
   exposure, contrast, highlights, shadows, temperature, tint, saturation,
   clarity and sharpness, applied where the mask is, as much as it is.
   Places are fractions of the photo upright (as spots).
   Text form, a line for each mask, then a line for each stroke on one:
     mask k=radial x0=0.5000 y0=0.4000 x1=0.2000 y1=0.3000 f=0.50 invert=1 ev=0.50 shadows=30
     (off=1 after k=: switched off, there but not applied)
     paint m=0 k=add a=1.00 r=0.0300 f=0.50 p=4120,3310;4180,3325;...
   Strokes: under a brush of radius r (a fraction of the longer side)
   whose outer part f fades, amount 0..1 (flow); where strokes overlap on
   a mask the most counts; erasing takes the mask away again.
   (Before 0.0.38, "brush k=dodge|burn|saturate|desaturate a= r= f= p="
   lines were each a stroke of its own kind: each is read as a brush mask
   with the exposure or saturation it gave.) */
enum { MASK_BRUSH, MASK_GRADIENT, MASK_RADIAL, MASK_KINDS };
extern const char *const recipe_mask_keys[MASK_KINDS];     /* "brush", "gradient", "radial" */
enum { M_EV, M_CONTRAST, M_HIGHLIGHTS, M_SHADOWS, M_TEMP, M_TINT, M_SATURATION, M_CLARITY, M_SHARPNESS, M_COUNT };
extern const recipe_param recipe_mask_params[M_COUNT];      /* key, label, range; group unused */
#define RECIPE_MASKS 32
typedef struct { int kind, invert, off; float v[M_COUNT]; float x0, y0, x1, y1, f; } recipe_mask;   /* off: switched off (kept, not applied) */

enum { BRUSH_ADD, BRUSH_ERASE, BRUSH_KINDS };
extern const char *const recipe_brush_keys[BRUSH_KINDS];   /* "add", "erase" */
#define RECIPE_STROKES 64
#define RECIPE_STROKE_POINTS 1024      /* all strokes' points together */
#define RECIPE_STROKE_MAX 256          /* one stroke's (or painted spot's) */
typedef struct { int mask, kind, first, n; float r, f, amount; } recipe_stroke;

/* Lens correction: a lens's profile (from the lens database, lens.h),
   worked out for the photo's focal length and aperture, kept in the
   recipe so the photo looks the same with or without the database (or a
   newer one). The models are lensfun's (its database's terms):
   distortion maps a point's distance from the centre in the corrected
   picture, ru, to where it is in the photo, rd:
     ptlens: rd = ru (a ru^3 + b ru^2 + c ru + 1 - a - b - c)
     poly3:  rd = ru (1 - k1 + k1 ru^2)
     poly5:  rd = ru (1 + k1 ru^2 + k2 ru^4)
   lateral colour moves red and blue out from green's place, r:
     linear: r * kr, r * kb
     poly3:  r (vr + cr r + br r^2), r (vb + cb r + bb r^2)
   and vignetting darkens by 1 + k1 r^2 + k2 r^4 + k3 r^6.
   Distances for distortion and colour are rh at the corner (1 at half
   the shorter side of the camera the lens was measured on); for
   vignetting rv (1 at that camera's corner). */
enum { LENS_D_NONE, LENS_D_PTLENS, LENS_D_POLY3, LENS_D_POLY5 };
enum { LENS_T_NONE, LENS_T_LINEAR, LENS_T_POLY3 };
typedef struct
{
  int on;                              /* a profile is set */
  char name[96];                       /* the lens, as the database names it */
  char camera[64];                     /* the camera it's scaled for, when chosen to stand in for the photo's ("" if not) */
  int dist;                            /* LENS_D_* */
  float d[3];                          /* ptlens a b c; poly3 k1; poly5 k1 k2 */
  int tca;                             /* LENS_T_* */
  float t[6];                          /* linear kr kb; poly3 vr cr br vb cb bb */
  int vig;                             /* 1: k[] is set */
  float k[3];
  float rh, rv;
  float focal, aperture;               /* what it was worked out for (0 unknown) */
  char type;                           /* 'r' rectilinear, 'f' fisheye... (lensdb.py) */
} recipe_lens;

typedef struct
{
  float v[P_COUNT];
  recipe_lens lens;
  recipe_curve curve[CURVE_COUNT];
  int nspots, nspoints;
  recipe_spot spot[RECIPE_SPOTS];
  uint16_t spt[RECIPE_SPOT_POINTS][2];   /* painted spots' paths, x, y in 1/10000ths */
  int nmasks;
  recipe_mask mask[RECIPE_MASKS];
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

/* Geometry (rotation, straightening, perspective, crop, lens correction) is all default. */
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
/* Spots: added (its index, -1 when full), and taken away. A painted one
   from n points (xy: x, y fractions; its x, y are the first, sx, sy its
   source's first). Moved: by du, dv (its source too unless only_source),
   kept on the photo. */
int recipe_spot_add (recipe *r, const recipe_spot *sp);
int recipe_spot_add_path (recipe *r, const recipe_spot *sp, const float *xy, int n);
void recipe_spot_remove (recipe *r, int i);
void recipe_spot_move (recipe *r, int i, float du, float dv, int only_source);
int recipe_spots_equal (const recipe *a, const recipe *b);
/* Point j of spot i (its path, or the centre), as fractions. */
void recipe_spot_point (const recipe *r, int i, int j, float *u, float *v);
/* Masks: added (its index, -1 when full; m's strokes aren't copied),
   taken away with its strokes, compared (strokes too). A new mask:
   recipe_mask_init. */
void recipe_mask_init (recipe_mask *m, int kind);
int recipe_mask_add (recipe *r, const recipe_mask *m);
void recipe_mask_remove (recipe *r, int i);
void recipe_mask_set (recipe_mask *m, int p, float value);   /* clamped and stepped */
int recipe_masks_equal (const recipe *a, const recipe *b);
/* Strokes on mask `mask`: added from n points (xy: x, y fractions), its
   index or -1 when there's no room; taken away; compared. */
int recipe_stroke_add (recipe *r, int mask, int kind, float radius, float feather, float amount, const float *xy, int n);
void recipe_stroke_remove (recipe *r, int i);
int recipe_strokes_equal (const recipe *a, const recipe *b);
/* Lens profiles: the same? (Only what changes the picture, and the name.) */
int recipe_lens_equal (const recipe_lens *a, const recipe_lens *b);
/* Formats only the look (no geometry lines): what a preset stores. */
int recipe_format_look (const recipe *r, char *out, size_t n);

#endif
