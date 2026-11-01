/* viewer.c: one photo, large, with the adjustments panel beside it.

   Opening a photo shows its thumbnail at once (enlarged), decodes a proxy
   (up to PROXY_SIDE pixels on the long side) in idle time, turns it into
   16-bit linear light, and renders it through the recipe a band of rows
   per slice. While a slider is dragged, a half-size copy is rendered
   instead, every time the value changes, so the photo follows the pointer;
   the full proxy is rendered again when the button is let go. Every
   change is saved to the catalogue as it is made (and can be undone), and
   the thumbnail is made again in the background.

   Geometry (quarter turns, straightening, perspective, crop) comes first:
   the proxy goes through geometry.c into a second linear image, which the
   tone stages then render. That image is only made again when the
   geometry changes; when there is none, it is the proxy itself.

   The crop tool shows the whole straightened picture with the crop
   rectangle over it (outside it darkened); its corners and edges drag,
   and inside it moves. Return applies, Escape puts the old crop back.

   Layout, in work area coordinates (origin top left, y down is negative):
   the photo fills the left of the window on a dark ground; the panel is
   PANEL_W wide on the right: histogram, the photo's place and rating,
   a row of tools (Auto, Crop, Grid, Presets), the recipe's groups of
   sliders (recipe_groups; hidden parameters have no slider), then Undo,
   Redo and Reset all. */
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <kernel.h>
#include <swis.h>
#include "autoadj.h"
#include "develop.h"
#include "geometry.h"
#include "recipe.h"
#include "thumbgen.h"
#include "ui.h"

#define PROXY_SIDE 1600
#define BAND_ROWS 32
#define UNDO_MAX 64

#define PANEL_W 640
#define PAD 24
#define HIST_H 150
#define INFO_H 52
#define TOOLS_H 84                     /* the tool buttons and the space under them */
#define GROUP_H 52
#define ROW_H 56
#define BUTTON_H 60
#define HANDLE 36                      /* how near a crop corner or edge a click must be */
#define LINK_GAP 28

#define COL_GROUND 0x2A2A2A00u
#define COL_PANEL 0xE2E2E200u
#define COL_TEXT 0x1A1A1A00u
#define COL_QUIET 0x55555500u
#define COL_LIGHT 0xDDDDDD00u
#define COL_TRACK 0xA8A8A800u
#define COL_ACCENT 0x166AC000u
#define COL_HIST_BG 0x1F1F1F00u
#define COL_HIST 0xC8C8C800u
#define COL_KNOB 0xFFFFFF00u
#define COL_FRAME 0x33333300u
#define COL_BUTTON 0xF5F5F500u
#define COL_STAR_OFF 0x9A9A9A00u
#define COL_GRID 0xC8C8C800u
#define COL_CROP 0xFFFFFF00u

enum { V_CLOSED, V_LOADING, V_READY, V_FAILED };
enum { HIT_NONE, HIT_SLIDER, HIT_LINK, HIT_UNDO, HIT_REDO, HIT_RESET_ALL, HIT_TOOL, HIT_IMAGE };
enum { TOOL_AUTO, TOOL_CROP, TOOL_GRID, TOOL_PRESETS, TOOL_COUNT };
enum { LINK_RESET, LINK_PICK, LINK_AUTO_WB, LINK_LEFT, LINK_RIGHT };
/* Crop drags: which edges move (or all of it). */
enum { E_L = 1, E_R = 2, E_T = 4, E_B = 8, E_MOVE = 16, E_NEW = 32 };
#define DRAG_CROP (-2)

typedef struct { void *area; uint32_t *pixels; int w, h; } sprite32;

static int win = -1, is_open, state = V_CLOSED;
static char title[200], err[200];
static int index_in_list = -1;
static cat_item item;
static int vis_w = 2400, vis_h = 1500;
static int screen_w = 3840, screen_h = 2160;

static thumbgen *tg;
static uint16_t *src, *half;           /* linear proxy, and half size */
static int sw, sh, hw, hh;
static uint16_t *geo, *geo_half;       /* after geometry (the proxy itself when there is none) */
static int gw, gh, ghw, ghh, geo_own, geo_pending;
static geom_plan plan_full, plan_half;
static float geo_key[P_COUNT - P_FIRST_GEOMETRY + 2];
static int geo_key_valid;
static sprite32 full_spr, half_spr;
static int show_half;                  /* displaying the half-size render */
static int full_y = -1;                /* next band of the full render; -1 when done */
static unsigned hist[64], hist_acc[64];

static develop *dev;
static recipe current, undo_stack[UNDO_MAX];
static int undo_n, undo_i;
static int before;                     /* showing the original */
static int dragging = -1, drag_x = -1, drag_y = -1; /* the parameter (or DRAG_CROP), the pointer's last place */

static int crop_mode, crop_edges, crop_shape, grid_mode, picking;
static recipe crop_saved;
static double crop_ax, crop_ay, crop_start[4], crop_px, crop_py;

static const char *const shape_names[6] = { "Free", "Original", "Square", "3:2", "4:3", "16:9" };
static const char *const grid_names[4] = { "None", "Thirds", "Quarters", "Fine" };

int viewer_window (void) { return win; }
int viewer_is_open (void) { return is_open; }

/* --- sprites */
static int sprite_make (sprite32 *s, int w, int h)
{
  size_t bytes = 16 + 44 + (size_t) w * h * 4;
  unsigned *a = malloc (bytes), *sp;
  if (!a)
    return -1;
  a[0] = (unsigned) bytes; a[1] = 1; a[2] = 16; a[3] = (unsigned) bytes;
  sp = a + 4;
  sp[0] = (unsigned) (44 + (size_t) w * h * 4);
  memset (sp + 1, 0, 12);
  memcpy (sp + 1, "photo", 5);
  sp[4] = (unsigned) (w - 1); sp[5] = (unsigned) (h - 1);
  sp[6] = 0; sp[7] = 31; sp[8] = 44; sp[9] = 44;
  sp[10] = 0x301680B5u;                /* 32bpp, 90 dpi */
  s->area = a;
  s->pixels = (uint32_t *) (sp + 11);
  s->w = w; s->h = h;
  return 0;
}

static void sprite_free (sprite32 *s)
{
  free (s->area);
  memset (s, 0, sizeof *s);
}

/* A sprite of exactly w x h, keeping the old one if it already is. */
static int sprite_size (sprite32 *s, int w, int h)
{
  if (s->area && s->w == w && s->h == h)
    return 0;
  sprite_free (s);
  return sprite_make (s, w, h);
}

static void free_geo (void)
{
  if (geo_own)
    {
      free (geo);
      free (geo_half);
    }
  geo = geo_half = NULL;
  geo_own = 0;
  geo_key_valid = 0;
  geo_pending = 0;
}

static void free_photo (void)
{
  thumbgen_free (tg);
  tg = NULL;
  free_geo ();
  free (src); free (half);
  src = half = NULL;
  sprite_free (&full_spr);
  sprite_free (&half_spr);
  full_y = -1;
  show_half = 0;
}

/* --- layout: everything in work area coordinates */
static int panel_x0 (void) { return vis_w - PANEL_W; }
static int track_x0 (void) { return panel_x0 () + 232; }
static int track_x1 (void) { return panel_x0 () + PANEL_W - 132; }

static void image_rect (int r[4])
{
  r[0] = 0; r[1] = -vis_h; r[2] = panel_x0 (); r[3] = 0;
}

/* Top of each part of the panel. */
static int hist_top (void) { return -PAD; }
static int info_top (void) { return hist_top () - HIST_H - 16; }
static int tools_top (void) { return info_top () - INFO_H; }
static int groups_top (void) { return tools_top () - TOOLS_H; }

static int group_shown (int g) { return recipe_groups[g].panel; }

static int group_rows (int g)
{
  int p, n = 0;
  for (p = recipe_groups[g].first; p <= recipe_groups[g].last; p++)
    if (!recipe_params[p].hidden)
      n++;
  return n;
}

/* The top of group g's header, and of parameter p's row (0 if it has none). */
static int group_top (int g)
{
  int y = groups_top (), k;
  for (k = 0; k < g; k++)
    if (group_shown (k))
      y -= GROUP_H + ROW_H * group_rows (k) + 8;
  return y;
}

static int row_top (int p)
{
  int g, q, i;
  if (recipe_params[p].hidden)
    return 0;
  for (g = 0; g < recipe_ngroups; g++)
    if (p >= recipe_groups[g].first && p <= recipe_groups[g].last)
      {
        if (!group_shown (g))
          return 0;
        for (i = 0, q = recipe_groups[g].first; q < p; q++)
          if (!recipe_params[q].hidden)
            i++;
        return group_top (g) - GROUP_H - ROW_H * i;
      }
  return 0;
}

static int buttons_top (void)
{
  return group_top (recipe_ngroups) - 16;
}

/* Button k of n across the panel, its top at top. */
static void button_at (int k, int n, int top, int r[4])
{
  int w = (PANEL_W - 2 * PAD - (n - 1) * 16) / n;
  r[0] = panel_x0 () + PAD + k * (w + 16);
  r[2] = r[0] + w;
  r[3] = top;
  r[1] = r[3] - BUTTON_H;
}

static void button_rect (int k, int r[4]) { button_at (k, 3, buttons_top (), r); }
static void tool_rect (int k, int r[4]) { button_at (k, TOOL_COUNT, tools_top (), r); }

/* The links on a group's header, right to left: Reset, then the group's own. */
static int group_links (int g, int ids[3], const char *names[3])
{
  int n = 0;
  ids[n] = LINK_RESET; names[n++] = "Reset";
  if (recipe_groups[g].first == P_TEMP)
    {
      ids[n] = LINK_AUTO_WB; names[n++] = "Auto";
      ids[n] = LINK_PICK; names[n++] = "Pick";
    }
  else if (recipe_groups[g].first == P_ROTATE)
    {
      ids[n] = LINK_RIGHT; names[n++] = "+90\xB0";
      ids[n] = LINK_LEFT; names[n++] = "-90\xB0";
    }
  return n;
}

/* Link k of group g: its x range. */
static void link_x (int g, int k, int *x0, int *x1)
{
  int ids[3], i, x = vis_w - PAD;
  const char *names[3];
  group_links (g, ids, names);
  for (i = 0; i <= k; i++)
    {
      *x1 = x;
      *x0 = x - draw_text_width (names[i]);
      x = *x0 - LINK_GAP;
    }
}

static int value_to_x (int p, float v)
{
  const recipe_param *d = &recipe_params[p];
  return track_x0 () + (int) ((v - d->min) / (d->max - d->min) * (float) (track_x1 () - track_x0 ()));
}

static float x_to_value (int p, int x)
{
  const recipe_param *d = &recipe_params[p];
  return d->min + (float) (x - track_x0 ()) / (float) (track_x1 () - track_x0 ()) * (d->max - d->min);
}

/* Where the photo is drawn (work area), as draw_sprite_rect places it. */
static int photo_rect (int out[4])
{
  int r[4], bw, bh, mul, div, dw, dh;
  image_rect (r);
  r[0] += PAD; r[1] += PAD; r[2] -= PAD; r[3] -= PAD;
  bw = r[2] - r[0]; bh = r[3] - r[1];
  if (!src || gw <= 0 || gh <= 0 || bw <= 0 || bh <= 0)
    return 0;
  if ((long long) bw * gh <= (long long) bh * gw)
    {
      mul = bw; div = gw;
    }
  else
    {
      mul = bh; div = gh;
    }
  dw = (int) ((long long) gw * mul / div);
  dh = (int) ((long long) gh * mul / div);
  out[0] = r[0] + (bw - dw) / 2;
  out[1] = r[1] + (bh - dh) / 2;
  out[2] = out[0] + dw;
  out[3] = out[1] + dh;
  return 1;
}

static int hit (int wx, int wy, int *what)
{
  int p, g, k, r[4];
  if (wx < panel_x0 ())
    return HIT_IMAGE;
  for (p = 0; p < P_COUNT; p++)
    {
      int top = row_top (p);
      if (top && wy <= top && wy > top - ROW_H && wx >= track_x0 () - 24 && wx <= track_x1 () + 24)
        {
          *what = p;
          return HIT_SLIDER;
        }
    }
  for (g = 0; g < recipe_ngroups; g++)
    {
      int top = group_top (g), ids[3], n;
      const char *names[3];
      if (!group_shown (g) || wy > top || wy <= top - GROUP_H)
        continue;
      n = group_links (g, ids, names);
      for (k = 0; k < n; k++)
        {
          int x0, x1;
          link_x (g, k, &x0, &x1);
          if (wx >= x0 - LINK_GAP / 2 && wx <= x1 + LINK_GAP / 2)
            {
              *what = g * 8 + ids[k];
              return HIT_LINK;
            }
        }
    }
  for (k = 0; k < TOOL_COUNT; k++)
    {
      tool_rect (k, r);
      if (wx >= r[0] && wx <= r[2] && wy >= r[1] && wy <= r[3])
        {
          *what = k;
          return HIT_TOOL;
        }
    }
  for (k = 0; k < 3; k++)
    {
      button_rect (k, r);
      if (wx >= r[0] && wx <= r[2] && wy >= r[1] && wy <= r[3])
        return HIT_UNDO + k;
    }
  return HIT_NONE;
}

/* --- window */
static void read_screen (void)
{
  int xl = 1919, yl = 1079;
  _swix (OS_ReadModeVariable, _INR (0, 1) | _OUT (2), -1, 11, &xl);
  _swix (OS_ReadModeVariable, _INR (0, 1) | _OUT (2), -1, 12, &yl);
  screen_w = (xl + 1) << draw_xeig ();
  screen_h = (yl + 1) << draw_yeig ();
}

int viewer_create (void)
{
  int b[23];
  read_screen ();
  memset (b, 0, sizeof b);
  b[0] = 120; b[1] = 180; b[2] = 120 + vis_w; b[3] = 180 + vis_h;
  b[6] = -1;
  /* moveable; back, close, title, toggle size, adjust size; new format */
  b[7] = (int) 0xAF000002;
  ((unsigned char *) b)[32] = 7;  ((unsigned char *) b)[33] = 2;
  ((unsigned char *) b)[34] = 7;  ((unsigned char *) b)[35] = 255;  /* we draw the background */
  ((unsigned char *) b)[36] = 3;  ((unsigned char *) b)[37] = 1;
  ((unsigned char *) b)[38] = 12;
  b[10] = 0; b[11] = -screen_h; b[12] = screen_w; b[13] = 0;
  b[14] = 0x13D;
  b[15] = 3 << 12;                     /* work area: click */
  b[16] = 1;
  b[17] = (1300 << 16) | 1200;         /* minimum height, width */
  snprintf (title, sizeof title, "%s", APP_NAME);
  b[18] = (int) title; b[19] = -1; b[20] = sizeof title;
  b[21] = 0;
  dev = malloc (sizeof *dev);
  if (!dev)
    return -1;
  return _swix (Wimp_CreateWindow, _IN (1) | _OUT (0), b, &win) ? -1 : 0;
}

static void force (int x0, int y0, int x1, int y1)
{
  if (is_open)
    _swix (Wimp_ForceRedraw, _INR (0, 4), win, x0, y0, x1, y1);
}

static void force_image (void)
{
  int r[4];
  image_rect (r);
  force (r[0], r[1], r[2], r[3]);
}

static void force_panel (void)
{
  force (panel_x0 (), -vis_h, vis_w, 0);
}

static void force_row (int p)
{
  if (row_top (p))
    force (panel_x0 (), row_top (p) - ROW_H, vis_w, row_top (p));
}

static void force_hist (void)
{
  force (panel_x0 (), hist_top () - HIST_H, vis_w, hist_top ());
}

static void update_title (void)
{
  char t[200];
  snprintf (t, sizeof t, "%s - %s (%d of %d)%s%s", APP_NAME, item.name, index_in_list + 1,
            browser_count (), recipe_is_default (&current) ? "" : "  \xB7  edited",
            crop_mode ? "  \xB7  cropping" : "");
  if (strcmp (t, title))
    {
      snprintf (title, sizeof title, "%s", t);
      if (is_open)
        _swix (Wimp_ForceRedraw, _INR (0, 2), win, TASK_WORD, 3);
    }
}

void viewer_open_window (int *block)
{
  int b[9];
  if (!block)
    {
      b[0] = win;
      if (is_open && _swix (Wimp_GetWindowState, _IN (1), b) == NULL)
        b[7] = -1;
      else
        {
          int w = screen_w - 160 < 3000 ? screen_w - 160 : 3000;
          int h = screen_h - 300;
          b[1] = (screen_w - w) / 2; b[2] = 160;
          b[3] = b[1] + w; b[4] = 160 + h;
          b[5] = 0; b[6] = 0; b[7] = -1;
        }
      block = b;
    }
  if (block[3] - block[1] != vis_w || block[4] - block[2] != vis_h)
    {
      vis_w = block[3] - block[1];
      vis_h = block[4] - block[2];
      force (0, -0x10000000, 0x10000000, 0);
    }
  block[5] = 0;                        /* never scrolled */
  block[6] = 0;
  _swix (Wimp_OpenWindow, _IN (1), block);
  if (!is_open)
    {
      is_open = 1;
      _swix (Wimp_SetCaretPosition, _INR (0, 5), win, -1, 0, 0, 1 << 25, -1);
    }
}

/* --- rendering */
static void effective (recipe *r)
{
  if (before)
    recipe_default (r);
  else
    *r = current;
}

static void prepare (void)
{
  recipe r;
  effective (&r);
  develop_prepare (dev, &r);
}

static int plan_identity (const geom_plan *g)
{
  return g->quarter == 0 && g->sn == 0 && g->kv == 0 && g->kh == 0
    && g->x0 == 0 && g->y0 == 0 && g->x1 == 1 && g->y1 == 1;
}

/* What the geometry image depends on; 1 if it changed since it was made. */
static int geometry_changed (void)
{
  float key[P_COUNT - P_FIRST_GEOMETRY + 2];
  recipe r;
  int p;
  effective (&r);
  for (p = P_FIRST_GEOMETRY; p < P_COUNT; p++)
    key[p - P_FIRST_GEOMETRY] = crop_mode && p >= P_CROP_X0 ? 0 : r.v[p];
  key[P_COUNT - P_FIRST_GEOMETRY] = (float) crop_mode;
  key[P_COUNT - P_FIRST_GEOMETRY + 1] = (float) sw;
  if (geo_key_valid && memcmp (key, geo_key, sizeof key) == 0)
    return 0;
  memcpy (geo_key, key, sizeof key);
  geo_key_valid = 1;
  return 1;
}

/* Makes the geometry plans, the half-size geometry image now, and marks
   the full one to be made with the next full render. */
static int geometry_apply (void)
{
  recipe r;
  effective (&r);
  geom_plan_make (&plan_full, &r, sw, sh, !crop_mode, 0, 0);
  geom_plan_make (&plan_half, &r, hw, hh, !crop_mode, 0, 0);
  if (geo_own)
    {
      free (geo);
      free (geo_half);
    }
  geo = geo_half = NULL;
  geo_own = 0;
  geo_pending = 0;
  if (plan_identity (&plan_full))
    {
      geo = src; geo_half = half;
      gw = sw; gh = sh; ghw = hw; ghh = hh;
    }
  else
    {
      gw = plan_full.ow; gh = plan_full.oh;
      ghw = plan_half.ow; ghh = plan_half.oh;
      geo = malloc ((size_t) gw * gh * 6);
      geo_half = malloc ((size_t) ghw * ghh * 6);
      geo_own = 1;
      if (!geo || !geo_half)
        goto no_memory;
      geom_rows (&plan_half, half, geo_half, 0, ghh);
      geo_pending = 1;
    }
  if (sprite_size (&full_spr, gw, gh) || sprite_size (&half_spr, ghw, ghh))
    goto no_memory;
  return 0;
no_memory:
  free_geo ();
  sprite_free (&full_spr);
  sprite_free (&half_spr);
  snprintf (err, sizeof err, "Not enough memory to show this photo.");
  state = V_FAILED;
  return -1;
}

/* In the crop tool, everything outside the crop is darkened. */
static void shade (uint32_t *pix, int w, int h, int y0, int y1)
{
  int x, y, cx0, cx1, cy0, cy1;
  if (!crop_mode)
    return;
  cx0 = (int) lround (current.v[P_CROP_X0] * w); cx1 = (int) lround (current.v[P_CROP_X1] * w);
  cy0 = (int) lround (current.v[P_CROP_Y0] * h); cy1 = (int) lround (current.v[P_CROP_Y1] * h);
  for (y = y0; y < y1; y++)
    {
      uint32_t *row = pix + (size_t) y * w;
      int inside = y >= cy0 && y < cy1;
      for (x = 0; x < w; x++)
        if (!inside || x < cx0 || x >= cx1)
          row[x] = (row[x] >> 2) & 0x3F3F3Fu;
    }
}

/* The half-size copy, all at once (it is a quarter of the pixels). */
static void render_half (void)
{
  if (!geo_half || !half_spr.area)
    return;
  memset (hist, 0, sizeof hist);
  develop_rows (dev, geo_half, ghw, 0, ghh, half_spr.pixels, hist);
  shade (half_spr.pixels, ghw, ghh, 0, ghh);
  show_half = 1;
  force_image ();
  force_hist ();
}

/* Starts the full proxy again from the top, in bands. */
static void restart_full (void)
{
  if (!geo)
    return;
  full_y = 0;
  memset (hist_acc, 0, sizeof hist_acc);
}

static void render_full_until (unsigned deadline)
{
  while (full_y >= 0 && full_y < gh)
    {
      int y1 = full_y + BAND_ROWS < gh ? full_y + BAND_ROWS : gh;
      if (geo_pending)
        geom_rows (&plan_full, src, geo, full_y, y1);
      develop_rows (dev, geo, gw, full_y, y1, full_spr.pixels, hist_acc);
      shade (full_spr.pixels, gw, gh, full_y, y1);
      full_y = y1;
      if ((int) (app_now () - deadline) >= 0)
        break;
    }
  if (full_y >= gh)
    {
      full_y = -1;
      geo_pending = 0;
      show_half = 0;
      memcpy (hist, hist_acc, sizeof hist);
      force_image ();
      force_hist ();
    }
}

/* Something changed: the geometry image if need be, then the half-size
   render now, and (if full) the full one in the background. */
static void refresh (int full)
{
  if (!src)
    return;
  if (geometry_changed () && geometry_apply ())
    {
      force_image ();
      return;
    }
  prepare ();
  render_half ();
  if (full)
    restart_full ();
}

/* Proxy decoded: linear copies and the half-size copy. */
static int proxy_ready (void)
{
  rgb_image img;
  int x, y;
  if (thumbgen_take (tg, &img))
    return -1;
  sw = img.w; sh = img.h;
  hw = sw / 2 > 0 ? sw / 2 : 1; hh = sh / 2 > 0 ? sh / 2 : 1;
  src = malloc ((size_t) sw * sh * 6);
  half = malloc ((size_t) hw * hh * 6);
  if (!src || !half)
    {
      free (img.rgb);
      snprintf (err, sizeof err, "Not enough memory to show this photo.");
      return -1;
    }
  dev_to_linear (img.rgb, src, (size_t) sw * sh);
  free (img.rgb);
  /* Half size: the mean of each 2x2 block, in linear light. */
  for (y = 0; y < hh; y++)
    for (x = 0; x < hw; x++)
      {
        int c;
        const uint16_t *a = src + ((size_t) (2 * y) * sw + 2 * x) * 3;
        const uint16_t *b = 2 * y + 1 < sh ? a + (size_t) sw * 3 : a;
        int dx = 2 * x + 1 < sw ? 3 : 0;
        for (c = 0; c < 3; c++)
          half[((size_t) y * hw + x) * 3 + c] = (uint16_t) ((a[c] + a[c + dx] + b[c] + b[c + dx] + 2) >> 2);
      }
  geo_key_valid = 0;
  if (geometry_changed () && geometry_apply ())
    return -1;
  prepare ();
  render_half ();                      /* something better than the thumbnail, straight away */
  restart_full ();
  return 0;
}

/* --- the photo */
static void load_recipe (void)
{
  char text[1024];
  cat_get_recipe (lib_cat (app_library ()), item.id, text, sizeof text);
  recipe_parse (&current, text);
  undo_stack[0] = current;
  undo_n = 1;
  undo_i = 0;
}

void viewer_show (int i)
{
  cat_details d;
  char e[200];
  if (!browser_item (i, &item))
    return;
  free_photo ();
  index_in_list = i;
  before = 0;
  dragging = -1;
  crop_mode = 0;
  picking = 0;
  err[0] = '\0';
  load_recipe ();
  state = V_LOADING;
  if (cat_get_details (lib_cat (app_library ()), item.id, &d))
    {
      state = V_FAILED;
      snprintf (err, sizeof err, "This photo is no longer in the library.");
    }
  else if (!(tg = thumbgen_start (d.path, NULL, PROXY_SIDE, d.orientation, e, sizeof e)))
    {
      state = V_FAILED;
      snprintf (err, sizeof err, "%s", e);
    }
  update_title ();
  force (0, -0x10000000, 0x10000000, 0);
}

void viewer_open (int i)
{
  viewer_show (i);
  viewer_open_window (NULL);
}

void viewer_close (void)
{
  int b[1];
  if (!is_open)
    return;
  if (crop_mode)
    viewer_crop_finish (1);
  b[0] = win;
  _swix (Wimp_CloseWindow, _IN (1), b);
  is_open = 0;
  dragging = -1;
  picking = 0;
  free_photo ();
  state = V_CLOSED;
  browser_focus_on (index_in_list);
}

int viewer_busy (void)
{
  return is_open && (dragging != -1 || (state == V_LOADING && tg) || full_y >= 0);
}

static void crop_follow (double fx, double fy);

/* Called on null events while busy. */
void viewer_idle (void)
{
  unsigned deadline = app_now () + 2;
  if (!is_open)
    return;
  if (dragging != -1)
    {
      int ptr[5], b[9];
      b[0] = win;
      if (_swix (Wimp_GetPointerInfo, _IN (1), ptr) == NULL && (ptr[0] != drag_x || ptr[1] != drag_y)
          && _swix (Wimp_GetWindowState, _IN (1), b) == NULL)
        {
          drag_x = ptr[0];
          drag_y = ptr[1];
          if (dragging == DRAG_CROP)
            {
              int r[4];
              if (photo_rect (r))
                crop_follow ((double) (ptr[0] - b[1] - r[0]) / (r[2] - r[0]),
                             (double) (r[3] - (ptr[1] - b[4])) / (r[3] - r[1]));
            }
          else
            {
              float old = current.v[dragging];
              recipe_set (&current, dragging, x_to_value (dragging, ptr[0] - b[1]));
              if (current.v[dragging] != old)
                {
                  refresh (0);
                  force_row (dragging);
                }
            }
        }
      return;
    }
  if (state == V_LOADING && tg)
    {
      int r;
      do
        r = thumbgen_step (tg, 32);
      while (r == 0 && (int) (app_now () - deadline) < 0);
      if (r < 0 || (r > 0 && proxy_ready ()))
        {
          if (r < 0)
            snprintf (err, sizeof err, "%s", thumbgen_error (tg));
          state = V_FAILED;
          free_photo ();
          force_image ();
        }
      else if (r > 0)
        {
          thumbgen_free (tg);
          tg = NULL;
          state = V_READY;
          force_panel ();              /* the tools come alive (found in the ROL 5.31-5 test) */
        }
      return;
    }
  if (full_y >= 0)
    render_full_until (deadline);
}

/* --- changes: saved at once, with undo */
static void save (void)
{
  char text[1024];
  recipe_format (&current, text, sizeof text);
  cat_set_recipe (lib_cat (app_library ()), item.id, text);
  lib_thumbs_changed (app_library ());
  browser_after_change ();
  update_title ();
}

static void commit (void)
{
  if (recipe_equal (&current, &undo_stack[undo_i]))
    return;
  if (undo_i == UNDO_MAX - 1)
    {
      memmove (undo_stack, undo_stack + 1, sizeof undo_stack[0] * (UNDO_MAX - 1));
      undo_i--;
    }
  undo_stack[++undo_i] = current;
  undo_n = undo_i + 1;
  save ();
  /* Undo, Redo, Reset all and the groups' Reset change with it (found in
     the ROL 5.31-5 test: only the slider's row was redrawn). */
  force_panel ();
}

static void changed_everything (void)
{
  before = 0;
  refresh (1);
  force_panel ();
}

void viewer_undo (void)
{
  if (undo_i == 0)
    return;
  current = undo_stack[--undo_i];
  save ();
  changed_everything ();
}

void viewer_redo (void)
{
  if (undo_i + 1 >= undo_n)
    return;
  current = undo_stack[++undo_i];
  save ();
  changed_everything ();
}

void viewer_reset_all (void)
{
  if (crop_mode)
    viewer_crop_finish (0);
  recipe_default (&current);
  commit ();
  changed_everything ();
}

void viewer_toggle_before (void)
{
  if (crop_mode)
    return;
  before = !before;
  refresh (1);
}

int viewer_can_undo (void) { return undo_i > 0; }
int viewer_can_redo (void) { return undo_i + 1 < undo_n; }
int viewer_before (void) { return before; }
const recipe *viewer_recipe (void) { return &current; }

/* A new recipe from outside (a preset, pasting, the library's menu). */
void viewer_reload (cat_id id)
{
  if (!is_open || id != item.id || crop_mode)
    return;
  {
    char text[1024];
    recipe r;
    cat_get_recipe (lib_cat (app_library ()), item.id, text, sizeof text);
    recipe_parse (&r, text);
    if (recipe_equal (&r, &current))
      return;
    current = r;
  }
  if (undo_i == UNDO_MAX - 1)
    {
      memmove (undo_stack, undo_stack + 1, sizeof undo_stack[0] * (UNDO_MAX - 1));
      undo_i--;
    }
  undo_stack[++undo_i] = current;
  undo_n = undo_i + 1;
  update_title ();
  changed_everything ();
}

void viewer_apply_look (const char *look)
{
  recipe r;
  if (!is_open || state == V_CLOSED)
    return;
  recipe_parse (&r, look);
  recipe_copy_look (&current, &r);
  commit ();
  changed_everything ();
}

/* The look as text (what a preset or Copy keeps). */
void viewer_look (char *out, size_t n)
{
  recipe_format_look (&current, out, n);
}

cat_id viewer_item (void) { return is_open ? item.id : 0; }

/* One adjustment, as if its slider were moved (scripts and the screenshot harness). */
void viewer_set (int p, float v)
{
  if (p < 0 || p >= P_COUNT)
    return;
  recipe_set (&current, p, v);
  commit ();
  changed_everything ();
}

/* --- the tools */
void viewer_auto (void)
{
  if (!src || crop_mode)
    return;
  auto_adjust (src, sw, sh, &current);
  commit ();
  changed_everything ();
}

static void auto_wb (void)
{
  float t, n;
  if (!src)
    return;
  auto_white_balance (src, sw, sh, &t, &n);
  recipe_set (&current, P_TEMP, t);
  recipe_set (&current, P_TINT, n);
  commit ();
  changed_everything ();
}

void viewer_pick_white (void)
{
  if (!src || crop_mode)
    return;
  picking = !picking;
  force_image ();
  force_panel ();
}

/* The eyedropper: (fx, fy) across the photo as shown. */
static void pick_at (double fx, double fy)
{
  double sx, sy, rgb[3];
  float t, n;
  picking = 0;
  geom_to_source (&plan_full, fx * gw, fy * gh, &sx, &sy);
  wb_sample (src, sw, sh, (int) sx, (int) sy, 3, rgb);
  /* The sample is the original's colour; the picked tone also has the
     current exposure etc., which white balance doesn't need. */
  if (wb_neutral (rgb[0], rgb[1], rgb[2], &t, &n) == 0)
    {
      recipe_set (&current, P_TEMP, t);
      recipe_set (&current, P_TINT, n);
      commit ();
    }
  changed_everything ();
  force_image ();
}

/* A quarter turn of r, its crop turning with the picture. */
static void turn (recipe *r, int clockwise)
{
  float x0 = r->v[P_CROP_X0], y0 = r->v[P_CROP_Y0], x1 = r->v[P_CROP_X1], y1 = r->v[P_CROP_Y1];
  r->v[P_ROTATE] = (float) (((int) r->v[P_ROTATE] + (clockwise ? 1 : 3)) & 3);
  if (clockwise)
    {
      r->v[P_CROP_X0] = 1 - y1; r->v[P_CROP_Y0] = x0; r->v[P_CROP_X1] = 1 - y0; r->v[P_CROP_Y1] = x1;
    }
  else
    {
      r->v[P_CROP_X0] = y0; r->v[P_CROP_Y0] = 1 - x1; r->v[P_CROP_X1] = y1; r->v[P_CROP_Y1] = 1 - x0;
    }
}

void viewer_rotate (int clockwise)
{
  if (!src)
    return;
  turn (&current, clockwise);
  if (crop_mode)
    turn (&crop_saved, clockwise);     /* so Escape puts back the old crop, turned */
  else
    commit ();
  changed_everything ();
}

int viewer_crop_mode (void) { return crop_mode; }
int viewer_grid (void) { return grid_mode; }
int viewer_crop_shape (void) { return crop_shape; }
int viewer_picking (void) { return picking; }
const char *viewer_grid_name (int k) { return grid_names[k & 3]; }
const char *viewer_shape_name (int k) { return k >= 0 && k < 6 ? shape_names[k] : ""; }

void viewer_set_grid (int k)
{
  grid_mode = k & 3;
  force_image ();
  force_panel ();
}

/* The shape's width over height, in pixels, for the crop as it is now
   (portrait when the crop is taller than wide). 0 for free. */
static double shape_aspect (void)
{
  double a, w = (current.v[P_CROP_X1] - current.v[P_CROP_X0]) * gw;
  double h = (current.v[P_CROP_Y1] - current.v[P_CROP_Y0]) * gh;
  switch (crop_shape)
    {
    case 1: a = sw > sh ? (double) sw / sh : (double) sh / sw; break;
    case 2: return 1;
    case 3: a = 1.5; break;
    case 4: a = 4.0 / 3; break;
    case 5: a = 16.0 / 9; break;
    default: return 0;
    }
  return h > w ? 1 / a : a;
}

static void crop_store (double x0, double y0, double x1, double y1)
{
  current.v[P_CROP_X0] = (float) x0; current.v[P_CROP_Y0] = (float) y0;
  current.v[P_CROP_X1] = (float) x1; current.v[P_CROP_Y1] = (float) y1;
}

void viewer_set_crop_shape (int k)
{
  double x0, y0, x1, y1, a;
  if (k < 0 || k > 5)
    return;
  crop_shape = k;
  if (!src)
    return;
  if (!crop_mode)
    viewer_crop_start ();
  a = shape_aspect ();
  x0 = current.v[P_CROP_X0]; y0 = current.v[P_CROP_Y0];
  x1 = current.v[P_CROP_X1]; y1 = current.v[P_CROP_Y1];
  if (a > 0)
    {
      geom_crop_aspect (&x0, &y0, &x1, &y1, a, gw, gh);
      crop_store (x0, y0, x1, y1);
    }
  refresh (1);
}

void viewer_crop_start (void)
{
  if (!src || crop_mode)
    return;
  crop_saved = current;
  crop_mode = 1;
  before = 0;
  picking = 0;
  update_title ();
  refresh (1);
  force_panel ();
}

/* apply: keep the new crop (one undo step); else put the old one back. */
void viewer_crop_finish (int apply)
{
  int p;
  if (!crop_mode)
    return;
  if (dragging == DRAG_CROP)
    dragging = -1;
  if (!apply)
    for (p = P_CROP_X0; p <= P_CROP_Y1; p++)
      current.v[p] = crop_saved.v[p];
  crop_mode = 0;
  /* Rounded to the recipe's steps, as saved. */
  for (p = P_CROP_X0; p <= P_CROP_Y1; p++)
    recipe_set (&current, p, current.v[p]);
  if (current.v[P_CROP_X1] - current.v[P_CROP_X0] > 0.999f && current.v[P_CROP_Y1] - current.v[P_CROP_Y0] > 0.999f)
    for (p = P_CROP_X0; p <= P_CROP_Y1; p++)
      current.v[p] = recipe_params[p].def;
  commit ();
  update_title ();
  if (is_open)
    changed_everything ();
}

void viewer_toggle_crop (void)
{
  if (crop_mode)
    viewer_crop_finish (1);
  else
    viewer_crop_start ();
}

/* Starts a crop drag at (fx, fy) (fractions across the whole picture,
   y down): finds the corner, edge or inside it was on. */
static int crop_grab (double fx, double fy, double tol_x, double tol_y)
{
  double x0 = current.v[P_CROP_X0], y0 = current.v[P_CROP_Y0];
  double x1 = current.v[P_CROP_X1], y1 = current.v[P_CROP_Y1];
  int e = 0, inside_x = fx > x0 - tol_x && fx < x1 + tol_x, inside_y = fy > y0 - tol_y && fy < y1 + tol_y;
  if (inside_y && fabs (fx - x0) < tol_x) e |= E_L;
  else if (inside_y && fabs (fx - x1) < tol_x) e |= E_R;
  if (inside_x && fabs (fy - y0) < tol_y) e |= E_T;
  else if (inside_x && fabs (fy - y1) < tol_y) e |= E_B;
  if (e && shape_aspect () > 0 && (e == E_L || e == E_R || e == E_T || e == E_B))
    {
      /* With a fixed shape, an edge takes its nearer corner. */
      if (e & (E_L | E_R))
        e |= fy < (y0 + y1) / 2 ? E_T : E_B;
      else
        e |= fx < (x0 + x1) / 2 ? E_L : E_R;
    }
  if (!e)
    e = fx > x0 && fx < x1 && fy > y0 && fy < y1 ? E_MOVE : E_NEW;
  crop_start[0] = x0; crop_start[1] = y0; crop_start[2] = x1; crop_start[3] = y1;
  crop_px = fx;
  crop_py = fy;
  /* The fixed corner, for corners and new rectangles. */
  crop_ax = e & E_L ? x1 : x0;
  crop_ay = e & E_T ? y1 : y0;
  if (e == E_NEW)
    {
      crop_ax = fx;
      crop_ay = fy;
    }
  return e;
}

static double clamp01 (double v) { return v < 0 ? 0 : v > 1 ? 1 : v; }

static void crop_follow (double fx, double fy)
{
  const double m = 0.02;               /* geometry.c's smallest crop */
  double x0 = crop_start[0], y0 = crop_start[1], x1 = crop_start[2], y1 = crop_start[3];
  double a = shape_aspect ();
  int e = crop_edges;
  fx = clamp01 (fx);
  fy = clamp01 (fy);
  if (e == E_MOVE)
    {
      double dx = fx - crop_px, dy = fy - crop_py;
      if (x0 + dx < 0) dx = -x0;
      if (x1 + dx > 1) dx = 1 - x1;
      if (y0 + dy < 0) dy = -y0;
      if (y1 + dy > 1) dy = 1 - y1;
      x0 += dx; x1 += dx; y0 += dy; y1 += dy;
    }
  else if (e == E_NEW || ((e & (E_L | E_R)) && (e & (E_T | E_B))))
    {
      /* A corner (or a new rectangle) from the fixed corner to the pointer. */
      double w = fabs (fx - crop_ax) * gw, h = fabs (fy - crop_ay) * gh;
      int right = fx >= crop_ax, down = fy >= crop_ay;
      double room_w = (right ? 1 - crop_ax : crop_ax) * gw, room_h = (down ? 1 - crop_ay : crop_ay) * gh;
      if (e == E_NEW && a > 0 && w > 0 && h > 0)
        a = (w > h) == (a > 1) ? a : 1 / a;   /* a new rectangle takes the way the pointer went */
      if (w < m * gw) w = m * gw;
      if (h < m * gh) h = m * gh;
      if (a > 0)
        {
          if (w / h > a) w = h * a; else h = w / a;
          if (w > room_w) { w = room_w; h = w / a; }
          if (h > room_h) { h = room_h; w = h * a; }
        }
      if (w > room_w) w = room_w;
      if (h > room_h) h = room_h;
      x0 = right ? crop_ax : crop_ax - w / gw;
      x1 = right ? crop_ax + w / gw : crop_ax;
      y0 = down ? crop_ay : crop_ay - h / gh;
      y1 = down ? crop_ay + h / gh : crop_ay;
    }
  else
    {
      if (e & E_L) x0 = fx < x1 - m ? fx : x1 - m;
      if (e & E_R) x1 = fx > x0 + m ? fx : x0 + m;
      if (e & E_T) y0 = fy < y1 - m ? fy : y1 - m;
      if (e & E_B) y1 = fy > y0 + m ? fy : y0 + m;
    }
  crop_store (clamp01 (x0), clamp01 (y0), clamp01 (x1), clamp01 (y1));
  refresh (0);
}

/* --- input */
static void start_drag (int x, int y, int x0, int y0, int x1, int y1, int what)
{
  /* Wimp_DragBox block (classic PRM; not in the 2026 edition): window,
     type 7 (drag point, nothing drawn), the start box, and the box the
     pointer is kept in. */
  int db[10];
  db[0] = win;
  db[1] = 7;
  db[2] = x; db[3] = y; db[4] = x; db[5] = y;
  db[6] = x0; db[7] = y0; db[8] = x1; db[9] = y1;
  if (_swix (Wimp_DragBox, _IN (1), db) == NULL)
    {
      dragging = what;
      drag_x = x;
      drag_y = y;
    }
  else
    {
      dragging = -1;
      if (what >= 0)
        commit ();
      restart_full ();
    }
}

static void link_click (int g, int id)
{
  int p;
  switch (id)
    {
    case LINK_RESET:
      for (p = recipe_groups[g].first; p <= recipe_groups[g].last; p++)
        current.v[p] = recipe_params[p].def;
      if (recipe_groups[g].first == P_ROTATE && !crop_mode)
        for (p = P_CROP_X0; p <= P_CROP_Y1; p++)   /* the crop was of the turned picture */
          current.v[p] = recipe_params[p].def;
      if (!crop_mode)
        commit ();
      changed_everything ();
      break;
    case LINK_PICK: viewer_pick_white (); break;
    case LINK_AUTO_WB: auto_wb (); break;
    case LINK_LEFT: viewer_rotate (0); break;
    case LINK_RIGHT: viewer_rotate (1); break;
    }
}

static void tool_click (int k, int b0, int b1)
{
  switch (k)
    {
    case TOOL_AUTO: viewer_auto (); break;
    case TOOL_CROP: viewer_toggle_crop (); break;
    case TOOL_GRID: viewer_set_grid (grid_mode + 1); break;
    case TOOL_PRESETS: menus_open_presets_menu (b0, b1); break;
    }
}

void viewer_click (int *b)
{
  int st[9], wx, wy, what = -1, h;
  st[0] = win;
  if (_swix (Wimp_GetWindowState, _IN (1), st))
    return;
  if (b[2] == Button_Menu)
    {
      menus_open_viewer_menu (b[0], b[1]);
      return;
    }
  _swix (Wimp_SetCaretPosition, _INR (0, 5), win, -1, 0, 0, 1 << 25, -1);
  wx = b[0] - st[1];
  wy = b[1] - st[4];
  h = hit (wx, wy, &what);
  if (h == HIT_SLIDER)
    {
      if (b[2] == Button_Adjust)       /* Adjust: back to the default */
        {
          recipe_set (&current, what, recipe_params[what].def);
          if (!crop_mode || what < P_FIRST_GEOMETRY)
            commit ();
          before = 0;
          refresh (1);
          force_row (what);
          return;
        }
      if (b[2] != Button_Select)
        return;
      before = 0;
      recipe_set (&current, what, x_to_value (what, wx));
      refresh (0);
      force_row (what);
      if (what >= P_FIRST_GEOMETRY)
        force_image ();                /* the fine grid while straightening */
      start_drag (b[0], b[1], st[1] + track_x0 () - 4, st[4] + row_top (what) - ROW_H + 4,
                  st[1] + track_x1 () + 4, st[4] + row_top (what) - 4, what);
    }
  else if (h == HIT_IMAGE && b[2] == Button_Select)
    {
      int r[4];
      double fx, fy;
      if (!photo_rect (r) || wx < r[0] - HANDLE || wx > r[2] + HANDLE || wy < r[1] - HANDLE || wy > r[3] + HANDLE)
        return;
      fx = (double) (wx - r[0]) / (r[2] - r[0]);
      fy = (double) (r[3] - wy) / (r[3] - r[1]);
      if (picking)
        {
          if (fx >= 0 && fx <= 1 && fy >= 0 && fy <= 1)
            pick_at (fx, fy);
        }
      else if (crop_mode)
        {
          crop_edges = crop_grab (fx, fy, (double) HANDLE / (r[2] - r[0]), (double) HANDLE / (r[3] - r[1]));
          start_drag (b[0], b[1], st[1] + r[0], st[4] + r[1], st[1] + r[2], st[4] + r[3], DRAG_CROP);
        }
    }
  else if (h == HIT_LINK && b[2] == Button_Select)
    link_click (what / 8, what % 8);
  else if (h == HIT_TOOL && b[2] == Button_Select)
    tool_click (what, b[0], b[1]);
  else if (h == HIT_UNDO && b[2] == Button_Select)
    viewer_undo ();
  else if (h == HIT_REDO && b[2] == Button_Select)
    viewer_redo ();
  else if (h == HIT_RESET_ALL && b[2] == Button_Select)
    viewer_reset_all ();
}

/* User_Drag_Box: the button was let go. */
void viewer_drag_end (void)
{
  int p = dragging;
  if (dragging == -1)
    return;
  dragging = -1;
  if (p >= 0)
    {
      force_row (p);
      if (p >= P_FIRST_GEOMETRY)
        force_image ();                /* the straightening grid goes */
      if (!crop_mode || p < P_FIRST_GEOMETRY)
        commit ();
    }
  restart_full ();
}

int viewer_key (int key)
{
  if (crop_mode && (key == KEY_RETURN || key == KEY_ESCAPE))
    {
      viewer_crop_finish (key == KEY_RETURN);
      return 1;
    }
  if (picking && key == KEY_ESCAPE)
    {
      viewer_pick_white ();
      return 1;
    }
  switch (key)
    {
    case KEY_LEFT:
    case KEY_RIGHT:
      {
        int i = index_in_list + (key == KEY_LEFT ? -1 : 1);
        if (crop_mode)
          viewer_crop_finish (1);
        if (i >= 0 && i < browser_count ())
          viewer_show (i);
        return 1;
      }
    case KEY_ESCAPE:
    case 'v':
    case 'V':
      viewer_close ();
      return 1;
    case 'm':
    case 'M':
      viewer_toggle_before ();
      return 1;
    case 'a':
    case 'A':
      viewer_auto ();
      return 1;
    case 'c':
    case 'C':
      viewer_toggle_crop ();
      return 1;
    case 'g':
    case 'G':
      viewer_set_grid (grid_mode + 1);
      return 1;
    case 'w':
    case 'W':
      viewer_pick_white ();
      return 1;
    case '[':
      viewer_rotate (0);
      return 1;
    case ']':
      viewer_rotate (1);
      return 1;
    case 0x03:                         /* Ctrl-C */
      looks_copy_viewer ();
      return 1;
    case 0x16:                         /* Ctrl-V */
      if (looks_clip ())
        viewer_apply_look (looks_clip ());
      return 1;
    case 0x1A:                         /* Ctrl-Z */
      viewer_undo ();
      return 1;
    case 0x19:                         /* Ctrl-Y */
      viewer_redo ();
      return 1;
    }
  if ((key >= '0' && key <= '5') || key == '9' || key == '/')
    {
      cat *c = lib_cat (app_library ());
      if (key == '/')
        cat_set_flag (c, &item.id, 1, -1);
      else
        cat_set_rating (c, &item.id, 1, key == '9' ? RATING_REJECT : key - '0');
      browser_after_change ();
      browser_item (index_in_list, &item);
      force (panel_x0 (), info_top () - INFO_H, vis_w, info_top ());
      return 1;
    }
  return 0;
}

/* --- drawing */
static void draw_slider (int p, int ox, int oy)
{
  const recipe_param *d = &recipe_params[p];
  int top = oy + row_top (p), mid = top - ROW_H / 2;
  int x0 = ox + track_x0 (), x1 = ox + track_x1 ();
  int kx = ox + value_to_x (p, current.v[p]);
  int zx = ox + value_to_x (p, d->def);
  char value[24];
  draw_text (ox + panel_x0 () + PAD, mid - 9, d->label, COL_TEXT, COL_PANEL);
  draw_rect (x0, mid - 4, x1, mid + 4, COL_TRACK);
  /* Filled from the default to the value, so a change shows at a glance. */
  if (kx != zx)
    draw_rect (kx < zx ? kx : zx, mid - 4, kx < zx ? zx : kx, mid + 4, COL_ACCENT);
  draw_rect (kx - 10, mid - 18, kx + 10, mid + 18, COL_FRAME);
  draw_rect (kx - 7, mid - 15, kx + 7, mid + 15, p == dragging ? COL_ACCENT : COL_KNOB);
  snprintf (value, sizeof value, "%s%.*f", current.v[p] > 0 && d->min < 0 ? "+" : "",
            d->decimals, (double) current.v[p]);
  draw_text (ox + vis_w - PAD - draw_text_width (value), mid - 9, value, COL_TEXT, COL_PANEL);
}

static void draw_button_rect (const int r[4], const char *text, int enabled, int on, int ox, int oy)
{
  unsigned bg = on ? COL_ACCENT : COL_BUTTON;
  draw_rect (ox + r[0], oy + r[1], ox + r[2], oy + r[3], COL_FRAME);
  draw_rect (ox + r[0] + 3, oy + r[1] + 3, ox + r[2] - 3, oy + r[3] - 3, bg);
  draw_text (ox + (r[0] + r[2] - draw_text_width (text)) / 2, oy + r[1] + 20, text,
             on ? COL_KNOB : enabled ? COL_TEXT : COL_STAR_OFF, bg);
}

static void draw_button (int k, const char *text, int enabled, int ox, int oy)
{
  int r[4];
  button_rect (k, r);
  draw_button_rect (r, text, enabled, 0, ox, oy);
}

static void draw_panel (int ox, int oy)
{
  int px = ox + panel_x0 (), g, k;
  unsigned peak = 1;
  draw_rect (px, oy - vis_h, ox + vis_w, oy, COL_PANEL);
  draw_rect (px, oy - vis_h, px + 2, oy, COL_FRAME);
  /* Histogram */
  {
    int x0 = px + PAD, x1 = ox + vis_w - PAD, y1 = oy + hist_top (), y0 = y1 - HIST_H;
    int bw = (x1 - x0) / 64;
    draw_rect (x0, y0, x1, y1, COL_HIST_BG);
    for (k = 1; k < 63; k++)           /* the ends hold clipped pixels: don't let them flatten the rest */
      if (hist[k] > peak)
        peak = hist[k];
    for (k = 0; k < 64; k++)
      {
        unsigned v = hist[k] > peak ? peak : hist[k];
        int h = (int) ((unsigned long long) v * (HIST_H - 12) / peak);
        if (h > 0)
          draw_rect (x0 + 4 + k * bw, y0 + 4, x0 + 4 + k * bw + bw - 2, y0 + 4 + h, COL_HIST);
      }
  }
  /* Place in the list, rating */
  {
    char t[64];
    int y = oy + info_top () - 34;
    snprintf (t, sizeof t, "%d of %d", index_in_list + 1, browser_count ());
    draw_text (px + PAD, y, t, COL_QUIET, COL_PANEL);
    if (item.rating == RATING_REJECT)
      draw_text (ox + vis_w - PAD - draw_text_width ("Rejected"), y, "Rejected", COL_QUIET, COL_PANEL);
    else
      for (k = 0; k < 5; k++)
        draw_star (ox + vis_w - PAD - 15 - (4 - k) * 34, y + 10, 15, k < item.rating ? COL_ACCENT : COL_STAR_OFF);
    if (item.flag)
      {
        draw_rect (px + 200, y - 4, px + 228, y + 24, COL_FRAME);
        draw_rect (px + 204, y, px + 224, y + 20, 0xB0E6F200u);
      }
  }
  /* Tools */
  {
    static const char *const names[TOOL_COUNT] = { "Auto", "Crop", "Grid", "Presets" };
    for (k = 0; k < TOOL_COUNT; k++)
      {
        int r[4];
        tool_rect (k, r);
        draw_button_rect (r, k == TOOL_CROP && crop_mode ? "Done" : names[k],
                          src != NULL && !(k == TOOL_AUTO && crop_mode),
                          (k == TOOL_CROP && crop_mode) || (k == TOOL_GRID && grid_mode), ox, oy);
      }
  }
  /* Groups of sliders */
  for (g = 0; g < recipe_ngroups; g++)
    {
      int top = oy + group_top (g), p, changed = 0, ids[3], n;
      const char *names[3];
      if (!group_shown (g))
        continue;
      for (p = recipe_groups[g].first; p <= recipe_groups[g].last; p++)
        if (current.v[p] != recipe_params[p].def)
          changed = 1;
      if (recipe_groups[g].first == P_ROTATE && !crop_mode)
        for (p = P_CROP_X0; p <= P_CROP_Y1; p++)   /* its Reset takes the crop away too */
          if (current.v[p] != recipe_params[p].def)
            changed = 1;
      draw_rect (px + PAD, top - 2, ox + vis_w - PAD, top, COL_TRACK);
      draw_text (px + PAD, top - 36, recipe_groups[g].name, COL_TEXT, COL_PANEL);
      n = group_links (g, ids, names);
      for (k = 0; k < n; k++)
        {
          int x0, x1;
          unsigned col = COL_ACCENT;
          link_x (g, k, &x0, &x1);
          if (ids[k] == LINK_RESET && !changed)
            col = COL_STAR_OFF;
          if (ids[k] == LINK_PICK && picking)
            {
              draw_rect (ox + x0 - 6, top - 44, ox + x1 + 6, top - 8, COL_ACCENT);
              draw_text (ox + x0, top - 36, names[k], COL_KNOB, COL_ACCENT);
              continue;
            }
          draw_text (ox + x0, top - 36, names[k], src ? col : COL_STAR_OFF, COL_PANEL);
        }
      for (p = recipe_groups[g].first; p <= recipe_groups[g].last; p++)
        if (!recipe_params[p].hidden)
          draw_slider (p, ox, oy);
    }
  draw_button (0, "Undo", viewer_can_undo (), ox, oy);
  draw_button (1, "Redo", viewer_can_redo (), ox, oy);
  draw_button (2, "Reset all", !recipe_is_default (&current), ox, oy);
}

/* Grid lines over a rectangle (screen coordinates): n by n cells. */
static void draw_grid (int x0, int y0, int x1, int y1, int n, unsigned col)
{
  int k;
  for (k = 1; k < n; k++)
    {
      int x = x0 + (x1 - x0) * k / n, y = y0 + (y1 - y0) * k / n;
      draw_rect (x - 1, y0, x + 1, y1, col);
      draw_rect (x0, y - 1, x1, y + 1, col);
    }
}

static void draw_overlays (int ox, int oy, const int out[4])
{
  static const int cells[4] = { 0, 3, 4, 8 };
  int n = cells[grid_mode];
  if (dragging >= P_ANGLE && dragging <= P_PERSP_H)
    n = 12;                            /* a fine grid to straighten against */
  if (crop_mode)
    {
      int w = out[2] - out[0], h = out[3] - out[1];
      int x0 = out[0] + (int) lround (current.v[P_CROP_X0] * w), x1 = out[0] + (int) lround (current.v[P_CROP_X1] * w);
      int y1 = out[3] - (int) lround (current.v[P_CROP_Y0] * h), y0 = out[3] - (int) lround (current.v[P_CROP_Y1] * h);
      int c[4][2] = { { x0, y0 }, { x1, y0 }, { x0, y1 }, { x1, y1 } }, k;
      (void) ox; (void) oy;
      draw_grid (x0, y0, x1, y1, n ? n : 3, COL_GRID);
      draw_frame (x0 - 2, y0 - 2, x1 + 2, y1 + 2, COL_CROP);
      draw_frame (x0, y0, x1, y1, COL_CROP);
      for (k = 0; k < 4; k++)          /* corner handles */
        {
          draw_rect (c[k][0] - 12, c[k][1] - 12, c[k][0] + 12, c[k][1] + 12, COL_FRAME);
          draw_rect (c[k][0] - 9, c[k][1] - 9, c[k][0] + 9, c[k][1] + 9, COL_CROP);
        }
    }
  else if (n)
    draw_grid (out[0], out[1], out[2], out[3], n, COL_GRID);
}

static void draw_image (int ox, int oy)
{
  int r[4], out[4];
  image_rect (r);
  draw_rect (ox + r[0], oy + r[1], ox + r[2], oy + r[3], COL_GROUND);
  r[0] += PAD; r[1] += PAD; r[2] -= PAD; r[3] -= PAD;
  if ((state == V_READY || state == V_LOADING) && (src || state == V_LOADING))
    {
      const sprite32 *s = show_half || full_y >= 0 ? &half_spr : &full_spr;
      if (s->area)
        {
          int pr[4];
          draw_sprite_rect (s->area, (const unsigned *) s->area + 4, s->w, s->h,
                            ox + r[0], oy + r[1], ox + r[2], oy + r[3], out);
          /* The overlays line up with the full-size placement. */
          if (photo_rect (pr))
            {
              pr[0] += ox; pr[2] += ox; pr[1] += oy; pr[3] += oy;
              draw_overlays (ox, oy, pr);
            }
        }
      else
        {
          /* Still decoding: the thumbnail, enlarged. */
          const thumb_sprite *t = thumbcache_get (item.id, item.thumb_state == THUMB_READY);
          if (t)
            draw_sprite_rect (t->area, t->sprite, t->w, t->h, ox + r[0], oy + r[1], ox + r[2], oy + r[3], out);
          else
            draw_text (ox + r[0] + 24, oy + r[3] - 60, "Opening...", COL_LIGHT, COL_GROUND);
        }
      if (before || picking || crop_mode)
        {
          const char *t = before ? "Before" : picking ? "Click something that should be grey or white"
            : "Drag the corners or edges; Return to crop, Escape to cancel";
          int tw = draw_text_width (t);
          draw_rect (ox + r[0] + 16, oy + r[3] - 64, ox + r[0] + 56 + tw, oy + r[3] - 16, 0x00000000u);
          draw_text (ox + r[0] + 36, oy + r[3] - 50, t, COL_LIGHT, 0x00000000u);
        }
    }
  else if (state == V_FAILED)
    draw_text (ox + r[0] + 24, oy + r[3] - 60, err, COL_LIGHT, COL_GROUND);
}

void viewer_redraw (int *b)
{
  int more = 0;
  _swix (Wimp_RedrawWindow, _IN (1) | _OUT (0), b, &more);
  while (more)
    {
      int ox = b[1] - b[5], oy = b[4] - b[6];
      int cx0 = b[7] - ox;             /* the rectangle in work area x */
      if (cx0 < panel_x0 ())
        draw_image (ox, oy);
      if (b[9] - ox > panel_x0 ())
        draw_panel (ox, oy);
      _swix (Wimp_GetRectangle, _IN (1) | _OUT (0), b, &more);
    }
}

void viewer_mode_changed (void)
{
  read_screen ();
  force (0, -0x10000000, 0x10000000, 0);
}

/* For the screenshot harness and tests. */
int viewer_proxy_size (int *w, int *h)
{
  *w = sw;
  *h = sh;
  return src != NULL;
}

int viewer_shown_size (int *w, int *h)
{
  *w = gw;
  *h = gh;
  return geo != NULL;
}
