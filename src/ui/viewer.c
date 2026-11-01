/* viewer.c: one photo, large, with the adjustments panel beside it.

   Opening a photo shows its thumbnail at once (enlarged), decodes a proxy
   (up to PROXY_SIDE pixels on the long side) in idle time, turns it into
   16-bit linear light, and renders it through the recipe a band of rows
   per slice. While a slider is dragged, a half-size copy is rendered
   instead, every time the value changes, so the photo follows the pointer;
   the full proxy is rendered again when the button is let go. Every
   change is saved to the catalogue as it is made (and can be undone), and
   the thumbnail is made again in the background.

   Layout, in work area coordinates (origin top left, y down is negative):
   the photo fills the left of the window on a dark ground; the panel is
   PANEL_W wide on the right: histogram, the photo's place and rating,
   then the recipe's groups of sliders (recipe_groups), then Undo, Redo
   and Reset all. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <kernel.h>
#include <swis.h>
#include "develop.h"
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
#define GROUP_H 52
#define ROW_H 56
#define BUTTON_H 60

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

enum { V_CLOSED, V_LOADING, V_READY, V_FAILED };
enum { HIT_NONE, HIT_SLIDER, HIT_GROUP_RESET, HIT_UNDO, HIT_REDO, HIT_RESET_ALL };

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
static sprite32 full_spr, half_spr;
static int show_half;                  /* displaying the half-size render */
static int full_y = -1;                /* next band of the full render; -1 when done */
static unsigned hist[64], hist_acc[64];

static develop *dev;
static recipe current, undo_stack[UNDO_MAX];
static int undo_n, undo_i;
static int before;                     /* showing the original */
static int dragging = -1, drag_x = -1; /* the parameter being dragged, the pointer's last x */

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

static void free_photo (void)
{
  thumbgen_free (tg);
  tg = NULL;
  free (src); free (half);
  src = half = NULL;
  sprite_free (&full_spr);
  sprite_free (&half_spr);
  full_y = -1;
  show_half = 0;
}

/* --- geometry: everything in work area coordinates */
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
static int groups_top (void) { return info_top () - INFO_H; }

/* The top of group g's header, and of parameter p's row. */
static int group_top (int g)
{
  int y = groups_top (), k;
  for (k = 0; k < g; k++)
    y -= GROUP_H + ROW_H * (recipe_groups[k].last - recipe_groups[k].first + 1) + 8;
  return y;
}

static int row_top (int p)
{
  int g;
  for (g = 0; g < recipe_ngroups; g++)
    if (p >= recipe_groups[g].first && p <= recipe_groups[g].last)
      return group_top (g) - GROUP_H - ROW_H * (p - recipe_groups[g].first);
  return 0;
}

static int buttons_top (void)
{
  return group_top (recipe_ngroups) - 16;
}

static void button_rect (int k, int r[4])
{
  int w = (PANEL_W - 2 * PAD - 2 * 16) / 3;
  r[0] = panel_x0 () + PAD + k * (w + 16);
  r[2] = r[0] + w;
  r[3] = buttons_top ();
  r[1] = r[3] - BUTTON_H;
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

static int hit (int wx, int wy, int *what)
{
  int p, g, k, r[4];
  if (wx < panel_x0 ())
    return HIT_NONE;
  for (p = 0; p < P_COUNT; p++)
    {
      int top = row_top (p);
      if (wy <= top && wy > top - ROW_H && wx >= track_x0 () - 24 && wx <= track_x1 () + 24)
        {
          *what = p;
          return HIT_SLIDER;
        }
    }
  for (g = 0; g < recipe_ngroups; g++)
    {
      int top = group_top (g);
      if (wy <= top && wy > top - GROUP_H && wx > panel_x0 () + PANEL_W - 140)
        {
          *what = g;
          return HIT_GROUP_RESET;
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
  b[17] = (900 << 16) | 1200;          /* minimum height, width */
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
  force (panel_x0 (), row_top (p) - ROW_H, vis_w, row_top (p));
}

static void force_hist (void)
{
  force (panel_x0 (), hist_top () - HIST_H, vis_w, hist_top ());
}

static void update_title (void)
{
  char t[200];
  snprintf (t, sizeof t, "%s - %s (%d of %d)%s", APP_NAME, item.name, index_in_list + 1,
            browser_count (), recipe_is_default (&current) ? "" : "  \xB7  edited");
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
static void prepare (void)
{
  recipe r;
  if (before)
    recipe_default (&r);
  else
    r = current;
  develop_prepare (dev, &r);
}

/* The half-size copy, all at once (it is a quarter of the pixels). */
static void render_half (void)
{
  if (!half || !half_spr.area)
    return;
  memset (hist, 0, sizeof hist);
  develop_rows (dev, half, hw, 0, hh, half_spr.pixels, hist);
  show_half = 1;
  force_image ();
  force_hist ();
}

/* Starts the full proxy again from the top, in bands. */
static void restart_full (void)
{
  if (!src)
    return;
  full_y = 0;
  memset (hist_acc, 0, sizeof hist_acc);
}

static void render_full_until (unsigned deadline)
{
  while (full_y >= 0 && full_y < sh)
    {
      int y1 = full_y + BAND_ROWS < sh ? full_y + BAND_ROWS : sh;
      develop_rows (dev, src, sw, full_y, y1, full_spr.pixels, hist_acc);
      full_y = y1;
      if ((int) (app_now () - deadline) >= 0)
        break;
    }
  if (full_y >= sh)
    {
      full_y = -1;
      show_half = 0;
      memcpy (hist, hist_acc, sizeof hist);
      force_image ();
      force_hist ();
    }
}

/* Proxy decoded: linear copies, the half-size copy, the sprites. */
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
  if (!src || !half || sprite_make (&full_spr, sw, sh) || sprite_make (&half_spr, hw, hh))
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
  b[0] = win;
  _swix (Wimp_CloseWindow, _IN (1), b);
  is_open = 0;
  dragging = -1;
  free_photo ();
  state = V_CLOSED;
  browser_focus_on (index_in_list);
}

int viewer_busy (void)
{
  return is_open && (dragging >= 0 || (state == V_LOADING && tg) || full_y >= 0);
}

/* Called on null events while busy. */
void viewer_idle (void)
{
  unsigned deadline = app_now () + 2;
  if (!is_open)
    return;
  if (dragging >= 0)
    {
      int ptr[5], b[9];
      b[0] = win;
      if (_swix (Wimp_GetPointerInfo, _IN (1), ptr) == NULL && ptr[0] != drag_x
          && _swix (Wimp_GetWindowState, _IN (1), b) == NULL)
        {
          float old = current.v[dragging];
          drag_x = ptr[0];
          recipe_set (&current, dragging, x_to_value (dragging, ptr[0] - b[1]));
          if (current.v[dragging] != old)
            {
              prepare ();
              render_half ();
              force_row (dragging);
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
  prepare ();
  render_half ();
  restart_full ();
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
  recipe_default (&current);
  commit ();
  changed_everything ();
}

void viewer_toggle_before (void)
{
  before = !before;
  prepare ();
  render_half ();
  restart_full ();
}

int viewer_can_undo (void) { return undo_i > 0; }
int viewer_can_redo (void) { return undo_i + 1 < undo_n; }
int viewer_before (void) { return before; }
const recipe *viewer_recipe (void) { return &current; }

/* --- input */
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
          commit ();
          before = 0;
          prepare ();
          render_half ();
          restart_full ();
          force_row (what);
          return;
        }
      if (b[2] != Button_Select)
        return;
      if (before)
        {
          before = 0;
          prepare ();
        }
      recipe_set (&current, what, x_to_value (what, wx));
      prepare ();
      render_half ();
      force_row (what);
      /* Follow the pointer along the track until the button is let go.
         Wimp_DragBox block (classic PRM; not in the 2026 edition): window,
         type 7 (drag point, nothing drawn), the start box, and the box the
         pointer is kept in: the track's row. */
      {
        int db[10];
        db[0] = win;
        db[1] = 7;
        db[2] = b[0]; db[3] = b[1]; db[4] = b[0]; db[5] = b[1];
        db[6] = st[1] + track_x0 () - 4;
        db[7] = st[4] + row_top (what) - ROW_H + 4;
        db[8] = st[1] + track_x1 () + 4;
        db[9] = st[4] + row_top (what) - 4;
        if (_swix (Wimp_DragBox, _IN (1), db) == NULL)
          {
            dragging = what;
            drag_x = b[0];
          }
        else
          {
            commit ();
            restart_full ();
          }
      }
    }
  else if (h == HIT_GROUP_RESET && b[2] == Button_Select)
    {
      int p;
      for (p = recipe_groups[what].first; p <= recipe_groups[what].last; p++)
        current.v[p] = recipe_params[p].def;
      commit ();
      changed_everything ();
    }
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
  if (dragging < 0)
    return;
  force_row (dragging);
  dragging = -1;
  commit ();
  restart_full ();
}

int viewer_key (int key)
{
  switch (key)
    {
    case KEY_LEFT:
    case KEY_RIGHT:
      {
        int i = index_in_list + (key == KEY_LEFT ? -1 : 1);
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

static void draw_button (int k, const char *text, int enabled, int ox, int oy)
{
  int r[4];
  button_rect (k, r);
  draw_rect (ox + r[0], oy + r[1], ox + r[2], oy + r[3], COL_FRAME);
  draw_rect (ox + r[0] + 3, oy + r[1] + 3, ox + r[2] - 3, oy + r[3] - 3, COL_BUTTON);
  draw_text (ox + (r[0] + r[2] - draw_text_width (text)) / 2, oy + r[1] + 20, text,
             enabled ? COL_TEXT : COL_STAR_OFF, COL_BUTTON);
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
  /* Groups of sliders */
  for (g = 0; g < recipe_ngroups; g++)
    {
      int top = oy + group_top (g), p, changed = 0;
      for (p = recipe_groups[g].first; p <= recipe_groups[g].last; p++)
        if (current.v[p] != recipe_params[p].def)
          changed = 1;
      draw_rect (px + PAD, top - 2, ox + vis_w - PAD, top, COL_TRACK);
      draw_text (px + PAD, top - 36, recipe_groups[g].name, COL_TEXT, COL_PANEL);
      draw_text (ox + vis_w - PAD - draw_text_width ("Reset"), top - 36, "Reset",
                 changed ? COL_ACCENT : COL_STAR_OFF, COL_PANEL);
      for (p = recipe_groups[g].first; p <= recipe_groups[g].last; p++)
        draw_slider (p, ox, oy);
    }
  draw_button (0, "Undo", viewer_can_undo (), ox, oy);
  draw_button (1, "Redo", viewer_can_redo (), ox, oy);
  draw_button (2, "Reset all", !recipe_is_default (&current), ox, oy);
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
        draw_sprite_rect (s->area, (const unsigned *) s->area + 4, s->w, s->h,
                          ox + r[0], oy + r[1], ox + r[2], oy + r[3], out);
      else
        {
          /* Still decoding: the thumbnail, enlarged. */
          const thumb_sprite *t = thumbcache_get (item.id, item.thumb_state == THUMB_READY);
          if (t)
            draw_sprite_rect (t->area, t->sprite, t->w, t->h, ox + r[0], oy + r[1], ox + r[2], oy + r[3], out);
          else
            draw_text (ox + r[0] + 24, oy + r[3] - 60, "Opening...", COL_LIGHT, COL_GROUND);
        }
      if (before)
        {
          draw_rect (ox + r[0] + 16, oy + r[3] - 64, ox + r[0] + 150, oy + r[3] - 16, 0x00000000u);
          draw_text (ox + r[0] + 36, oy + r[3] - 50, "Before", COL_LIGHT, 0x00000000u);
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
