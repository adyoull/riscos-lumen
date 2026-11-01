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
  { "levels",   "black",      "Black point",    0,  25, 0, 0.5f, 1, 0 },
  { "levels",   "white",      "White point",   75, 100, 100, 0.5f, 1, 0 },
  { "levels",   "mid",        "Midtones",    -100, 100, 0, 1, 0, 0 },
  { "enhance",  "contrast",   "Contrast",    -100, 100, 0, 1, 0, 0 },
  { "enhance",  "saturation", "Saturation",  -100, 100, 0, 1, 0, 0 },
  { "enhance",  "vibrance",   "Vibrancy",    -100, 100, 0, 1, 0, 0 },
  { "sharpen",  "amount",     "Amount",         0, 200, 0, 1, 0, 0 },
  { "sharpen",  "radius",     "Radius",       0.3,   3, 1, 0.1f, 1, 0 },
  { "geometry", "rotate",     "Rotate",         0,   3, 0, 1, 0, 1 },
  { "geometry", "angle",      "Straighten",   -15,  15, 0, 0.1f, 1, 0 },
  { "geometry", "vertical",   "Vertical",    -100, 100, 0, 1, 0, 0 },
  { "geometry", "horizontal", "Horizontal",  -100, 100, 0, 1, 0, 0 },
  { "crop",     "x0",         "Left",           0,   1, 0, 0.001f, 3, 1 },
  { "crop",     "y0",         "Top",            0,   1, 0, 0.001f, 3, 1 },
  { "crop",     "x1",         "Right",          0,   1, 1, 0.001f, 3, 1 },
  { "crop",     "y1",         "Bottom",         0,   1, 1, 0.001f, 3, 1 },
};

const recipe_group recipe_groups[] = {
  { "White Balance", P_TEMP, P_TINT, 1, 0 },
  { "Exposure", P_EV, P_EV, 1, 0 },
  { "Levels", P_BLACK, P_MID, 1, 0 },
  { "Curves", 1, 0, 1, 1 },            /* no sliders: the curves editor */
  { "Enhance", P_CONTRAST, P_VIBRANCE, 1, 0 },
  { "Sharpen", P_SHARPEN, P_SHARP_RADIUS, 1, 0 },
  { "Geometry", P_ROTATE, P_PERSP_H, 1, 0 },
  { "Crop", P_CROP_X0, P_CROP_Y1, 0, 0 },
};
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
  return 1;
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

int recipe_parse (recipe *r, const char *text)
{
  recipe_default (r);
  while (text && *text)
    {
      char line[1024], *tok, *save = NULL;
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
  return 1;
}

void recipe_copy_look (recipe *dst, const recipe *src)
{
  int i;
  for (i = 0; i < P_FIRST_GEOMETRY; i++)
    dst->v[i] = src->v[i];
  for (i = 0; i < CURVE_COUNT; i++)
    dst->curve[i] = src->curve[i];
}

int recipe_format_look (const recipe *r, char *out, size_t n)
{
  recipe look;
  recipe_default (&look);
  recipe_copy_look (&look, r);
  return recipe_format (&look, out, n);
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
