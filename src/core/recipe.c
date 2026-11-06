/* Copyright (c) 2026 Andrew Youll
   Released under the MIT licence: see the LICENSE file.
   SPDX-License-Identifier: MIT */
/* recipe.c: see recipe.h. */
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "recipe.h"

const recipe_param recipe_params[P_COUNT] = {
  { "wb",       "temp",       "Temperature", -100, 100, 0, 1, 0, 0 },
  { "wb",       "tint",       "Tint",        -100, 100, 0, 1, 0, 0 },
  { "exposure", "ev",         "Exposure",      -4,   4, 0, 0.05f, 2, 0 },
  { "hs",       "highlights", "Highlights",     0, 100, 0, 1, 0, 0 },
  { "hs",       "shadows",    "Shadows",        0, 100, 0, 1, 0, 0 },
  { "levels",   "black",      "Black point",    0,  25, 0, 0.5f, 1, 0 },
  { "levels",   "white",      "White point",   75, 100, 100, 0.5f, 1, 0 },
  { "levels",   "mid",        "Midtones",    -100, 100, 0, 1, 0, 0 },
  { "enhance",  "contrast",   "Contrast",    -100, 100, 0, 1, 0, 0 },
  { "enhance",  "saturation", "Saturation",  -100, 100, 0, 1, 0, 0 },
  { "enhance",  "vibrance",   "Vibrancy",    -100, 100, 0, 1, 0, 0 },
  { "hsl",      "red_hue", "Hue", -100, 100, 0, 1, 0, 1 },
  { "hsl",      "red_saturation", "Saturation", -100, 100, 0, 1, 0, 1 },
  { "hsl",      "red_luminance", "Luminance", -100, 100, 0, 1, 0, 1 },
  { "hsl",      "orange_hue", "Hue", -100, 100, 0, 1, 0, 1 },
  { "hsl",      "orange_saturation", "Saturation", -100, 100, 0, 1, 0, 1 },
  { "hsl",      "orange_luminance", "Luminance", -100, 100, 0, 1, 0, 1 },
  { "hsl",      "yellow_hue", "Hue", -100, 100, 0, 1, 0, 1 },
  { "hsl",      "yellow_saturation", "Saturation", -100, 100, 0, 1, 0, 1 },
  { "hsl",      "yellow_luminance", "Luminance", -100, 100, 0, 1, 0, 1 },
  { "hsl",      "green_hue", "Hue", -100, 100, 0, 1, 0, 1 },
  { "hsl",      "green_saturation", "Saturation", -100, 100, 0, 1, 0, 1 },
  { "hsl",      "green_luminance", "Luminance", -100, 100, 0, 1, 0, 1 },
  { "hsl",      "aqua_hue", "Hue", -100, 100, 0, 1, 0, 1 },
  { "hsl",      "aqua_saturation", "Saturation", -100, 100, 0, 1, 0, 1 },
  { "hsl",      "aqua_luminance", "Luminance", -100, 100, 0, 1, 0, 1 },
  { "hsl",      "blue_hue", "Hue", -100, 100, 0, 1, 0, 1 },
  { "hsl",      "blue_saturation", "Saturation", -100, 100, 0, 1, 0, 1 },
  { "hsl",      "blue_luminance", "Luminance", -100, 100, 0, 1, 0, 1 },
  { "hsl",      "purple_hue", "Hue", -100, 100, 0, 1, 0, 1 },
  { "hsl",      "purple_saturation", "Saturation", -100, 100, 0, 1, 0, 1 },
  { "hsl",      "purple_luminance", "Luminance", -100, 100, 0, 1, 0, 1 },
  { "hsl",      "magenta_hue", "Hue", -100, 100, 0, 1, 0, 1 },
  { "hsl",      "magenta_saturation", "Saturation", -100, 100, 0, 1, 0, 1 },
  { "hsl",      "magenta_luminance", "Luminance", -100, 100, 0, 1, 0, 1 },
  { "bw",       "amount",     "Amount",         0, 100, 0, 1, 0, 0 },
  { "bw",       "red",        "Red",         -100, 200, 30, 1, 0, 0 },
  { "bw",       "green",      "Green",       -100, 200, 59, 1, 0, 0 },
  { "bw",       "blue",       "Blue",        -100, 200, 11, 1, 0, 0 },
  { "tone",     "shadow_hue", "Shadow hue",     0, 360, 0, 1, 0, 0 },
  { "tone",     "shadows",    "Shadows",        0, 100, 0, 1, 0, 0 },
  { "tone",     "highlight_hue", "Highlight hue", 0, 360, 0, 1, 0, 0 },
  { "tone",     "highlights", "Highlights",     0, 100, 0, 1, 0, 0 },
  { "noise",    "luminance",  "Luminance",      0, 100, 0, 1, 0, 0 },
  { "noise",    "colour",     "Colour",         0, 100, 0, 1, 0, 0 },
  { "noise",    "raw",        "Raw",            0,   2, 0, 1, 0, 0 },
  { "sharpen",  "amount",     "Amount",         0, 200, 0, 1, 0, 0 },
  { "sharpen",  "radius",     "Radius",       0.3,   3, 1, 0.1f, 1, 0 },
  { "vignette", "amount",     "Amount",      -100, 100, 0, 1, 0, 0 },
  { "vignette", "midpoint",   "Midpoint",       0, 100, 50, 1, 0, 0 },
  { "glow",     "amount",     "Amount",         0, 100, 0, 1, 0, 0 },
  { "glow",     "spread",     "Spread",         0, 100, 40, 1, 0, 0 },
  { "glow",     "highlights", "Highlights",     0, 100, 60, 1, 0, 0 },
  { "glow",     "warmth",     "Warmth",      -100, 100, 0, 1, 0, 0 },
  { "geometry", "rotate",     "Rotate",         0,   3, 0, 1, 0, 1 },
  { "geometry", "angle",      "Straighten",   -15,  15, 0, 0.1f, 1, 0 },
  { "geometry", "vertical",   "Vertical",    -100, 100, 0, 1, 0, 0 },
  { "geometry", "horizontal", "Horizontal",  -100, 100, 0, 1, 0, 0 },
  { "crop",     "x0",         "Left",           0,   1, 0, 0.001f, 3, 1 },
  { "crop",     "y0",         "Top",            0,   1, 0, 0.001f, 3, 1 },
  { "crop",     "x1",         "Right",          0,   1, 1, 0.001f, 3, 1 },
  { "crop",     "y1",         "Bottom",         0,   1, 1, 0.001f, 3, 1 },
  { "lenscorr", "distortion", "Distortion",     0, 200, 100, 1, 0, 0 },
  { "lenscorr", "fringing",   "Fringing",       0, 200, 100, 1, 0, 0 },
  { "lenscorr", "vignetting", "Vignetting",     0, 200, 100, 1, 0, 0 },
  { "lenscorr", "manual",     "Manual",      -100, 100, 0, 1, 0, 0 },
};

const recipe_group recipe_groups[] = {
  { "White Balance", P_TEMP, P_TINT, 1, 0, 0 },
  { "Exposure", P_EV, P_EV, 1, 0, 0 },
  { "Highlights & Shadows", P_HIGHLIGHTS, P_SHADOWS, 1, 0, 0 },
  { "Levels", P_BLACK, P_MID, 1, 0, 0 },
  { "Curves", 1, 0, 1, 1, 0 },         /* no sliders: the curves editor */
  { "Enhance", P_CONTRAST, P_VIBRANCE, 1, 0, 0 },
  { "Colour", P_HSL, P_HSL_END, 1, 0, 1 },   /* a band's three sliders at a time */
  { "Black & White", P_BW_AMOUNT, P_BW_BLUE, 1, 0, 0 },
  { "Split Toning", P_TONE_SH_HUE, P_TONE_HI, 1, 0, 0 },
  { "Noise Reduction", P_NOISE_LUM, P_NOISE_RAW, 1, 0, 0 },
  { "Sharpen", P_SHARPEN, P_SHARP_RADIUS, 1, 0, 0 },
  { "Vignette", P_VIG_AMOUNT, P_VIG_MID, 1, 0, 0 },
  { "Glow", P_GLOW_AMOUNT, P_GLOW_WARM, 1, 0, 0 },
  { "Lens Correction", P_LENS_DIST, P_LENS_MANUAL, 1, 0, 0 },
  { "Geometry", P_ROTATE, P_PERSP_H, 1, 0, 0 },
  { "Crop", P_CROP_X0, P_CROP_Y1, 0, 0, 0 },
};

const char *const recipe_hsl_bands[HSL_BANDS] = { "Red", "Orange", "Yellow", "Green", "Aqua", "Blue", "Purple", "Magenta" };
const float recipe_hsl_centre[HSL_BANDS] = { 0, 30, 60, 120, 180, 225, 270, 315 };
const int recipe_ngroups = (int) (sizeof recipe_groups / sizeof recipe_groups[0]);

void recipe_default (recipe *r)
{
  int i;
  memset (r, 0, sizeof *r);
  for (i = 0; i < P_COUNT; i++)
    r->v[i] = recipe_params[i].def;
}

static const char *const curve_keys[CURVE_COUNT] = { "rgb", "red", "green", "blue" };

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
  for (i = 0; i < CURVE_COUNT; i++)
    if (!recipe_curve_is_identity (&r->curve[i]))
      return 0;
  if (r->nspots || r->nstrokes || r->nmasks || r->lens.on)
    return 0;
  return 1;
}

static int curve_equal (const recipe_curve *a, const recipe_curve *b)
{
  int i;
  if (recipe_curve_is_identity (a) && recipe_curve_is_identity (b))
    return 1;
  if (a->n != b->n)
    return 0;
  for (i = 0; i < a->n; i++)
    if (!same (a->x[i], b->x[i]) || !same (a->y[i], b->y[i]))
      return 0;
  return 1;
}

int recipe_equal (const recipe *a, const recipe *b)
{
  int i;
  for (i = 0; i < P_COUNT; i++)
    if (!same (a->v[i], b->v[i]))
      return 0;
  for (i = 0; i < CURVE_COUNT; i++)
    if (!curve_equal (&a->curve[i], &b->curve[i]))
      return 0;
  if (!recipe_spots_equal (a, b) || !recipe_masks_equal (a, b) || !recipe_strokes_equal (a, b))
    return 0;
  return recipe_lens_equal (&a->lens, &b->lens);
}

int recipe_lens_equal (const recipe_lens *a, const recipe_lens *b)
{
  int i;
  if (a->on != b->on)
    return 0;
  if (!a->on)
    return 1;
  if (a->dist != b->dist || a->tca != b->tca || a->vig != b->vig || a->type != b->type || strcmp (a->name, b->name) || strcmp (a->camera, b->camera)
      || !same (a->rh, b->rh) || !same (a->rv, b->rv) || !same (a->focal, b->focal) || !same (a->aperture, b->aperture))
    return 0;
  for (i = 0; i < 3; i++)
    if (a->d[i] != b->d[i] || a->k[i] != b->k[i])
      return 0;
  for (i = 0; i < 6; i++)
    if (a->t[i] != b->t[i])
      return 0;
  return 1;
}

/* "1.00006,0,0" into up to n floats: how many. */
static int parse_floats (const char *s, float *v, int n)
{
  int k = 0;
  while (*s && k < n)
    {
      char *end;
      float f = strtof (s, &end);
      if (end == s)
        break;
      v[k++] = f;
      s = end;
      if (*s != ',')
        break;
      s++;
    }
  return k;
}

static void parse_lens (recipe_lens *L, char *save)
{
  char *tok;
  float v[6];
  int n;
  memset (L, 0, sizeof *L);
  L->on = 1;
  L->rh = 1.8028f;
  L->rv = 1;
  L->type = 'r';
  while ((tok = strtok_r (NULL, " \t\r", &save)) != NULL)
    {
      char *eq = strchr (tok, '=');
      if (!eq)
        continue;
      *eq++ = '\0';
      if (!strcmp (tok, "d") && eq[0] && eq[1] == ':')
        {
          n = parse_floats (eq + 2, v, 3);
          L->dist = eq[0] == 'p' && n == 3 ? LENS_D_PTLENS : eq[0] == '3' && n >= 1 ? LENS_D_POLY3
                    : eq[0] == '5' && n >= 2 ? LENS_D_POLY5 : LENS_D_NONE;
          memset (L->d, 0, sizeof L->d);
          if (L->dist)
            memcpy (L->d, v, (size_t) n * sizeof v[0]);
        }
      else if (!strcmp (tok, "t") && eq[0] && eq[1] == ':')
        {
          n = parse_floats (eq + 2, v, 6);
          L->tca = eq[0] == 'l' && n == 2 ? LENS_T_LINEAR : eq[0] == '3' && n == 6 ? LENS_T_POLY3 : LENS_T_NONE;
          memset (L->t, 0, sizeof L->t);
          if (L->tca)
            memcpy (L->t, v, (size_t) n * sizeof v[0]);
        }
      else if (!strcmp (tok, "v"))
        {
          L->vig = parse_floats (eq, v, 3) == 3;
          if (L->vig)
            memcpy (L->k, v, sizeof L->k);
        }
      else if (!strcmp (tok, "rh")) L->rh = strtof (eq, NULL);
      else if (!strcmp (tok, "rv")) L->rv = strtof (eq, NULL);
      else if (!strcmp (tok, "focal")) L->focal = strtof (eq, NULL);
      else if (!strcmp (tok, "aperture")) L->aperture = strtof (eq, NULL);
      else if (!strcmp (tok, "type") && eq[0]) L->type = eq[0];
    }
  if (!(L->rh > 0.01f && L->rh < 100) || !(L->rv > 0.01f && L->rv < 100))
    memset (L, 0, sizeof *L);
}

/* The "lens" and "lensname" lines; the length, or -1. */
static int format_lens (const recipe_lens *L, char *out, size_t room)
{
  size_t used = 0;
  int k, i;
  if (!L->on)
    return 0;
#define ADD(...) do { k = snprintf (out + used, room - used, __VA_ARGS__); \
                      if (k < 0 || used + (size_t) k >= room) \
                        return -1; \
                      used += (size_t) k; } while (0)
  ADD ("lens");
  if (L->dist)
    {
      int nd = L->dist == LENS_D_PTLENS ? 3 : L->dist == LENS_D_POLY5 ? 2 : 1;
      ADD (" d=%c:", L->dist == LENS_D_PTLENS ? 'p' : L->dist == LENS_D_POLY5 ? '5' : '3');
      for (i = 0; i < nd; i++)
        ADD ("%s%.7g", i ? "," : "", (double) L->d[i]);
    }
  if (L->tca)
    {
      int nt = L->tca == LENS_T_LINEAR ? 2 : 6;
      ADD (" t=%c:", L->tca == LENS_T_LINEAR ? 'l' : '3');
      for (i = 0; i < nt; i++)
        ADD ("%s%.8g", i ? "," : "", (double) L->t[i]);
    }
  if (L->vig)
    ADD (" v=%.7g,%.7g,%.7g", (double) L->k[0], (double) L->k[1], (double) L->k[2]);
  ADD (" rh=%.4f rv=%.4f focal=%g aperture=%g type=%c\n", (double) L->rh, (double) L->rv, (double) L->focal,
       (double) L->aperture, L->type ? L->type : 'r');
  if (L->name[0])
    ADD ("lensname %s\n", L->name);
  if (L->camera[0])
    ADD ("lenscamera %s\n", L->camera);
#undef ADD
  return (int) used;
}

/* --- curves */
int recipe_curve_is_identity (const recipe_curve *c)
{
  int i;
  for (i = 0; i < c->n; i++)
    if (!same (c->x[i], c->y[i]))
      return 0;
  return 1;
}

static void curve_start (recipe_curve *c)
{
  if (c->n < 2)
    {
      c->n = 2;
      c->x[0] = 0; c->y[0] = 0;
      c->x[1] = 1; c->y[1] = 1;
    }
}

/* Fritsch-Carlson: a cubic through the points that never overshoots,
   so a curve with rising points never turns back on itself. */
double recipe_curve_eval (const recipe_curve *c, double x)
{
  int n = c->n, k;
  double m[CURVE_POINTS], d[CURVE_POINTS], h, t, t2, t3;
  if (n < 2)
    return x;
  if (x <= c->x[0])
    return c->y[0];
  if (x >= c->x[n - 1])
    return c->y[n - 1];
  for (k = 0; k < n - 1; k++)
    {
      double dx = c->x[k + 1] - c->x[k];
      d[k] = dx > 1e-6 ? (c->y[k + 1] - c->y[k]) / dx : 0;
    }
  m[0] = d[0];
  m[n - 1] = d[n - 2];
  for (k = 1; k < n - 1; k++)
    m[k] = d[k - 1] * d[k] <= 0 ? 0 : (d[k - 1] + d[k]) / 2;
  for (k = 0; k < n - 1; k++)
    if (fabs (d[k]) < 1e-9)
      m[k] = m[k + 1] = 0;
    else
      {
        double a = m[k] / d[k], b = m[k + 1] / d[k], s = a * a + b * b;
        if (s > 9)
          {
            double tau = 3 / sqrt (s);
            m[k] = tau * a * d[k];
            m[k + 1] = tau * b * d[k];
          }
      }
  for (k = 0; k < n - 2 && x > c->x[k + 1]; k++)
    ;
  h = c->x[k + 1] - c->x[k];
  if (h < 1e-6)
    return c->y[k + 1];
  t = (x - c->x[k]) / h;
  t2 = t * t;
  t3 = t2 * t;
  {
    double y = (2 * t3 - 3 * t2 + 1) * c->y[k] + (t3 - 2 * t2 + t) * h * m[k]
      + (-2 * t3 + 3 * t2) * c->y[k + 1] + (t3 - t2) * h * m[k + 1];
    return y < 0 ? 0 : y > 1 ? 1 : y;
  }
}

void recipe_curve_table (const recipe_curve *c, float *out, int n)
{
  int i;
  for (i = 0; i < n; i++)
    out[i] = (float) recipe_curve_eval (c, (double) i / (n - 1));
}

int recipe_curve_add (recipe_curve *c, float x)
{
  int i;
  float y;
  curve_start (c);
  if (c->n >= CURVE_POINTS || x <= c->x[0] + 0.02f || x >= c->x[c->n - 1] - 0.02f)
    return -1;
  y = (float) recipe_curve_eval (c, x);
  for (i = 0; i < c->n && c->x[i] < x; i++)
    ;
  if ((i > 0 && x - c->x[i - 1] < 0.02f) || (i < c->n && c->x[i] - x < 0.02f))
    return -1;                         /* too close to a point already there */
  memmove (&c->x[i + 1], &c->x[i], sizeof c->x[0] * (size_t) (c->n - i));
  memmove (&c->y[i + 1], &c->y[i], sizeof c->y[0] * (size_t) (c->n - i));
  c->x[i] = x;
  c->y[i] = y;
  c->n++;
  return i;
}

void recipe_curve_remove (recipe_curve *c, int i)
{
  if (i <= 0 || i >= c->n - 1)
    return;                            /* the ends stay */
  memmove (&c->x[i], &c->x[i + 1], sizeof c->x[0] * (size_t) (c->n - i - 1));
  memmove (&c->y[i], &c->y[i + 1], sizeof c->y[0] * (size_t) (c->n - i - 1));
  c->n--;
}

void recipe_curve_move (recipe_curve *c, int i, float x, float y)
{
  float lo, hi;
  curve_start (c);
  if (i < 0 || i >= c->n)
    return;
  lo = i > 0 ? c->x[i - 1] + 0.01f : 0;
  hi = i < c->n - 1 ? c->x[i + 1] - 0.01f : 1;
  if (i == 0) hi = c->n > 1 ? c->x[1] - 0.01f : 1;
  if (x < lo) x = lo;
  if (x > hi) x = hi;
  c->x[i] = roundf (x * 1000) / 1000;
  c->y[i] = roundf ((y < 0 ? 0 : y > 1 ? 1 : y) * 1000) / 1000;
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

static float clamp_unit (float v)
{
  return v < 0 ? 0 : v > 1 ? 1 : v;
}

/* "4120,3310;4180,3325;..." (1/10000ths) into xy (fractions): how many. */
static int parse_points (const char *q, float *xy, int max)
{
  int n = 0;
  char *e;
  while (*q && n < max)
    {
      long x = strtol (q, &e, 10), y;
      if (e == q || *e != ',')
        break;
      q = e + 1;
      y = strtol (q, &e, 10);
      if (e == q)
        break;
      xy[2 * n] = (float) x / 10000;
      xy[2 * n + 1] = (float) y / 10000;
      n++;
      if (*e != ';')
        break;
      q = e + 1;
    }
  return n;
}

int recipe_parse (recipe *r, const char *text)
{
  recipe_default (r);
  while (text && *text)
    {
      char line[4096], *tok, *save = NULL;   /* a stroke's (painted spot's) line is the longest */
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
      if (strcmp (tok, "lens") == 0)
        {
          char keep[96], cam[64];
          snprintf (keep, sizeof keep, "%s", r->lens.name);
          snprintf (cam, sizeof cam, "%s", r->lens.camera);
          parse_lens (&r->lens, save);
          if (r->lens.on)
            {
              snprintf (r->lens.name, sizeof r->lens.name, "%s", keep);
              snprintf (r->lens.camera, sizeof r->lens.camera, "%s", cam);
            }
          continue;
        }
      if (strcmp (tok, "lensname") == 0 || strcmp (tok, "lenscamera") == 0)
        {
          /* The rest of the line (it has spaces). */
          size_t tl = strlen (tok), l;
          const char *nm = tok + tl < line + len ? tok + tl + 1 : "";
          char *dst = tok[4] == 'n' ? r->lens.name : r->lens.camera;
          size_t room = tok[4] == 'n' ? sizeof r->lens.name : sizeof r->lens.camera;
          while (*nm == ' ' || *nm == '\t')
            nm++;
          snprintf (dst, room, "%s", nm);
          l = strlen (dst);
          while (l && (dst[l - 1] == '\r' || dst[l - 1] == ' '))
            dst[--l] = '\0';
          continue;
        }
      if (strcmp (tok, "brush") == 0 || strcmp (tok, "paint") == 0)
        {
          static const char *const old_keys[4] = { "dodge", "burn", "saturate", "desaturate" };
          int paint = tok[0] == 'p', kind = paint ? BRUSH_ADD : 0, n = 0, k, m = -1;
          float rad = 0.03f, fe = 0.5f, amt = paint ? 1.0f : 0.25f, xy[RECIPE_STROKE_MAX * 2];
          while ((tok = strtok_r (NULL, " \t\r", &save)) != NULL)
            {
              char *eq = strchr (tok, '=');
              if (!eq)
                continue;
              *eq = '\0';
              if (!strcmp (tok, "k"))
                {
                  for (k = 0; k < (paint ? BRUSH_KINDS : 4); k++)
                    if (!strcmp (eq + 1, paint ? recipe_brush_keys[k] : old_keys[k]))
                      kind = k;
                }
              else if (!strcmp (tok, "m")) m = (int) strtol (eq + 1, NULL, 10);
              else if (!strcmp (tok, "a")) amt = strtof (eq + 1, NULL);
              else if (!strcmp (tok, "r")) rad = strtof (eq + 1, NULL);
              else if (!strcmp (tok, "f")) fe = strtof (eq + 1, NULL);
              else if (!strcmp (tok, "p"))
                n = parse_points (eq + 1, xy, RECIPE_STROKE_MAX);
            }
          if (!n)
            continue;
          if (!paint)
            {
              /* A stroke from before masks: a brush mask of its own, giving
                 what it gave (2 stops at 1; double or no colour at 1). */
              recipe_mask mk;
              recipe_mask_init (&mk, MASK_BRUSH);
              if (kind <= 1)
                recipe_mask_set (&mk, M_EV, (kind == 0 ? 2.0f : -2.0f) * clamp_unit (amt));
              else
                recipe_mask_set (&mk, M_SATURATION, (kind == 2 ? 100.0f : -100.0f) * clamp_unit (amt));
              if ((m = recipe_mask_add (r, &mk)) < 0)
                continue;
              kind = BRUSH_ADD;
              amt = 1;
            }
          if (m >= 0 && m < r->nmasks)
            recipe_stroke_add (r, m, kind, rad, fe, amt, xy, n);
          continue;
        }
      if (strcmp (tok, "mask") == 0)
        {
          recipe_mask mk;
          int kind = MASK_BRUSH, k;
          recipe_mask_init (&mk, MASK_BRUSH);
          while ((tok = strtok_r (NULL, " \t\r", &save)) != NULL)
            {
              char *eq = strchr (tok, '=');
              float v;
              if (!eq)
                continue;
              *eq = '\0';
              v = strtof (eq + 1, NULL);
              if (!strcmp (tok, "k"))
                {
                  for (k = 0; k < MASK_KINDS; k++)
                    if (!strcmp (eq + 1, recipe_mask_keys[k]))
                      kind = k;
                  continue;
                }
              if (!strcmp (tok, "x0")) mk.x0 = v;
              else if (!strcmp (tok, "y0")) mk.y0 = v;
              else if (!strcmp (tok, "x1")) mk.x1 = v;
              else if (!strcmp (tok, "y1")) mk.y1 = v;
              else if (!strcmp (tok, "f")) mk.f = clamp_unit (v);
              else if (!strcmp (tok, "invert")) mk.invert = v != 0;
              else if (!strcmp (tok, "off")) mk.off = v != 0;
              else
                for (k = 0; k < M_COUNT; k++)
                  if (!strcmp (tok, recipe_mask_params[k].key))
                    recipe_mask_set (&mk, k, v);
            }
          mk.kind = kind;
          recipe_mask_add (r, &mk);
          continue;
        }
      if (strcmp (tok, "spot") == 0)
        {
          recipe_spot sp = { 0.5f, 0.5f, 0.01f, 0.5f, 0.5f, 0, 0.3f, 0, 0 };
          float xy[RECIPE_STROKE_MAX * 2];
          int n = 0;
          while ((tok = strtok_r (NULL, " \t\r", &save)) != NULL)
            {
              char *eq = strchr (tok, '=');
              float v;
              if (!eq)
                continue;
              *eq = '\0';
              if (!strcmp (tok, "p"))
                {
                  n = parse_points (eq + 1, xy, RECIPE_STROKE_MAX);
                  continue;
                }
              v = strtof (eq + 1, NULL);
              if (!strcmp (tok, "x")) sp.x = v;
              else if (!strcmp (tok, "y")) sp.y = v;
              else if (!strcmp (tok, "r")) sp.r = v;
              else if (!strcmp (tok, "sx")) sp.sx = v;
              else if (!strcmp (tok, "sy")) sp.sy = v;
              else if (!strcmp (tok, "clone")) sp.clone = v != 0;
              else if (!strcmp (tok, "f")) sp.f = v;
            }
          if (n >= 2)
            recipe_spot_add_path (r, &sp, xy, n);
          else
            recipe_spot_add (r, &sp);
          continue;
        }
      if (strcmp (tok, "curves") == 0)
        {
          while ((tok = strtok_r (NULL, " \t\r", &save)) != NULL)
            {
              char *eq = strchr (tok, '='), *pt, *s2 = NULL;
              int k;
              if (!eq)
                continue;
              *eq = '\0';
              for (k = 0; k < CURVE_COUNT; k++)
                if (strcmp (tok, curve_keys[k]) == 0)
                  {
                    recipe_curve *c = &r->curve[k];
                    c->n = 0;
                    for (pt = strtok_r (eq + 1, ",", &s2); pt && c->n < CURVE_POINTS; pt = strtok_r (NULL, ",", &s2))
                      {
                        float x = 0, y = 0;
                        if (sscanf (pt, "%f:%f", &x, &y) == 2 && x >= 0 && x <= 1 && y >= 0 && y <= 1
                            && (c->n == 0 || x > c->x[c->n - 1]))
                          {
                            c->x[c->n] = x;
                            c->y[c->n] = y;
                            c->n++;
                          }
                      }
                    if (c->n < 2)
                      c->n = 0;
                  }
            }
          continue;
        }
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

int recipe_geometry_is_default (const recipe *r)
{
  int i;
  for (i = P_FIRST_GEOMETRY; i < P_COUNT; i++)
    if (!same (r->v[i], recipe_params[i].def))
      return 0;
  return !r->lens.on;
}

void recipe_copy_look (recipe *dst, const recipe *src)
{
  int i;
  for (i = 0; i < P_FIRST_GEOMETRY; i++)
    dst->v[i] = src->v[i];
  for (i = 0; i < CURVE_COUNT; i++)
    dst->curve[i] = src->curve[i];
}

/* --- spots */
int recipe_spots_equal (const recipe *a, const recipe *b)
{
  int i;
  if (a->nspots != b->nspots)
    return 0;
  for (i = 0; i < a->nspots; i++)
    {
      const recipe_spot *p = &a->spot[i], *q = &b->spot[i];
      if (!same (p->x, q->x) || !same (p->y, q->y) || !same (p->r, q->r) || !same (p->sx, q->sx) || !same (p->sy, q->sy)
          || p->clone != q->clone || !same (p->f, q->f) || p->n != q->n)
        return 0;
      if (p->n > 1 && memcmp (a->spt[p->first], b->spt[q->first], sizeof a->spt[0] * (size_t) p->n))
        return 0;
    }
  return 1;
}

int recipe_spot_add (recipe *r, const recipe_spot *sp)
{
  recipe_spot *d;
  if (r->nspots >= RECIPE_SPOTS)
    return -1;
  d = &r->spot[r->nspots];
  d->x = clamp_unit (sp->x);
  d->y = clamp_unit (sp->y);
  d->sx = clamp_unit (sp->sx);
  d->sy = clamp_unit (sp->sy);
  d->r = sp->r < 0.001f ? 0.001f : sp->r > 0.25f ? 0.25f : sp->r;
  d->clone = sp->clone ? 1 : 0;
  d->f = clamp_unit (sp->f);
  d->first = r->nspoints;
  d->n = 0;
  return r->nspots++;
}

int recipe_spot_add_path (recipe *r, const recipe_spot *sp, const float *xy, int n)
{
  recipe_spot s2 = *sp;
  int i, j;
  if (n > RECIPE_STROKE_MAX)
    n = RECIPE_STROKE_MAX;
  if (n < 2)
    {
      if (n == 1)
        {
          s2.sx += xy[0] - s2.x; s2.sy += xy[1] - s2.y;
          s2.x = xy[0]; s2.y = xy[1];
        }
      return recipe_spot_add (r, &s2);
    }
  if (r->nspots >= RECIPE_SPOTS || r->nspoints + n > RECIPE_SPOT_POINTS)
    return -1;
  /* The source keeps its offset from the first point. */
  s2.sx = xy[0] + (sp->sx - sp->x);
  s2.sy = xy[1] + (sp->sy - sp->y);
  s2.x = xy[0];
  s2.y = xy[1];
  if ((i = recipe_spot_add (r, &s2)) < 0)
    return -1;
  r->spot[i].first = r->nspoints;
  r->spot[i].n = n;
  for (j = 0; j < n; j++)
    {
      r->spt[r->nspoints + j][0] = (uint16_t) lroundf (clamp_unit (xy[2 * j]) * 10000);
      r->spt[r->nspoints + j][1] = (uint16_t) lroundf (clamp_unit (xy[2 * j + 1]) * 10000);
    }
  r->nspoints += n;
  r->spot[i].x = r->spt[r->spot[i].first][0] / 10000.0f;
  r->spot[i].y = r->spt[r->spot[i].first][1] / 10000.0f;
  return i;
}

void recipe_spot_point (const recipe *r, int i, int j, float *u, float *v)
{
  const recipe_spot *sp = &r->spot[i];
  if (sp->n < 2)
    {
      *u = sp->x;
      *v = sp->y;
      return;
    }
  j = j < 0 ? 0 : j >= sp->n ? sp->n - 1 : j;
  *u = r->spt[sp->first + j][0] / 10000.0f;
  *v = r->spt[sp->first + j][1] / 10000.0f;
}

void recipe_spot_move (recipe *r, int i, float du, float dv, int only_source)
{
  recipe_spot *sp;
  int j;
  if (i < 0 || i >= r->nspots)
    return;
  sp = &r->spot[i];
  sp->sx = clamp_unit (sp->sx + du);
  sp->sy = clamp_unit (sp->sy + dv);
  if (only_source)
    return;
  if (sp->n >= 2)
    {
      /* The whole path, kept on the photo (as a whole). */
      float lo_u = 1, hi_u = 0, lo_v = 1, hi_v = 0;
      for (j = 0; j < sp->n; j++)
        {
          float u = r->spt[sp->first + j][0] / 10000.0f, v = r->spt[sp->first + j][1] / 10000.0f;
          lo_u = u < lo_u ? u : lo_u; hi_u = u > hi_u ? u : hi_u;
          lo_v = v < lo_v ? v : lo_v; hi_v = v > hi_v ? v : hi_v;
        }
      du = lo_u + du < 0 ? -lo_u : hi_u + du > 1 ? 1 - hi_u : du;
      dv = lo_v + dv < 0 ? -lo_v : hi_v + dv > 1 ? 1 - hi_v : dv;
      for (j = 0; j < sp->n; j++)
        {
          r->spt[sp->first + j][0] = (uint16_t) lroundf (clamp_unit (r->spt[sp->first + j][0] / 10000.0f + du) * 10000);
          r->spt[sp->first + j][1] = (uint16_t) lroundf (clamp_unit (r->spt[sp->first + j][1] / 10000.0f + dv) * 10000);
        }
      sp->x = r->spt[sp->first][0] / 10000.0f;
      sp->y = r->spt[sp->first][1] / 10000.0f;
      return;
    }
  sp->x = clamp_unit (sp->x + du);
  sp->y = clamp_unit (sp->y + dv);
}

/* --- masks */
const char *const recipe_mask_keys[MASK_KINDS] = { "brush", "gradient", "radial" };

const recipe_param recipe_mask_params[M_COUNT] =
{
  { "mask", "ev",         "Exposure",    -4,   4, 0, 0.05f, 2, 0 },
  { "mask", "contrast",   "Contrast",  -100, 100, 0, 1,     0, 0 },
  { "mask", "highlights", "Highlights", -100, 100, 0, 1,    0, 0 },
  { "mask", "shadows",    "Shadows",   -100, 100, 0, 1,     0, 0 },
  { "mask", "temp",       "Temperature", -100, 100, 0, 1,   0, 0 },
  { "mask", "tint",       "Tint",      -100, 100, 0, 1,     0, 0 },
  { "mask", "saturation", "Saturation", -100, 100, 0, 1,    0, 0 },
  { "mask", "clarity",    "Clarity",   -100, 100, 0, 1,     0, 0 },
  { "mask", "sharpness",  "Sharpness", -100, 100, 0, 1,     0, 0 },
};

void recipe_mask_init (recipe_mask *m, int kind)
{
  memset (m, 0, sizeof *m);
  m->kind = kind < 0 || kind >= MASK_KINDS ? MASK_BRUSH : kind;
  m->f = 0.5f;
  if (m->kind == MASK_GRADIENT)
    {
      m->x0 = 0.5f; m->y0 = 0.0f; m->x1 = 0.5f; m->y1 = 0.5f;   /* the top half, as for a sky */
    }
  else if (m->kind == MASK_RADIAL)
    {
      m->x0 = 0.5f; m->y0 = 0.5f; m->x1 = 0.25f; m->y1 = 0.25f;
    }
}

void recipe_mask_set (recipe_mask *m, int p, float value)
{
  const recipe_param *d;
  if (p < 0 || p >= M_COUNT)
    return;
  d = &recipe_mask_params[p];
  if (value < d->min) value = d->min;
  if (value > d->max) value = d->max;
  value = d->min + roundf ((value - d->min) / d->step) * d->step;
  if (value > d->max)
    value = d->max;
  if (same (value, 0))
    value = 0;
  m->v[p] = value;
}

int recipe_mask_add (recipe *r, const recipe_mask *m)
{
  recipe_mask *d;
  int p;
  if (r->nmasks >= RECIPE_MASKS)
    return -1;
  d = &r->mask[r->nmasks];
  *d = *m;
  d->kind = m->kind < 0 || m->kind >= MASK_KINDS ? MASK_BRUSH : m->kind;
  d->invert = m->invert ? 1 : 0;
  d->off = m->off ? 1 : 0;
  d->f = clamp_unit (m->f);
  for (p = 0; p < M_COUNT; p++)
    recipe_mask_set (d, p, m->v[p]);
  if (d->kind == MASK_RADIAL)
    {
      d->x1 = d->x1 < 0.005f ? 0.005f : d->x1 > 2 ? 2 : d->x1;
      d->y1 = d->y1 < 0.005f ? 0.005f : d->y1 > 2 ? 2 : d->y1;
    }
  return r->nmasks++;
}

void recipe_mask_remove (recipe *r, int i)
{
  int j;
  if (i < 0 || i >= r->nmasks)
    return;
  for (j = r->nstrokes - 1; j >= 0; j--)
    if (r->stroke[j].mask == i)
      recipe_stroke_remove (r, j);
  for (j = 0; j < r->nstrokes; j++)
    if (r->stroke[j].mask > i)
      r->stroke[j].mask--;
  memmove (&r->mask[i], &r->mask[i + 1], sizeof r->mask[0] * (size_t) (r->nmasks - i - 1));
  r->nmasks--;
}

int recipe_masks_equal (const recipe *a, const recipe *b)
{
  int i, p;
  if (a->nmasks != b->nmasks)
    return 0;
  for (i = 0; i < a->nmasks; i++)
    {
      const recipe_mask *m = &a->mask[i], *n = &b->mask[i];
      if (m->kind != n->kind || m->invert != n->invert || m->off != n->off || !same (m->x0, n->x0) || !same (m->y0, n->y0)
          || !same (m->x1, n->x1) || !same (m->y1, n->y1) || !same (m->f, n->f))
        return 0;
      for (p = 0; p < M_COUNT; p++)
        if (!same (m->v[p], n->v[p]))
          return 0;
    }
  return 1;
}

/* --- strokes */
const char *const recipe_brush_keys[BRUSH_KINDS] = { "add", "erase" };

int recipe_stroke_add (recipe *r, int mask, int kind, float radius, float feather, float amount, const float *xy, int n)
{
  recipe_stroke *st;
  int j;
  if (n > RECIPE_STROKE_MAX)
    n = RECIPE_STROKE_MAX;
  if (n <= 0 || mask < 0 || mask >= r->nmasks || r->nstrokes >= RECIPE_STROKES || r->npoints + n > RECIPE_STROKE_POINTS)
    return -1;
  st = &r->stroke[r->nstrokes];
  st->mask = mask;
  st->kind = kind < 0 || kind >= BRUSH_KINDS ? BRUSH_ADD : kind;
  st->r = radius < 0.001f ? 0.001f : radius > 0.25f ? 0.25f : radius;
  st->f = clamp_unit (feather);
  st->amount = clamp_unit (amount);
  st->first = r->npoints;
  st->n = n;
  for (j = 0; j < n; j++)
    {
      r->pt[r->npoints + j][0] = (uint16_t) lroundf (clamp_unit (xy[2 * j]) * 10000);
      r->pt[r->npoints + j][1] = (uint16_t) lroundf (clamp_unit (xy[2 * j + 1]) * 10000);
    }
  r->npoints += n;
  return r->nstrokes++;
}

void recipe_stroke_remove (recipe *r, int i)
{
  int j, first, n;
  if (i < 0 || i >= r->nstrokes)
    return;
  first = r->stroke[i].first;
  n = r->stroke[i].n;
  memmove (&r->pt[first], &r->pt[first + n], sizeof r->pt[0] * (size_t) (r->npoints - first - n));
  r->npoints -= n;
  memmove (&r->stroke[i], &r->stroke[i + 1], sizeof r->stroke[0] * (size_t) (r->nstrokes - i - 1));
  r->nstrokes--;
  for (j = i; j < r->nstrokes; j++)
    r->stroke[j].first -= n;
}

int recipe_strokes_equal (const recipe *a, const recipe *b)
{
  int i;
  if (a->nstrokes != b->nstrokes || a->npoints != b->npoints)
    return 0;
  for (i = 0; i < a->nstrokes; i++)
    {
      const recipe_stroke *p = &a->stroke[i], *q = &b->stroke[i];
      if (p->kind != q->kind || p->mask != q->mask || p->n != q->n || p->first != q->first || !same (p->r, q->r) || !same (p->f, q->f)
          || !same (p->amount, q->amount))
        return 0;
    }
  return !memcmp (a->pt, b->pt, sizeof a->pt[0] * (size_t) a->npoints);
}

void recipe_spot_remove (recipe *r, int i)
{
  int j, first, n;
  if (i < 0 || i >= r->nspots)
    return;
  first = r->spot[i].first;
  n = r->spot[i].n >= 2 ? r->spot[i].n : 0;
  if (n)
    {
      memmove (&r->spt[first], &r->spt[first + n], sizeof r->spt[0] * (size_t) (r->nspoints - first - n));
      r->nspoints -= n;
    }
  memmove (&r->spot[i], &r->spot[i + 1], sizeof r->spot[0] * (size_t) (r->nspots - i - 1));
  r->nspots--;
  for (j = 0; j < r->nspots; j++)
    if (n && r->spot[j].n >= 2 && r->spot[j].first > first)
      r->spot[j].first -= n;
}

int recipe_format_look (const recipe *r, char *out, size_t n)
{
  recipe look;
  recipe_default (&look);
  recipe_copy_look (&look, r);
  return recipe_format (&look, out, n);
}

/* " p=4120,3310;4180,3325..." for n points: its length, or -1. */
static int format_points (char *out, size_t room, const uint16_t (*pt)[2], int n)
{
  size_t used = 0;
  int j, k = snprintf (out, room, " p=");
  if (k < 0 || (size_t) k >= room)
    return -1;
  used = (size_t) k;
  for (j = 0; j < n; j++)
    {
      k = snprintf (out + used, room - used, "%s%u,%u", j ? ";" : "", pt[j][0], pt[j][1]);
      if (k < 0 || used + (size_t) k >= room)
        return -1;
      used += (size_t) k;
    }
  return (int) used;
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
      if (recipe_groups[g].curves)
        {
          int c, i, started = 0;
          for (c = 0; c < CURVE_COUNT; c++)
            {
              const recipe_curve *cv = &r->curve[c];
              if (recipe_curve_is_identity (cv))
                continue;
              k = snprintf (out + used, n - used, "%s %s=", started ? "" : "curves", curve_keys[c]);
              if (k < 0 || used + (size_t) k >= n)
                return -1;
              /* "curves rgb=" for the first, " red=" after */
              used += (size_t) k;
              started = 1;
              for (i = 0; i < cv->n; i++)
                {
                  k = snprintf (out + used, n - used, "%s%.3f:%.3f", i ? "," : "", (double) cv->x[i], (double) cv->y[i]);
                  if (k < 0 || used + (size_t) k >= n)
                    return -1;
                  used += (size_t) k;
                }
            }
          if (started)
            {
              if (used + 2 > n)
                return -1;
              out[used++] = '\n';
              out[used] = '\0';
            }
          continue;
        }
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
          if (recipe_groups[g].hsl && same (r->v[p], recipe_params[p].def))
            continue;                  /* 24 values: only those changed */
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
  if ((k = format_lens (&r->lens, out + used, n - used)) < 0)
    return -1;
  used += (size_t) k;
  {
    int i;
    for (i = 0; i < r->nspots; i++)
      {
        const recipe_spot *p = &r->spot[i];
        int k = snprintf (out + used, n - used, "spot x=%.4f y=%.4f r=%.4f sx=%.4f sy=%.4f clone=%d f=%.2f",
                             (double) p->x, (double) p->y, (double) p->r, (double) p->sx, (double) p->sy, p->clone,
                             (double) p->f);
        if (k < 0 || used + (size_t) k >= n)
          return -1;
        used += (size_t) k;
        if (p->n >= 2 && (k = format_points (out + used, n - used, (const uint16_t (*)[2]) &r->spt[p->first], p->n)) < 0)
          return -1;
        used += p->n >= 2 ? (size_t) k : 0;
        if (used + 2 > n)
          return -1;
        out[used++] = '\n';
        out[used] = '\0';
      }
    for (i = 0; i < r->nmasks; i++)
      {
        const recipe_mask *m = &r->mask[i];
        int q, k = snprintf (out + used, n - used, "mask k=%s%s", recipe_mask_keys[m->kind], m->off ? " off=1" : "");
        if (k < 0 || used + (size_t) k >= n)
          return -1;
        used += (size_t) k;
        if (m->kind != MASK_BRUSH)
          {
            k = snprintf (out + used, n - used, " x0=%.4f y0=%.4f x1=%.4f y1=%.4f f=%.2f%s", (double) m->x0, (double) m->y0,
                          (double) m->x1, (double) m->y1, (double) m->f, m->invert ? " invert=1" : "");
            if (k < 0 || used + (size_t) k >= n)
              return -1;
            used += (size_t) k;
          }
        for (q = 0; q < M_COUNT; q++)
          if (!same (m->v[q], 0))
            {
              k = snprintf (out + used, n - used, " %s=%.*f", recipe_mask_params[q].key, recipe_mask_params[q].decimals,
                            (double) m->v[q]);
              if (k < 0 || used + (size_t) k >= n)
                return -1;
              used += (size_t) k;
            }
        if (used + 2 > n)
          return -1;
        out[used++] = '\n';
        out[used] = '\0';
      }
    for (i = 0; i < r->nstrokes; i++)
      {
        const recipe_stroke *st = &r->stroke[i];
        int k = snprintf (out + used, n - used, "paint m=%d k=%s a=%.2f r=%.4f f=%.2f", st->mask, recipe_brush_keys[st->kind],
                          (double) st->amount, (double) st->r, (double) st->f);
        if (k < 0 || used + (size_t) k >= n)
          return -1;
        used += (size_t) k;
        if ((k = format_points (out + used, n - used, (const uint16_t (*)[2]) &r->pt[st->first], st->n)) < 0)
          return -1;
        used += (size_t) k;
        if (used + 2 > n)
          return -1;
        out[used++] = '\n';
        out[used] = '\0';
      }
  }
  return (int) used;
}
