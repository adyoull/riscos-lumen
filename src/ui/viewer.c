/* Copyright (c) 2026 Andrew Youll
   Released under the MIT licence: see the LICENSE file.
   SPDX-License-Identifier: MIT */
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
#include "sharpen.h"
#include "denoise.h"
#include "retouch.h"
#include "thumbgen.h"
#include "ui.h"
#include "fsys.h"
#include "rawjob.h"
#include "lens.h"

#define PROXY_SIDE 1600
#define BAND_ROWS 32
#define UNDO_MAX 64

#define PANEL_W 640
#define PAD 24
#define HIST_H 150
#define INFO_H 52
#define TOOLS_H 156                    /* two rows of tool buttons and the space under them */
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
#define COL_ACCENT 0x99440000u          /* #004499, the Wimp's dark blue (colour 8) */
#define COL_HIST_BG 0x1C1C1C00u
#define COL_HIST_GUIDE 0x33333300u
#define COL_KNOB 0xFFFFFF00u
#define COL_FRAME 0x33333300u
#define COL_BUTTON 0xF5F5F500u
#define COL_STAR_OFF 0x9A9A9A00u
#define COL_GRID 0xC8C8C800u
#define COL_CROP 0xFFFFFF00u
#define COL_HEADER 0xC9C9C900u          /* a group's header bar (folds it) */

enum { V_CLOSED, V_LOADING, V_READY, V_FAILED };
enum { HIT_NONE, HIT_SLIDER, HIT_LINK, HIT_UNDO, HIT_REDO, HIT_RESET_ALL, HIT_TOOL, HIT_IMAGE, HIT_GROUP, HIT_CURVE, HIT_CURVE_TAB,
       HIT_HSL_BAND, HIT_LABEL, HIT_MASK_LINE, HIT_MASK_TICK, HIT_MASK_BUTTON, HIT_MASK_SLIDER, HIT_MASK_BRUSH, HIT_MASK_RT, HIT_FOLD_ALL,
       HIT_LENS_CAMERA };
#define LABEL_X0 250                   /* the panel's colour label dots: the first's centre */
#define LABEL_DX 24
enum { TOOL_AUTO, TOOL_CROP, TOOL_GRID, TOOL_PRESETS, TOOL_RETOUCH, TOOL_BRUSH, TOOL_COUNT };
enum { LINK_RESET, LINK_PICK, LINK_AUTO_WB, LINK_LEFT, LINK_RIGHT, LINK_AUTO_NR, LINK_AUTO_LENS, LINK_LENS_MENU };
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
static recipe_lens geo_lens;           /* and the lens profile it was made with */
static int geo_key_valid;
static sprite32 full_spr, half_spr;
static int show_half;                  /* displaying the half-size render */
static int full_y = -1;                /* next band of the full render; -1 when done */
static dev_hist hist, hist_acc;

static develop *dev;
static recipe current, undo_stack[UNDO_MAX];
static int undo_n, undo_i;
static int before;                     /* showing the original */
static int dragging = -1, drag_x = -1, drag_y = -1; /* the parameter (or DRAG_CROP), the pointer's last place */

static int crop_mode, crop_edges, crop_shape, grid_mode, picking;

/* Retouch: spots painted out (retouch.h). The proxy is kept as decoded
   (src_orig) once a photo has spots; src is it with the spots applied. */
static int retouching, rt_clone;       /* a local tool is on (Retouch, or Masks with brushing) */
static int brushing, br_kind;          /* br_kind: BRUSH_ADD or BRUSH_ERASE */
static int mk_sel = -1;                /* the chosen mask (-1: none; painting makes a new one) */
static int mk_new = -1;                /* MASK_GRADIENT or MASK_RADIAL: the next drag draws one */
static int mk_show, mk_handle;         /* Show mask; the handle being dragged */
static recipe_mask mk_last;            /* the adjustments a new mask starts with */
static int mk_last_set;
static float mk_du0, mk_dv0;           /* where a new shape's drag began */
static float rt_gu, rt_gv;             /* where a spot was taken hold of */
static int applied_show = -1;          /* the mask src shows in red */
static float br_pts[RECIPE_STROKE_MAX * 2];   /* the stroke being painted */
static int br_n;
static float rt_strength;              /* a stroke's amount */
static float rt_size = 0.015f;         /* a new spot's radius, a fraction of the longer side */
static float rt_feather = 0.3f;        /* and its feather */
static int rt_sel = -1;                /* the spot last placed or moved, which the sliders change too */
static int rt_sel_auto;
static int rt_hover, rt_hx, rt_hy, rt_hr;  /* the brush under the pointer: centre (work area), radius */
static void rt_brush_follow (void);                /* and its source was found for it (found again when it is resized) */
static uint16_t *src_orig;
static recipe applied;                 /* the spots and strokes src has */
static int applied_n = -1;             /* -1: not yet looked */
static int rt_drag = -1, rt_drag_src;  /* the spot being moved, and whether its source */
static float rt_dx, rt_dy;             /* where it is being moved to (fractions) */
#define DRAG_SPOT (-5)
#define DRAG_RT_SIZE (-6)
#define DRAG_RT_FEATHER (-7)
#define DRAG_RT_STRENGTH (-8)
#define DRAG_STROKE (-9)
#define DRAG_HANDLE (-11)
#define DRAG_SHAPE (-12)
#define DRAG_MASK_SLIDER (-30)         /* - M_EV ... - M_SHARPNESS */
static void handle_follow (float u, float v);
static void shape_end (float u, float v);
static void mask_value (int p, float v, int live);
static float mb_slider_value (int p, int x);
static recipe_mask *mb_mask (void);
static void stroke_follow (int wx, int wy);
static void stroke_end (void);
static void draw_masks_panel (int ox, int oy);
static void masks_panel_click (const int *b, int h, int what, int wx, const int *st);
static float *rt_slider_var (int k);
static float rt_size_max (void);
void viewer_toggle_retouch (void);
static void local_tool (int brush);
static int retouch_sync (void);
static void make_half (void);
static void retouch_click (const int *b, int wx, int wy);
static int screen_to_photo (const int out[4], int wx, int wy, float *u, float *v);
static void spots_changed (void);
enum { RT_SIZE, RT_FEATHER, RT_STRENGTH, RT_SLIDERS };
#define RT_SLOT 430                    /* a slider with its label and value */
#define RT_ROW 64
#define RT_LABEL_W 150                 /* "Strength" and a gap */
#define RT_TRACK_W 180
#define RT_SIZE_MIN 0.002f
#define RT_SIZE_MAX 0.1f
static float rt_slider_value (int k, int x);
static void rt_apply (int live);

/* Compare: a photo pinned on the left (its render kept as it was), the
   one being looked at on the right; Left and Right change the right one. */
static int comparing;
static sprite32 cmp_spr;
static cat_item cmp_item;
static int compare_pending = -1;       /* pin when this render is done, then show this index (-2: the next) */
static void compare_stop (void);
static void compare_pin_now (void);

/* 100%: the full-size original (8-bit, decoded on first use), and the
   part of the output that fits the window, rendered into zspr. zcx, zcy
   is the output pixel at the middle of the view. */
static int zoom;                       /* 0 fit, 1 decoding the full size, 2 at 100% */
static char photo_path[1024];
static int photo_orientation, photo_long;   /* the original's longer side */
static int photo_raw;                  /* a camera raw file: its full size is read ahead (it is slow) */
static cat_id photo_original;          /* its original (for its preview, library.h) */
static int photo_is_raw;               /* the original is read by a plug-in (a camera raw or HEIF photo) */
static int photo_kind;                 /* and which (rawjob.h's RAWJOB_*) */
static int from_kept;                  /* opened from the library's preview, not the file */

/* A camera raw file's own noise reduction (in the plug-in, before
   demosaicing) is in its recipe: the picture is developed again when it
   changes. What the picture on screen was developed with: */
static int loaded_noise;

static int raw_nr_photo (void)
{
  return src && photo_is_raw && photo_kind == RAWJOB_RAW;
}

/* Lens correction: what the photo says of its camera and lens (read from
   its file when first wanted; a raw's lens as the plug-in found it). */
static lens_photo vlens;
static cat_id vlens_id;
static img_info raw_info;               /* a raw's details from the plug-in (lens, focal lengths) */
static int raw_info_ok;
static thumbgen *info_gen;             /* opened from its preview: the plug-in's quick look, for them */
static int info_tried;
static int info_asked;                 /* wanted (Auto, or the Lens menu): fetched only then, not at every opening */
static int auto_after_info;            /* Auto again once they're here */

int viewer_lens_photo (lens_photo *ph)
{
  cat_details d;
  memset (ph, 0, sizeof *ph);
  ph->camera = -1;
  if (state == V_CLOSED || !item.id)
    return 0;
  if (vlens_id != item.id)
    {
      memset (&vlens, 0, sizeof vlens);
      vlens.camera = -1;
      if (cat_get_details (lib_cat (app_library ()), item.id, &d) == 0)
        {
          snprintf (vlens.make, sizeof vlens.make, "%s", d.make);
          snprintf (vlens.model, sizeof vlens.model, "%s", d.model);
          vlens.focal = d.focal;
          vlens.fnumber = d.fnumber;
          lens_photo_read (&vlens, d.path);
        }
      if (raw_info_ok)
        {
          if (!vlens.lens[0] && raw_info.lens[0])
            snprintf (vlens.lens, sizeof vlens.lens, "%s", raw_info.lens);
          if (vlens.fmin <= 0 && raw_info.lens_fmin > 0)
            {
              vlens.fmin = raw_info.lens_fmin;
              vlens.fmax = raw_info.lens_fmax;
              vlens.amin = raw_info.lens_amin;
            }
          if (vlens.crop <= 0 && raw_info.focal35 > 0 && vlens.focal > 0)
            vlens.crop = raw_info.focal35 / vlens.focal;
        }
      vlens_id = item.id;
    }
  *ph = vlens;
  return 1;
}

const recipe_lens *viewer_lens_profile (void) { return &current.lens; }

/* The Lens menu opened: a raw's lens details, if not had yet, for the next time. */
void viewer_lens_want_info (void) { info_asked = 1; }

/* The camera the profile is scaled for: the one chosen for it, by name. */
static int lens_chosen_camera (lens_db *db)
{
  int c;
  if (!db || !current.lens.camera[0])
    return -1;
  for (c = 0; c < lens_db_cameras (db); c++)
    {
      char nm[128];
      const char *mk = lens_db_camera_maker (db, c), *md = lens_db_camera_model (db, c);
      if (!strcmp (md, current.lens.camera))
        return c;
      snprintf (nm, sizeof nm, "%.20s %.40s", mk, md);
      if (!strcmp (nm, current.lens.camera))
        return c;
    }
  return -1;
}

/* Whether the photo's camera is in the database (2 if there's no
   database to say: taken to be). */
static cat_id lens_cam_known_id;
static int lens_cam_known_v;
static int lens_camera_known (void)
{
  lens_db *db;
  lens_photo ph;
  const char *mount;
  double crop;
  if (!app_has_lens_db ())
    return 2;
  if (lens_cam_known_id == item.id)
    return lens_cam_known_v;
  db = app_lens_db ();
  viewer_lens_photo (&ph);
  lens_cam_known_v = !db ? 2 : lens_db_camera (db, ph.make, ph.model, &mount, &crop) >= 0;
  lens_cam_known_id = item.id;
  return lens_cam_known_v;
}

static void commit (void);
static void changed_everything (void);

/* Lens i's profile for the photo, the camera chosen kept (c: -2 keep, -1
   none, else that camera). */
static void lens_set (int i, int c)
{
  lens_db *db = app_lens_db ();
  lens_photo ph;
  recipe_lens L;
  if (!db || !src || i < 0)
    return;
  viewer_lens_photo (&ph);
  ph.camera = c == -2 ? lens_chosen_camera (db) : c;
  if (lens_db_profile (db, i, &ph, &L))
    return;
  current.lens = L;
  commit ();
  changed_everything ();
}

static int lens_current_index (lens_db *db)
{
  int i;
  if (!db || !current.lens.on)
    return -1;
  for (i = 0; i < lens_db_count (db); i++)
    if (!strcmp (lens_db_name (db, i), current.lens.name))
      {
        /* the same lens measured more than once: the one whose scales these are */
        return i;
      }
  return -1;
}

void viewer_lens_choose (int i) { lens_set (i, -2); }

void viewer_lens_camera (int c)
{
  lens_db *db = app_lens_db ();
  lens_photo ph;
  int i;
  if (!db || !current.lens.on)
    return;
  /* The lens again, likeliest for that camera if measured more than once. */
  viewer_lens_photo (&ph);
  ph.camera = c;
  snprintf (ph.lens, sizeof ph.lens, "%s", current.lens.name);
  i = lens_db_match (db, &ph);
  if (i < 0 || strcmp (lens_db_name (db, i), current.lens.name))
    i = lens_current_index (db);
  lens_set (i, c);
}

void viewer_lens_none (void)
{
  if (!current.lens.on)
    return;
  memset (&current.lens, 0, sizeof current.lens);
  commit ();
  changed_everything ();
}

void viewer_lens_auto (void)
{
  lens_db *db = app_lens_db ();
  lens_photo ph;
  int i;
  if (!db || !src)
    {
      if (!app_has_lens_db ())
        app_report ("Lens profiles come with the LumenLens plug-in: with it installed, Auto finds the photo's lens.");
      return;
    }
  viewer_lens_photo (&ph);
  i = lens_db_match (db, &ph);
  if (i < 0 && photo_is_raw && !raw_info_ok && from_kept && !info_tried)
    {
      /* Opened from its preview: the plug-in's quick look at the raw for
         the lens's details, then Auto again by itself. */
      info_asked = auto_after_info = 1;
      return;
    }
  if (i < 0)
    {
      if (photo_is_raw && !raw_info_ok && info_gen)
        app_inform ("Lumen is still reading this raw file's details from the plug-in. Try Auto again in a moment.");
      else
        app_report ("Lumen couldn't tell which lens this photo was taken with. Choose it from the Lens menu.");
      return;
    }
  lens_set (i, -1);
}

/* Starts developing the full size, with the photo's raw noise reduction. */
static thumbgen *start_full (char *e, size_t en)
{
  thumbgen *t;
  rawjob_set_raw_noise (loaded_noise);
  t = thumbgen_start (photo_path, NULL, 1 << 30, photo_orientation, e, en);
  rawjob_set_raw_noise (0);
  return t;
}

static thumbgen *zgen;
static rgb_image zfull;
static geom_plan zplan;
static double zcx, zcy, zfx = 0.5, zfy = 0.5;
static sprite32 zspr;
static int zrx, zry, zy = -1;          /* the region's top left; next band, -1 done */
static uint16_t *zlin;
static int pan_dx, pan_dy;             /* while dragging the view: how far, in OS units */
static int pan_start_x, pan_start_y;
#define DRAG_PAN (-3)
#define DRAG_CURVE (-4)

/* The loupe: a round magnifier that follows the pointer over the photo,
   showing the full-size original there at 100%, 200% or 400%. */
#define LOUPE_PX 300                   /* its diameter in screen pixels */
static int loupe_on, loupe_pinned, loupe_mag = 1, loupe_shown, loupe_dirty;
static int loupe_wx, loupe_wy;         /* its centre, work area coordinates */
static double loupe_fx, loupe_fy;      /* the point of the photo under it */
static geom_plan lplan;
static int lplan_ok;
static unsigned *loupe_area;           /* a sprite with a round mask */
static uint16_t *loupe_lin;
static uint32_t *loupe_px;
static recipe crop_saved;
static double crop_ax, crop_ay, crop_start[4], crop_px, crop_py;

static const char *const shape_names[6] = { "Free", "Original", "Square", "3:2", "4:3", "16:9" };
static const char *const grid_names[4] = { "None", "Thirds", "Quarters", "Fine" };

static void zoom_layout (int replan);

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
  if (s->area)
    draw_forget ((unsigned *) s->area + 4);
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

static void loupe_force (void);

static void free_zoom (void)
{
  if (loupe_shown)
    loupe_force ();
  loupe_shown = 0;
  lplan_ok = 0;
  thumbgen_free (zgen);
  zgen = NULL;
  free (zfull.rgb);
  zfull.rgb = NULL;
  free (zlin);
  zlin = NULL;
  sprite_free (&zspr);
  zy = -1;
  zoom = 0;
}

/* Highlights & Shadows: the proxy's lightness map (develop.h), made when
   first needed after the proxy is read. */
static dev_lummap *lummap;
static int lummap_ok;

static int previewing;                 /* a style shown from the menu, not yet chosen */
static recipe preview_saved;           /* the photo's own recipe meanwhile */

static void free_photo (void)
{
  thumbgen_free (info_gen);
  info_gen = NULL;
  info_tried = info_asked = auto_after_info = 0;
  raw_info_ok = 0;
  vlens_id = 0;
  previewing = 0;
  photo_raw = 0;
  lummap_ok = 0;
  free_zoom ();
  thumbgen_free (tg);
  tg = NULL;
  free_geo ();
  free (src); free (half);
  src = half = NULL;
  free (src_orig);
  src_orig = NULL;
  applied_n = -1;
  applied_show = -1;
  retouching = 0;
  br_n = 0;
  rt_drag = -1;
  rt_sel = -1;
  mk_sel = mk_new = -1;
  mk_show = 0;
  sprite_free (&full_spr);
  sprite_free (&half_spr);
  full_y = -1;
  show_half = 0;
}

/* --- layout: everything in work area coordinates */
/* The panel is drawn in the inspector (mainwin.c), a window of its own:
   its coordinates are that window's work area, x from 0 to PANEL_W. */
static int panel_x0 (void) { return 0; }
static int drag_win = -1;              /* the window a drag started in */
static int track_x0 (void) { return panel_x0 () + 232; }
static int track_x1 (void) { return panel_x0 () + PANEL_W - 132; }

static void image_rect (int r[4])
{
  r[0] = comparing ? vis_w / 2 : 0; r[1] = -vis_h; r[2] = vis_w; r[3] = 0;
}

/* Top of each part of the panel. */
static int hist_top (void) { return -PAD; }
static int info_top (void) { return hist_top () - HIST_H - 16; }
static int tools_top (void) { return info_top () - INFO_H; }
/* Fold all and Open all, on a line of their own under the tools (for
   the Layers section and the groups). */
#define FOLD_LINKS_H 44
static int fold_links_h (void);
static int fold_links_top (void) { return tools_top () - TOOLS_H; }
static int masks_top (void) { return fold_links_top () - fold_links_h (); }
static int masks_height (void);
static int groups_top (void) { return masks_top () - masks_height (); }

/* While the Masks tool is on, the panel is the masks' (the groups go). */
static int group_shown (int g) { return recipe_groups[g].panel && !(retouching && brushing); }

/* --- the panel's Masks section: a line for each mask (a tick to switch
   it off and on), and while the Masks tool is on, buttons for new ones,
   the chosen mask's sliders, and Invert, Show mask and Delete. Shown when
   the photo has masks, or the tool is on. */
#define MASK_LINE_H 52
enum { MB_NEW_BRUSH, MB_NEW_GRADIENT, MB_NEW_RADIAL, MB_INVERT, MB_SHOW, MB_DELETE, MB_BUTTONS };
static int masks_tool (void) { return retouching && brushing; }
static int fold_links_h (void) { return retouching && brushing ? 0 : FOLD_LINKS_H; }
static int masks_folded;               /* the list folded away (not while the tool is on) */
static int masks_shown (void) { return src && (current.nmasks > 0 || masks_tool ()); }
static int mask_lines (void) { return current.nmasks ? current.nmasks : 1; }

static int mask_line_top (int i) { return masks_top () - GROUP_H - i * MASK_LINE_H; }
static int mask_new_top (void) { return mask_line_top (mask_lines ()) - 52; }
/* The brush: Add and Erase, then Size, Feather and Flow (RT_*). */
static int mask_brush_top (void) { return mask_new_top () - BUTTON_H - 52; }
static int mask_rt_top (int k) { return mask_brush_top () - BUTTON_H - 8 - k * ROW_H; }
/* The chosen layer's adjustments. */
static int mask_slider_top (int k) { return mask_rt_top (RT_SLIDERS) - 52 - k * ROW_H; }
static int mask_buttons_top (void) { return mask_slider_top (M_COUNT) - 12; }

static int masks_height (void)
{
  if (!masks_shown ())
    return 0;
  if (!masks_tool ())
    return masks_folded ? GROUP_H + 8 : GROUP_H + mask_lines () * MASK_LINE_H + 24;
  return masks_top () - (mask_buttons_top () - BUTTON_H) + 24;
}

/* Groups fold away: a click on a group's name. A
   group the person hasn't folded or opened themselves is folded
   automatically, from the bottom, when the panel wouldn't fit. */
#define MAX_GROUPS 16
static int folded[MAX_GROUPS], folded_by_hand[MAX_GROUPS];

/* A parameter with no slider: hidden ones, and Raw (noise reduction in
   the raw plug-in) unless the photo is a camera raw file. */
static int raw_nr_photo (void);
static int param_hidden (int p)
{
  if (p == P_LENS_DIST)
    return !current.lens.on || !current.lens.dist;
  if (p == P_LENS_TCA)
    return !current.lens.on || !current.lens.tca;
  if (p == P_LENS_VIG)
    return !current.lens.on || !current.lens.vig;
  return recipe_params[p].hidden || (p == P_NOISE_RAW && !raw_nr_photo ());
}

/* Lens Correction: two lines (the lens, and what it's for) above its sliders. */
#define LENS_INFO_H 92
static int group_pre (int g) { return recipe_groups[g].first == P_LENS_DIST ? LENS_INFO_H : 0; }

static int group_rows (int g)
{
  int p, n = 0;
  if (g < MAX_GROUPS && folded[g])
    return 0;
  for (p = recipe_groups[g].first; p <= recipe_groups[g].last; p++)
    if (!param_hidden (p))
      n++;
  return n;
}

/* The curves editor: channel buttons, then the graph. */
#define CURVE_SEL_H 72
#define CURVE_BOX_H 360
static int curve_ch, curve_drag = -1;

/* Colour (HSL): a row of the eight bands' swatches with the chosen one's
   name under it, then that band's Hue, Saturation and Luminance. */
#define HSL_SEL_H 104
static int hsl_band = HSL_BLUE;

/* The height under a group's header. */
static int group_body (int g)
{
  if (g < MAX_GROUPS && folded[g])
    return 0;
  if (recipe_groups[g].curves)
    return CURVE_SEL_H + CURVE_BOX_H;
  if (recipe_groups[g].hsl)
    return HSL_SEL_H + 3 * ROW_H;
  return group_pre (g) + ROW_H * group_rows (g);
}

/* The top of group g's header, and of parameter p's row (0 if it has none). */
static int group_top (int g)
{
  int y = groups_top (), k;
  for (k = 0; k < g; k++)
    if (group_shown (k))
      y -= GROUP_H + group_body (k) + 8;
  return y;
}

static int row_top (int p)
{
  int g, q, i;
  if (p >= P_HSL && p <= P_HSL_END)
    {
      /* The chosen band's three, under the swatches. */
      if ((p - P_HSL) / 3 != hsl_band)
        return 0;
      for (g = 0; g < recipe_ngroups; g++)
        if (recipe_groups[g].hsl)
          return !group_shown (g) || (g < MAX_GROUPS && folded[g]) ? 0
                 : group_top (g) - GROUP_H - HSL_SEL_H - ROW_H * ((p - P_HSL) % 3);
      return 0;
    }
  if (param_hidden (p))
    return 0;
  for (g = 0; g < recipe_ngroups; g++)
    if (p >= recipe_groups[g].first && p <= recipe_groups[g].last)
      {
        if (!group_shown (g) || (g < MAX_GROUPS && folded[g]))
          return 0;
        for (i = 0, q = recipe_groups[g].first; q < p; q++)
          if (!param_hidden (q))
            i++;
        return group_top (g) - GROUP_H - group_pre (g) - ROW_H * i;
      }
  return 0;
}

static int buttons_top (void)
{
  return group_top (recipe_ngroups) - 16;
}

static int group_changed (int g)
{
  int p;
  if (recipe_groups[g].curves)
    {
      for (p = 0; p < CURVE_COUNT; p++)
        if (!recipe_curve_is_identity (&current.curve[p]))
          return 1;
      return 0;
    }
  if (recipe_groups[g].first == P_LENS_DIST && current.lens.on)
    return 1;
  for (p = recipe_groups[g].first; p <= recipe_groups[g].last; p++)
    if (current.v[p] != recipe_params[p].def)
      return 1;
  return 0;
}

static int curves_group (void)
{
  int g;
  for (g = 0; g < recipe_ngroups; g++)
    if (recipe_groups[g].curves)
      return g;
  return -1;
}

/* The graph's inner rectangle (work area), 0 if the group is folded. */
static int curve_graph (int r[4])
{
  int g = curves_group (), top;
  if (g < 0 || (g < MAX_GROUPS && folded[g]))
    return 0;
  top = group_top (g) - GROUP_H - CURVE_SEL_H;
  r[0] = panel_x0 () + PAD + 10;
  r[2] = PANEL_W - PAD - 10;
  r[3] = top - 10;
  r[1] = top - CURVE_BOX_H + 10;
  return 1;
}

static void curve_tab_rect (int k, int r[4])
{
  int g = curves_group (), top = group_top (g) - GROUP_H, w = (PANEL_W - 2 * PAD - 3 * 12) / 4;
  r[0] = panel_x0 () + PAD + k * (w + 12);
  r[2] = r[0] + w;
  r[3] = top - 6;
  r[1] = top - CURVE_SEL_H + 12;
}

static int hsl_group (void)
{
  int g;
  for (g = 0; g < recipe_ngroups; g++)
    if (recipe_groups[g].hsl)
      return g;
  return -1;
}

/* Band k's swatch (work area); 0 if the group is folded. */
static int hsl_swatch (int k, int r[4])
{
  int g = hsl_group (), top, w = (PANEL_W - 2 * PAD - 7 * 8) / 8;
  if (g < 0 || !group_shown (g) || (g < MAX_GROUPS && folded[g]))
    return 0;
  top = group_top (g) - GROUP_H;
  r[0] = panel_x0 () + PAD + k * (w + 8);
  r[2] = r[0] + w;
  r[3] = top - 8;
  r[1] = top - 52;
  return 1;
}

/* The panel's height (it scrolls in the inspector). */
static int panel_height (void)
{
  return -(buttons_top () - BUTTON_H - PAD);
}

int viewer_panel_height (void) { return panel_height (); }

/* The panel's height may have changed. */
static void fit_panel (void)
{
  if (mainwin_tab () == TAB_ADJUST)
    mainwin_inspector_extent (panel_height ());
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
/* Auto, Crop, Grid, Presets; then Retouch and Masks, a row lower. */
static void tool_rect (int k, int r[4])
{
  if (k < TOOL_RETOUCH)
    button_at (k, 4, tools_top (), r);
  else
    button_at (k - TOOL_RETOUCH, 2, tools_top () - BUTTON_H - 12, r);
}

/* The Lens Correction group's two lines: the lens (or why there's none),
   and what its profile is for, with a Camera link (when there's a
   profile and a database). Text fitted to w. */
static void fit_text (char *t, int w)
{
  size_t l = strlen (t);
  if (draw_text_width (t) <= w)
    return;
  while (l > 3 && (t[l - 3] = '.', t[l - 2] = '.', t[l - 1] = '.', t[l] = '\0', draw_text_width (t) > w))
    {
      l--;
      t[l] = '\0';
    }
}

static int lens_camera_link (void) { return current.lens.on && app_has_lens_db (); }

static void lens_lines (char *a, char *b, size_t n)
{
  const recipe_lens *L = &current.lens;
  a[0] = b[0] = '\0';
  if (!L->on)
    {
      snprintf (a, n, "%s", app_has_lens_db () ? "No lens profile yet" : "No lens profiles: add LumenLens");
      snprintf (b, n, "%s", app_has_lens_db () ? "Auto finds it, or choose from Lens" : "Manual corrects distortion by hand");
      return;
    }
  snprintf (a, n, "%s", L->name);
  {
    char f[48] = "";
    if (L->focal > 0 && L->aperture > 0)
      snprintf (f, sizeof f, "%g mm, f/%g", (double) L->focal, (double) L->aperture);
    else if (L->focal > 0)
      snprintf (f, sizeof f, "%g mm", (double) L->focal);
    if (L->camera[0])
      snprintf (b, n, "%s%sas on %s", f, f[0] ? ", " : "", L->camera);
    else if (lens_camera_known () == 0)
      snprintf (b, n, "%s%scamera not known", f, f[0] ? ", " : "");
    else
      snprintf (b, n, "%s", f);
  }
}

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
  else if (recipe_groups[g].first == P_NOISE_LUM)
    {
      ids[n] = LINK_AUTO_NR; names[n++] = "Auto";
    }
  else if (recipe_groups[g].first == P_LENS_DIST && app_has_lens_db ())
    {
      ids[n] = LINK_LENS_MENU; names[n++] = "Lens";
      ids[n] = LINK_AUTO_LENS; names[n++] = "Auto";
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
  int ids[3], i, x = PANEL_W - PAD;
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

static int hit (int wx, int wy, int *what, int panel)
{
  int p, g, k, r[4];
  if (!panel)
    return HIT_IMAGE;
  if (curve_graph (r))
    {
      if (wx >= r[0] - 10 && wx <= r[2] + 10 && wy >= r[1] - 10 && wy <= r[3] + 10)
        return HIT_CURVE;
      for (k = 0; k < CURVE_COUNT; k++)
        {
          curve_tab_rect (k, r);
          if (wx >= r[0] && wx <= r[2] && wy >= r[1] && wy <= r[3])
            {
              *what = k;
              return HIT_CURVE_TAB;
            }
        }
    }
  if (wy <= info_top () - 10 && wy >= info_top () - INFO_H + 4 && wx >= panel_x0 () + LABEL_X0 - LABEL_DX / 2
      && wx < panel_x0 () + LABEL_X0 + CAT_LABELS * LABEL_DX - LABEL_DX / 2)
    {
      *what = (wx - panel_x0 () - LABEL_X0 + LABEL_DX / 2) / LABEL_DX;
      return HIT_LABEL;
    }
  for (k = 0; k < HSL_BANDS; k++)
    if (hsl_swatch (k, r) && wx >= r[0] && wx <= r[2] && wy >= r[1] - 4 && wy <= r[3] + 4)
      {
        *what = k;
        return HIT_HSL_BAND;
      }
  if (masks_shown ())
    {
      if (!masks_tool () && wy <= masks_top () && wy > masks_top () - GROUP_H && wx >= PANEL_W - PAD - 120)
        {
          *what = -1;                  /* Edit */
          return HIT_MASK_LINE;
        }
      if (!masks_tool () && wy <= masks_top () && wy > masks_top () - GROUP_H && wx >= panel_x0 () + PAD)
        {
          *what = -2;                  /* fold or open */
          return HIT_MASK_LINE;
        }
      for (k = 0; k < (masks_folded && !masks_tool () ? 0 : current.nmasks); k++)
        if (wy <= mask_line_top (k) && wy > mask_line_top (k) - MASK_LINE_H && wx >= panel_x0 () + PAD && wx <= PANEL_W - PAD)
          {
            *what = k;
            return wx < panel_x0 () + PAD + 52 ? HIT_MASK_TICK : HIT_MASK_LINE;
          }
      if (masks_tool ())
        {
          for (k = 0; k < MB_BUTTONS; k++)
            {
              button_at (k % 3, 3, k < 3 ? mask_new_top () : mask_buttons_top (), r);
              if (wx >= r[0] && wx <= r[2] && wy >= r[1] && wy <= r[3])
                {
                  *what = k;
                  return HIT_MASK_BUTTON;
                }
            }
          for (k = 0; k < M_COUNT; k++)
            if (wy <= mask_slider_top (k) && wy > mask_slider_top (k) - ROW_H && wx >= track_x0 () - 24 && wx <= track_x1 () + 24)
              {
                *what = k;
                return HIT_MASK_SLIDER;
              }
          for (k = 0; k < BRUSH_KINDS; k++)
            {
              button_at (k, 3, mask_brush_top (), r);
              if (wx >= r[0] && wx <= r[2] && wy >= r[1] && wy <= r[3])
                {
                  *what = k;
                  return HIT_MASK_BRUSH;
                }
            }
          for (k = 0; k < RT_SLIDERS; k++)
            if (wy <= mask_rt_top (k) && wy > mask_rt_top (k) - ROW_H && wx >= track_x0 () - 24 && wx <= track_x1 () + 24)
              {
                *what = k;
                return HIT_MASK_RT;
              }
        }
    }
  if (fold_links_h () && wy <= fold_links_top () && wy > fold_links_top () - FOLD_LINKS_H)
    {
      int x1 = PANEL_W - PAD, x0 = x1 - draw_text_width ("Open all");
      if (wx >= x0 - 12 && wx <= x1 + 4)
        {
          *what = 1;                   /* Open all */
          return HIT_FOLD_ALL;
        }
      x1 = x0 - 40;
      x0 = x1 - draw_text_width ("Fold all");
      if (wx >= x0 - 12 && wx <= x1 + 12)
        {
          *what = 0;
          return HIT_FOLD_ALL;
        }
    }
  for (g = 0; g < recipe_ngroups; g++)
    if (group_pre (g) && group_shown (g) && !(g < MAX_GROUPS && folded[g]) && lens_camera_link ())
      {
        int y = group_top (g) - GROUP_H - 44, x1 = PANEL_W - PAD, x0 = x1 - draw_text_width ("Camera");
        if (wy <= y && wy > y - 44 && wx >= x0 - 12 && wx <= x1 + 4)
          return HIT_LENS_CAMERA;
      }
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
      if (wx >= panel_x0 () + PAD && wx <= PANEL_W - PAD)
        {
          *what = g;                   /* anywhere on its header bar */
          return HIT_GROUP;
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
  /* A child of the main window (mainwin.c): no frame of its own. */
  b[7] = (int) 0x80000002;
  ((unsigned char *) b)[32] = 7;  ((unsigned char *) b)[33] = 2;
  ((unsigned char *) b)[34] = 7;  ((unsigned char *) b)[35] = 255;  /* we draw the background */
  ((unsigned char *) b)[36] = 3;  ((unsigned char *) b)[37] = 1;
  ((unsigned char *) b)[38] = 12;
  b[10] = 0; b[11] = -screen_h; b[12] = screen_w; b[13] = 0;
  b[14] = 0x13D;
  b[15] = 3 << 12;                     /* work area: click */
  b[16] = 1;
  b[17] = (64 << 16) | 64;             /* minimum height, width */
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

/* The panel, if the inspector shows it. */
static void pforce (int x0, int y0, int x1, int y1)
{
  if (mainwin_tab () == TAB_ADJUST)
    mainwin_force_inspector (x0, y0, x1, y1);
}

static void force_panel (void)
{
  pforce (0, -0x10000000, 0x10000000, 0);
}

/* The photo and the panel. */
static void force_all (void)
{
  force (0, -0x10000000, 0x10000000, 0);
  force_panel ();
  mainwin_info_changed ();
}

static void force_row (int p)
{
  if (row_top (p))
    pforce (0, row_top (p) - ROW_H, PANEL_W, row_top (p));
}

static void force_hist (void)
{
  pforce (0, hist_top () - HIST_H, PANEL_W, hist_top ());
}

/* For the main window's title: the photo, its place in the list. */
void viewer_title (char *out, size_t n)
{
  if (comparing)
    {
      snprintf (out, n, "%s, compared with %s (%d of %d)", cmp_item.name, item.name, index_in_list + 1, browser_count ());
      return;
    }
  snprintf (out, n, "%s (%d of %d)%s%s", item.name, index_in_list + 1,
            browser_count (), recipe_is_default (previewing ? &preview_saved : &current) ? "" : "  \xB7  edited",
            crop_mode ? "  \xB7  cropping" : retouching ? (brushing ? "  \xB7  layers" : "  \xB7  retouching") : "");
}

static void update_title (void)
{
  if (state != V_CLOSED)
    mainwin_update_title ();
}

/* Opens the window at block's place, inside the main window (mainwin.c
   decides where). */
void viewer_place (int *block)
{
  if (block[3] - block[1] != vis_w || block[4] - block[2] != vis_h)
    {
      vis_w = block[3] - block[1];
      vis_h = block[4] - block[2];
      force (0, -0x10000000, 0x10000000, 0);
      if (zoom == 2)
        zoom_layout (0);
    }
  block[5] = 0;                        /* never scrolled */
  block[6] = 0;
  block[7] = -1;
  _swix (Wimp_OpenWindow, _INR (1, 4), block, TASK_WORD, mainwin_window (), 0);
  is_open = 1;
}

/* Taken out of the main window (the Browser layout): the photo stays. */
void viewer_unplace (void)
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
  loupe_shown = 0;
}

/* Its own Open_Window_Request. */
void viewer_open_window (int *block)
{
  if (block)
    viewer_place (block);
  else
    mainwin_set_mode (LAYOUT_VIEWER);
}

int viewer_has_photo (void) { return state != V_CLOSED; }
/* A new version of the photo shown (with its adjustments, or as the
   original), shown in its place. */
void viewer_new_version (int keep_look)
{
  int i = viewer_index ();
  if (i < 0)
    return;
  if (crop_mode)
    viewer_crop_finish (1);
  browser_focus_on (i);
  i = browser_new_version (keep_look);
  if (i >= 0)
    viewer_open (i);
}

int viewer_index (void) { return state != V_CLOSED ? index_in_list : -1; }

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
  if (develop_needs_mask (dev) && src)
    {
      if (!lummap)
        lummap = malloc (sizeof *lummap);
      if (lummap && !lummap_ok)
        {
          dev_lummap_lin (lummap, src, sw, sh);
          lummap_ok = 1;
        }
      if (lummap)
        develop_mask (dev, lummap, &plan_full, sw, sh);
    }
}

static int plan_identity (const geom_plan *g)
{
  return geom_plan_is_simple (g)
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
    key[p - P_FIRST_GEOMETRY] = crop_mode && p >= P_CROP_X0 && p <= P_CROP_Y1 ? 0 : r.v[p];
  key[P_COUNT - P_FIRST_GEOMETRY] = (float) crop_mode;
  key[P_COUNT - P_FIRST_GEOMETRY + 1] = (float) sw;
  if (geo_key_valid && memcmp (key, geo_key, sizeof key) == 0 && recipe_lens_equal (&r.lens, &geo_lens))
    return 0;
  memcpy (geo_key, key, sizeof key);
  geo_lens = r.lens;
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

/* Noise reduction then sharpening, last: on a picture at scale (its size
   over the full size's). Not in the crop tool, where the picture is only
   a guide. */
static void sharpen_pixels (uint32_t *px, int w, int h, int pic_long)
{
  recipe r;
  double scale = photo_long > 0 ? (double) pic_long / photo_long : 1;
  effective (&r);
  if (crop_mode || pic_long <= 0)
    return;
  if (r.v[P_NOISE_LUM] > 0 || r.v[P_NOISE_COL] > 0)
    denoise32 (px, w, h, (int) r.v[P_NOISE_LUM], (int) r.v[P_NOISE_COL], scale);
  if (r.v[P_SHARPEN] > 0)
    sharpen32 (px, w, h, (int) r.v[P_SHARPEN], sharpen_sigma (&r, scale));
}

static int longer_side (int w, int h) { return w > h ? w : h; }

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

/* The picture shown (the half-size render while the full one is being
   made), and its copy at the size shown made now, so the redraw is a
   straight copy: plotting it scaled in the redraw took long enough on the
   Pi to be seen drawing down the screen. */
static const sprite32 *shown_sprite (void)
{
  return show_half || full_y >= 0 ? &half_spr : &full_spr;
}

static void prepare_shown (void)
{
  const sprite32 *s = shown_sprite ();
  int r[4];
  if (!s->area || zoom == 2)
    return;
  image_rect (r);
  draw_prepare ((const unsigned *) s->area + 4, s->w, s->h, r[0] + PAD, r[1] + PAD, r[2] - PAD, r[3] - PAD);
}

/* The half-size copy, all at once (it is a quarter of the pixels). */
static void render_half (void)
{
  if (!geo_half || !half_spr.area)
    return;
  memset (&hist, 0, sizeof hist);
  develop_region (dev, ghw, ghh, 0, 0);
  develop_rows (dev, geo_half, ghw, 0, ghh, half_spr.pixels, &hist);
  sharpen_pixels (half_spr.pixels, ghw, ghh, longer_side (hw, hh));
  shade (half_spr.pixels, ghw, ghh, 0, ghh);
  show_half = 1;
  draw_forget ((unsigned *) half_spr.area + 4);
  prepare_shown ();
  force_image ();
  force_hist ();
}

/* Starts the full proxy again from the top, in bands. */
static void restart_full (void)
{
  if (!geo)
    return;
  full_y = 0;
  memset (&hist_acc, 0, sizeof hist_acc);
}

static void render_full_until (unsigned deadline)
{
  while (full_y >= 0 && full_y < gh)
    {
      int y1 = full_y + BAND_ROWS < gh ? full_y + BAND_ROWS : gh;
      if (geo_pending)
        geom_rows (&plan_full, src, geo, full_y, y1);
      develop_region (dev, gw, gh, 0, 0);
      develop_rows (dev, geo, gw, full_y, y1, full_spr.pixels, &hist_acc);
      shade (full_spr.pixels, gw, gh, full_y, y1);
      full_y = y1;
      if ((int) (app_now () - deadline) >= 0)
        break;
    }
  if (full_y >= gh)
    {
      sharpen_pixels (full_spr.pixels, gw, gh, longer_side (sw, sh));
      draw_forget ((unsigned *) full_spr.area + 4);   /* new pixels: smooth them again */
      full_y = -1;
      geo_pending = 0;
      show_half = 0;
      hist = hist_acc;
      prepare_shown ();
      force_image ();
      force_hist ();
      if (compare_pending != -1)
        compare_pin_now ();
    }
}

/* Something changed: the geometry image if need be, then the half-size
   render now, and (if full) the full one in the background. */
/* --- 100% */
static double clampd2 (double v, double lo, double hi) { return v < lo ? lo : v > hi ? hi : v; }

/* The view in screen pixels: the image area less its margins. */
static void zoom_view (int *w, int *h)
{
  int r[4];
  image_rect (r);
  *w = (r[2] - r[0] - 2 * PAD) >> draw_xeig ();
  *h = (r[3] - r[1] - 2 * PAD) >> draw_yeig ();
}

/* Plans the full-size output (geometry as now) and lays the region out
   round zcx, zcy; starts rendering it. */
static void zoom_layout (int replan)
{
  int vw, vh, w, h;
  if (zoom != 2 || !zfull.rgb)
    return;
  if (replan)
    {
      recipe r;
      double fx = zplan.ow ? zcx / zplan.ow : zfx, fy = zplan.oh ? zcy / zplan.oh : zfy;
      effective (&r);
      geom_plan_make (&zplan, &r, zfull.w, zfull.h, 1, 0, 0);
      zcx = fx * zplan.ow;
      zcy = fy * zplan.oh;
    }
  zoom_view (&vw, &vh);
  w = vw < zplan.ow ? vw : zplan.ow;
  h = vh < zplan.oh ? vh : zplan.oh;
  if (w < 1 || h < 1)
    return;
  zcx = clampd2 (zcx, w / 2.0, zplan.ow - w / 2.0);
  zcy = clampd2 (zcy, h / 2.0, zplan.oh - h / 2.0);
  zrx = (int) (zcx - w / 2.0);
  zry = (int) (zcy - h / 2.0);
  if (zrx + w > zplan.ow) zrx = zplan.ow - w;
  if (zry + h > zplan.oh) zry = zplan.oh - h;
  if (zrx < 0) zrx = 0;
  if (zry < 0) zry = 0;
  if (!zspr.area || zspr.w != w)
    {
      free (zlin);
      zlin = malloc ((size_t) w * BAND_ROWS * 6);
    }
  if (sprite_size (&zspr, w, h) || !zlin)
    {
      free_zoom ();
      app_report ("There isn't enough memory to show this photo at 100%.");
      return;
    }
  zy = 0;
}

static void zoom_render_until (unsigned deadline)
{
  const uint16_t *s2l = dev_srgb_to_linear ();
  while (zy >= 0 && zy < zspr.h)
    {
      int n = zspr.h - zy < BAND_ROWS ? zspr.h - zy : BAND_ROWS;
      geom_region_rgb8 (&zplan, zfull.rgb, s2l, zlin, zrx, zry + zy, zspr.w, n);
      develop_region (dev, zplan.ow, zplan.oh, zrx, zry + zy);
      develop_rows (dev, zlin, zspr.w, 0, n, zspr.pixels + (size_t) zy * zspr.w, NULL);
      zy += n;
      if ((int) (app_now () - deadline) >= 0)
        break;
    }
  if (zy >= zspr.h)
    {
      zy = -1;
      sharpen_pixels (zspr.pixels, zspr.w, zspr.h, longer_side (zfull.w, zfull.h));
    }
  force_image ();
}

/* Zooms to 100% with output fraction (fx, fy) in the middle, or back to fit. */
static void zoom_to (double fx, double fy)
{
  char e[200];
  retouching = 0;
  if (comparing)
    compare_stop ();
  if (zoom)
    {
      zoom = 0;
      zy = -1;
      force_image ();
      force_panel ();
      return;
    }
  if (!src || crop_mode || picking)
    return;
  zfx = fx;
  zfy = fy;
  zoom = 1;
  if (zfull.rgb)
    {
      zoom = 2;
      zplan.ow = zplan.oh = 0;
      zoom_layout (1);
    }
  else if (!zgen && !(zgen = start_full (e, sizeof e)))
    {
      zoom = 0;
      app_report (e);
    }
  force_image ();
  force_panel ();
}

void viewer_toggle_zoom (void)
{
  zoom_to (0.5, 0.5);
}

int viewer_zoomed (void) { return zoom; }

/* Called in idle time: the decode, then the region. */
static void zoom_idle (unsigned deadline)
{
  if (zgen)
    {
      int r;
      do
        r = thumbgen_step (zgen, 32);
      while (r == 0 && (int) (app_now () - deadline) < 0);
      if (r == 0)
        return;
      if (r > 0 && photo_is_raw && !raw_info_ok && thumbgen_raw_details (zgen, &raw_info) == 1)
        {
          raw_info_ok = 1;             /* (opened from its preview: the plug-in's details now) */
          vlens_id = 0;
        }
      if (r < 0 || thumbgen_take (zgen, &zfull))
        {
          char m[300];
          int asked = zoom || loupe_on;
          snprintf (m, sizeof m, "The full-size photo couldn't be read: %s", thumbgen_error (zgen));
          free_zoom ();
          photo_raw = 0;               /* not read ahead again */
          if (asked)
            app_report (m);            /* read ahead and not wanted yet: quietly */
          force_image ();
          return;
        }
      thumbgen_free (zgen);
      zgen = NULL;
      {
        recipe r;
        effective (&r);
        retouch_rgb8 (zfull.rgb, zfull.w, zfull.h, &r);   /* the spots, at full size */
      }
      lplan_ok = 0;
      loupe_dirty = 1;
      if (zoom == 1)
        {
          zoom = 2;
          zplan.ow = zplan.oh = 0;
          zoom_layout (1);
        }
      return;
    }
  if (zoom == 2 && zy >= 0)
    zoom_render_until (deadline);
}

/* --- the loupe */
static int loupe_os_w (void) { return LOUPE_PX << draw_xeig (); }
static int loupe_os_h (void) { return LOUPE_PX << draw_yeig (); }

static void loupe_force (void)
{
  /* The bezel rings reach 7 pixels beyond the lens: redraw all of them,
     or the outer rings are clipped flat at the four sides. */
  int hw = loupe_os_w () / 2 + (10 << draw_xeig ()), hh = loupe_os_h () / 2 + (10 << draw_yeig ());
  force (loupe_wx - hw, loupe_wy - hh, loupe_wx + hw, loupe_wy + hh);
}

/* The sprite: LOUPE_PX square, 32bpp, with a 1bpp mask that is a disc. */
static int loupe_make (void)
{
  int d = LOUPE_PX, mw = (d + 31) / 32, x, y;
  size_t img = (size_t) d * d * 4, msk = (size_t) mw * 4 * d, bytes = 16 + 44 + img + msk;
  unsigned *a, *sp, *m;
  double r = d / 2.0;
  if (loupe_area)
    return 0;
  a = calloc (1, bytes);
  loupe_lin = malloc ((size_t) d * d * 6);
  loupe_px = malloc ((size_t) d * d * 4);
  if (!a || !loupe_lin || !loupe_px)
    {
      free (a); free (loupe_lin); free (loupe_px);
      loupe_lin = NULL; loupe_px = NULL;
      return -1;
    }
  a[0] = (unsigned) bytes; a[1] = 1; a[2] = 16; a[3] = (unsigned) bytes;
  sp = a + 4;
  sp[0] = (unsigned) (44 + img + msk);
  memcpy (sp + 1, "loupe", 5);
  sp[4] = (unsigned) (d - 1); sp[5] = (unsigned) (d - 1);
  sp[6] = 0; sp[7] = 31;
  sp[8] = 44; sp[9] = (unsigned) (44 + img);
  sp[10] = 0x301680B5u;
  m = sp + 11 + img / 4;               /* words: the header is 11, the image d*d */
  for (y = 0; y < d; y++)
    for (x = 0; x < d; x++)
      {
        double dx = x + 0.5 - r, dy = y + 0.5 - r;
        if (dx * dx + dy * dy <= r * r)
          m[y * mw + x / 32] |= 1u << (x % 32);
      }
  loupe_area = a;
  return 0;
}

/* Renders what the loupe shows round loupe_fx, loupe_fy. */
static void loupe_render (void)
{
  int d = LOUPE_PX, n = (d + loupe_mag - 1) / loupe_mag, x, y;
  int cx, cy, rx, ry, x0, y0, x1, y1, w, h;
  uint32_t *out, ground = 0x2A2A2A;
  loupe_dirty = 0;
  if (loupe_make ())
    return;
  if (!zfull.rgb)
    {
      /* Still reading the full size (a raw takes a while): the picture on
         screen, enlarged as the full size would be, so the loupe is there
         at once; the full size replaces it when it comes. */
      const sprite32 *s = shown_sprite ();
      const uint32_t *px;
      double k, step, sx0, sy0;
      out = (uint32_t *) (loupe_area + 4 + 11);
      if (!s->area)
        {
          for (x = 0; x < d * d; x++)
            out[x] = ground;
          return;
        }
      px = s->pixels;
      /* Shown pixels per full-size pixel: the proxy is sw x sh of the
         original's photo_long; the half-size render a fraction of that. */
      k = photo_long > 0 ? (double) longer_side (sw, sh) / photo_long : 1;
      if (gw > 0)
        k *= (double) s->w / gw;
      if (k > 1)
        k = 1;
      step = k / loupe_mag;
      sx0 = loupe_fx * s->w - d / 2.0 * step;
      sy0 = loupe_fy * s->h - d / 2.0 * step;
      for (y = 0; y < d; y++)
        {
          double fy = sy0 + (y + 0.5) * step - 0.5;
          for (x = 0; x < d; x++)
            {
              double fx = sx0 + (x + 0.5) * step - 0.5;
              int ix, iy, jx, jy, c;
              unsigned a00, a01, a10, a11, v = 0;
              double ax, ay;
              if (fx < -0.5 || fy < -0.5 || fx > s->w - 0.5 || fy > s->h - 0.5)
                {
                  out[(size_t) y * d + x] = ground;
                  continue;
                }
              fx = fx < 0 ? 0 : fx > s->w - 1 ? s->w - 1 : fx;
              fy = fy < 0 ? 0 : fy > s->h - 1 ? s->h - 1 : fy;
              ix = (int) fx; iy = (int) fy;
              jx = ix + 1 < s->w ? ix + 1 : ix;
              jy = iy + 1 < s->h ? iy + 1 : iy;
              ax = fx - ix; ay = fy - iy;
              a00 = px[(size_t) iy * s->w + ix]; a01 = px[(size_t) iy * s->w + jx];
              a10 = px[(size_t) jy * s->w + ix]; a11 = px[(size_t) jy * s->w + jx];
              for (c = 0; c < 24; c += 8)
                {
                  double t = (((a00 >> c) & 255) * (1 - ax) + ((a01 >> c) & 255) * ax) * (1 - ay)
                           + (((a10 >> c) & 255) * (1 - ax) + ((a11 >> c) & 255) * ax) * ay;
                  v |= (unsigned) (t + 0.5) << c;
                }
              out[(size_t) y * d + x] = v;
            }
        }
      return;
    }
  if (!lplan_ok)
    {
      recipe r;
      effective (&r);
      geom_plan_make (&lplan, &r, zfull.w, zfull.h, 1, 0, 0);
      lplan_ok = 1;
    }
  out = (uint32_t *) (loupe_area + 4 + 11);
  cx = (int) (loupe_fx * lplan.ow);
  cy = (int) (loupe_fy * lplan.oh);
  rx = cx - n / 2;
  ry = cy - n / 2;
  x0 = rx < 0 ? 0 : rx; y0 = ry < 0 ? 0 : ry;
  x1 = rx + n > lplan.ow ? lplan.ow : rx + n;
  y1 = ry + n > lplan.oh ? lplan.oh : ry + n;
  w = x1 - x0;
  h = y1 - y0;
  if (w > 0 && h > 0)
    {
      geom_region_rgb8 (&lplan, zfull.rgb, dev_srgb_to_linear (), loupe_lin, x0, y0, w, h);
      develop_region (dev, lplan.ow, lplan.oh, x0, y0);
      develop_rows (dev, loupe_lin, w, 0, h, loupe_px, NULL);
      sharpen_pixels (loupe_px, w, h, photo_long);
    }
  /* Each photo pixel becomes loupe_mag x loupe_mag screen pixels. */
  for (y = 0; y < d; y++)
    {
      int sy = ry + y / loupe_mag;
      for (x = 0; x < d; x++)
        {
          int sx = rx + x / loupe_mag;
          out[(size_t) y * d + x] = sx >= x0 && sx < x1 && sy >= y0 && sy < y1
            ? loupe_px[(size_t) (sy - y0) * w + (sx - x0)] : ground;
        }
    }
}

/* Follows the pointer (unless pinned): called on null events. */
static void loupe_track (void)
{
  int ptr[5], st[9], r[4], wx, wy, over;
  if (!loupe_on || loupe_pinned || zoom || crop_mode || picking)
    return;
  st[0] = win;
  if (_swix (Wimp_GetPointerInfo, _IN (1), ptr) || _swix (Wimp_GetWindowState, _IN (1), st))
    return;
  wx = ptr[0] - st[1];
  wy = ptr[1] - st[4];
  over = ptr[3] == win && photo_rect (r) && wx >= r[0] && wx < r[2] && wy >= r[1] && wy < r[3];
  if (!over)
    {
      if (loupe_shown)
        {
          loupe_shown = 0;
          loupe_force ();
        }
      return;
    }
  if (loupe_shown && wx == loupe_wx && wy == loupe_wy && !loupe_dirty)
    return;
  if (loupe_shown)
    loupe_force ();                    /* where it was */
  loupe_wx = wx;
  loupe_wy = wy;
  loupe_fx = (double) (wx - r[0]) / (r[2] - r[0]);
  loupe_fy = (double) (r[3] - wy) / (r[3] - r[1]);
  loupe_shown = 1;
  loupe_render ();
  loupe_force ();
}

/* Reads the full-size original if it isn't in memory yet. */
static void loupe_need_full (void)
{
  char e[200];
  if (zfull.rgb || zgen)
    return;
  if (!(zgen = start_full (e, sizeof e)))
    {
      loupe_on = 0;
      app_report (e);
    }
}

void viewer_toggle_loupe (void)
{
  if (!src)
    return;
  if (comparing && !loupe_on)
    compare_stop ();
  loupe_on = !loupe_on;
  loupe_pinned = 0;
  if (loupe_shown)
    loupe_force ();
  loupe_shown = 0;
  if (loupe_on)
    {
      if (zoom)
        zoom_to (0, 0);
      loupe_need_full ();
    }
  force_panel ();
}

int viewer_loupe (void) { return loupe_on ? loupe_mag : 0; }

void viewer_set_loupe_mag (int mag)
{
  loupe_mag = mag == 2 || mag == 4 ? mag : 1;
  if (loupe_shown)
    {
      loupe_render ();
      loupe_force ();
    }
}

static void draw_loupe (int ox, int oy)
{
  int cx = ox + loupe_wx, cy = oy + loupe_wy, rad = loupe_os_w () / 2, k;
  char t[16];
  if (!loupe_shown || !loupe_area || zoom)
    return;
  draw_sprite_masked (loupe_area, loupe_area + 4, cx - loupe_os_w () / 2, cy - loupe_os_h () / 2);
  /* A bezel: dark edges round a light grey ring. */
  {
    int px = 1 << draw_xeig ();
    for (k = 0; k <= 7; k++)
      draw_circle (cx, cy, rad + k * px, k == 0 || k == 7 ? 0x20202000u : k < 3 ? 0xF0F0F000u : 0xC4C4C400u);
  }
  snprintf (t, sizeof t, "%d%%%s", loupe_mag * 100, loupe_pinned ? " \xB7 pinned" : "");
  {
    int tw = draw_text_width (t);
    draw_rect (cx - tw / 2 - 12, cy - rad + 8, cx + tw / 2 + 12, cy - rad + 48, 0x20202000u);
    draw_text (cx - tw / 2, cy - rad + 18, t, COL_LIGHT, 0x20202000u);
  }
}

static void redevelop (void);

static void refresh (int full)
{
  if (!src)
    return;
  /* Raw noise reduction changed (not while its slider is held): the
     picture is developed again by the plug-in. */
  if (raw_nr_photo () && (int) current.v[P_NOISE_RAW] != loaded_noise && dragging != P_NOISE_RAW)
    {
      redevelop ();
      return;
    }
  if (retouch_sync ())
    full = 1;
  if (geometry_changed () && geometry_apply ())
    {
      force_image ();
      return;
    }
  prepare ();
  render_half ();
  if (full)
    restart_full ();
  if (zoom == 2)
    zoom_layout (1);                   /* geometry may have changed the output */
  lplan_ok = 0;
  if (loupe_shown)
    {
      loupe_render ();                 /* follows the sliders */
      loupe_force ();
    }
}

/* Half size: the mean of each 2x2 block of src, in linear light. */
static void make_half (void)
{
  int x, y;
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
}

/* The proxy with the spots the recipe has now (src from src_orig): 1 if
   it changed, and then everything made from it is made again. */
static int retouch_sync (void)
{
  recipe r;
  size_t bytes = (size_t) sw * sh * 6;
  int show = retouching && brushing && mk_show && !before ? mk_sel : -1;
  effective (&r);
  if (!src || (applied_n >= 0 && recipe_spots_equal (&applied, &r) && recipe_masks_equal (&applied, &r)
               && recipe_strokes_equal (&applied, &r) && show == applied_show))
    return 0;
  applied_show = show;
  if (!src_orig)
    {
      if (!r.nspots && !r.nmasks)
        {
          applied = r;
          applied_n = 0;
          return 0;
        }
      if (!(src_orig = malloc (bytes)))
        return 0;
      memcpy (src_orig, src, bytes);
    }
  else
    memcpy (src, src_orig, bytes);
  retouch_lin_show (src, sw, sh, &r, show);
  make_half ();
  applied = r;
  applied_n = 1;
  geo_key_valid = 0;
  lummap_ok = 0;
  if (zfull.rgb || zgen)
    free_zoom ();                      /* the full size is read again, with them */
  return 1;
}

/* Proxy decoded: linear copies and the half-size copy. */
static int proxy_ready (void)
{
  rgb_image img;
  int x, y;
  raw_info_ok = photo_is_raw && thumbgen_raw_details (tg, &raw_info) == 1;
  if (raw_info_ok)
    vlens_id = 0;                      /* read again, with it */
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
  /* Opened from the file: kept as its preview, if previews are made for
     photos like it, so it opens straight away next time. */
  if (!from_kept && photo_original > 0)
    lib_preview_save (app_library (), photo_original, &img);
  dev_to_linear (img.rgb, src, (size_t) sw * sh);
  free (img.rgb);
  (void) x; (void) y;
  make_half ();
  retouch_sync ();                     /* the photo's spots */
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
  char text[RECIPE_TEXT_MAX];
  cat_get_recipe (lib_cat (app_library ()), item.id, text, sizeof text);
  recipe_parse (&current, text);
  undo_stack[0] = current;
  undo_n = 1;
  undo_i = 0;
}

/* The picture to work on: the library's preview of the photo if it has an
   up-to-date one (a raw opens at once rather than being developed again),
   else the file. A raw with noise reduction of its own is developed with
   it (the preview has none). */
static thumbgen *open_proxy (const cat_details *d, char *e, size_t en)
{
  cat_origin o;
  char kept[1300];
  thumbgen *t;
  photo_original = d->original;
  photo_is_raw = cat_origin_of (lib_cat (app_library ()), d->original, &o) == 1 && img_is_plugin_format (o.format);
  photo_kind = photo_is_raw ? rawjob_kind_for (o.format, o.path, NULL, 0) : 0;
  from_kept = 0;
  loaded_noise = photo_is_raw && photo_kind == RAWJOB_RAW ? (int) current.v[P_NOISE_RAW] : 0;
  if (loaded_noise > 0)
    {
      rawjob_set_raw_noise (loaded_noise);
      t = thumbgen_start (d->path, NULL, PROXY_SIDE, d->orientation, e, en);
      rawjob_set_raw_noise (0);
      thumbgen_hurry (t);
      return t;
    }
  if (!d->missing && lib_preview_find (app_library (), d->original, kept, sizeof kept)
      && (t = thumbgen_start (kept, NULL, PROXY_SIDE, 1, e, en)))
    {
      from_kept = 1;
      return t;
    }
  /* Being made in the background just now: carried on here, first. */
  if ((t = lib_preview_adopt (app_library (), d->original)) != NULL)
    {
      thumbgen_hurry (t);
      return t;
    }
  t = thumbgen_start (d->path, NULL, PROXY_SIDE, d->orientation, e, en);
  thumbgen_hurry (t);                  /* ahead of thumbnails and previews waiting for the plug-in */
  return t;
}

/* The photo developed again from its file (its recipe and undo kept). */
static void redevelop (void)
{
  cat_details d;
  char e[200];
  free_photo ();
  state = V_LOADING;
  err[0] = '\0';
  if (cat_get_details (lib_cat (app_library ()), item.id, &d) || !(tg = open_proxy (&d, e, sizeof e)))
    {
      state = V_FAILED;
      snprintf (err, sizeof err, "%s", d.path[0] ? e : "This photo is no longer in the library.");
    }
  update_title ();
  force_all ();
  fit_panel ();
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
  else if (snprintf (photo_path, sizeof photo_path, "%s", d.path), photo_orientation = d.orientation,
           photo_long = d.width > d.height ? d.width : d.height, !(tg = open_proxy (&d, e, sizeof e)))
    {
      fsys_entry fe;
      state = V_FAILED;
      if (fsys_stat (d.path, &fe) != 0)
        {
          /* Gone from where it was: say what to do, and mark it. */
          snprintf (err, sizeof err, "This photo's file can't be found. If it has moved, drag the folder it is "
                    "in now into Lumen, and Lumen will find it there.");
          if (!d.missing)
            {
              cat_set_missing (lib_cat (app_library ()), d.original, 1);
              item.missing = 1;
            }
        }
      else
        snprintf (err, sizeof err, "%s", e);
    }
  update_title ();
  force_all ();
  fit_panel ();
}

/* --- compare */

static void compare_stop (void)
{
  comparing = 0;
  compare_pending = -1;
  sprite_free (&cmp_spr);
  update_title ();
  force_all ();
}

/* The render shown is done: pin it, then show the photo asked for. */
static void compare_pin_now (void)
{
  int next = compare_pending, n = browser_count ();
  compare_pending = -1;
  if (state != V_READY || !full_spr.area || full_y >= 0)
    return;
  sprite_free (&cmp_spr);
  if (sprite_make (&cmp_spr, full_spr.w, full_spr.h))
    {
      app_report ("There isn't enough memory to compare photos.");
      return;
    }
  memcpy (cmp_spr.pixels, full_spr.pixels, (size_t) full_spr.w * full_spr.h * 4);
  cmp_item = item;
  comparing = 1;
  if (next == -2)                      /* the next photo (or the one before, at the end) */
    next = index_in_list + 1 < n ? index_in_list + 1 : index_in_list - 1;
  if (next >= 0 && next < n && next != index_in_list)
    {
      browser_focus_on (next);
      if (index_in_list != next)
        viewer_show (next);
    }
  update_title ();
  force_all ();
}

/* O: the photo shown pinned on the left, the next beside it to compare. */
void viewer_compare (void)
{
  if (state == V_CLOSED || browser_count () < 2)
    {
      app_report ("Comparing needs two photos or more in the Browser.");
      return;
    }
  if (crop_mode)
    viewer_crop_finish (1);
  if (zoom)
    zoom_to (0, 0);
  if (loupe_on)
    viewer_toggle_loupe ();
  compare_pending = -2;
  if (state == V_READY && full_y < 0 && full_spr.area)
    compare_pin_now ();                /* else when its render is done */
}

/* Two photos (the Browser's indexes) side by side: a pinned, b beside it. */
void viewer_compare_pair (int a, int b)
{
  if (comparing)
    compare_stop ();
  browser_focus_on (a);                /* the Viewer shows the focused photo */
  viewer_open (a);
  if (state == V_CLOSED)
    return;
  compare_pending = b;
  if (state == V_READY && full_y < 0 && full_spr.area)
    compare_pin_now ();
}

int viewer_comparing (void) { return comparing; }
cat_id viewer_compared_item (void) { return comparing ? cmp_item.id : 0; }

/* The Browser's list changed (a stack closed, a search, a smart album):
   the photo shown keeps its place in it; if it has gone from the list, the
   one standing for its stack, or the one at its old place, is shown; with
   none left, back to the Browser. */
void viewer_list_changed (void)
{
  int i, n = browser_count ();
  cat_item it;
  if (state == V_CLOSED)
    return;
  for (i = 0; i < n; i++)
    if (browser_item (i, &it) && it.id == item.id)
      {
        if (i != index_in_list)
          {
            index_in_list = i;
            update_title ();
            force_panel ();
          }
        return;
      }
  if (mainwin_mode () == LAYOUT_BROWSER)
    {
      index_in_list = -1;              /* shown again from the Browser when opened */
      return;
    }
  if (crop_mode)
    viewer_crop_finish (1);
  for (i = 0; item.stack && i < n; i++)
    if (browser_item (i, &it) && it.stack == item.stack)
      {
        viewer_show (i);
        return;
      }
  if (n > 0)
    viewer_show (index_in_list >= 0 && index_in_list < n ? index_in_list : n - 1);
  else
    {
      mainwin_set_mode (LAYOUT_BROWSER);
      viewer_shutdown ();
    }
}

/* A photo opened from the Browser: the Viewer layout. */
void viewer_open (int i)
{
  viewer_show (i);
  if (state != V_CLOSED)
    mainwin_set_mode (LAYOUT_VIEWER);
}

/* Back to the Browser, the photo shown focused there. */
void viewer_close (void)
{
  int i = index_in_list;
  if (comparing)
    compare_stop ();
  if (mainwin_full_screen ())
    {
      mainwin_toggle_full_screen ();   /* Escape in full screen: back to the layout before */
      return;
    }
  if (crop_mode)
    viewer_crop_finish (1);
  mainwin_set_mode (LAYOUT_BROWSER);
  if (i >= 0)
    browser_focus_on (i);
}

/* The main window closed: let the photo go. */
void viewer_shutdown (void)
{
  if (comparing)
    compare_stop ();
  compare_pending = -1;
  viewer_unplace ();
  free_photo ();
  state = V_CLOSED;
  force_panel ();
}

int viewer_loading (void)
{
  return is_open && state == V_LOADING && tg != NULL;
}

/* A raw opened from its preview: the plug-in's quick look for its lens
   is to be had (Lens Correction). */
static int info_wanted (void)
{
  return info_asked && state == V_READY && from_kept && photo_is_raw && photo_kind == RAWJOB_RAW && !raw_info_ok
         && !info_tried && app_has_lens_db () && rawjob_available_kind (photo_kind);
}

int viewer_busy (void)
{
  return is_open && (dragging != -1 || info_gen || info_wanted () || (state == V_LOADING && tg) || full_y >= 0 || zgen || (zoom == 2 && zy >= 0)
                     || (loupe_on && !loupe_pinned && zfull.rgb) || retouching);
}

static void crop_follow (double fx, double fy);
static void curve_follow (int wx, int wy);

/* Called on null events while busy. */
void viewer_idle (void)
{
  unsigned deadline = app_now () + 2;
  if (!is_open)
    return;
  if (dragging != -1)
    {
      int ptr[5], b[9];
      b[0] = drag_win;
      if (_swix (Wimp_GetPointerInfo, _IN (1), ptr) == NULL && (ptr[0] != drag_x || ptr[1] != drag_y)
          && _swix (Wimp_GetWindowState, _IN (1), b) == NULL)
        {
          b[1] -= b[5];                /* the work area origin (the inspector scrolls) */
          b[4] -= b[6];
          drag_x = ptr[0];
          drag_y = ptr[1];
          if (dragging == DRAG_CURVE)
            curve_follow (ptr[0] - b[1], ptr[1] - b[4]);
          else if (dragging == DRAG_PAN)
            {
              pan_dx = ptr[0] - pan_start_x;
              pan_dy = ptr[1] - pan_start_y;
              force_image ();
            }
          else if (dragging == DRAG_RT_SIZE || dragging == DRAG_RT_FEATHER || dragging == DRAG_RT_STRENGTH)
            {
              int k = dragging == DRAG_RT_SIZE ? RT_SIZE : dragging == DRAG_RT_FEATHER ? RT_FEATHER : RT_STRENGTH;
              *rt_slider_var (k) = rt_slider_value (k, ptr[0] - b[1]);
              rt_apply (1);
            }
          else if (dragging == DRAG_STROKE)
            stroke_follow (ptr[0] - b[1], ptr[1] - b[4]);
          else if (dragging <= DRAG_MASK_SLIDER && dragging > DRAG_MASK_SLIDER - M_COUNT)
            {
              int k = DRAG_MASK_SLIDER - dragging;
              float v = mb_slider_value (k, ptr[0] - b[1]);
              recipe_mask *m = mb_mask ();
              if (fabsf (v - m->v[k]) >= recipe_mask_params[k].step / 2)
                mask_value (k, v, 1);
            }
          else if (dragging == DRAG_HANDLE || dragging == DRAG_SHAPE)
            {
              int r[4];
              float u, v;
              if (photo_rect (r) && screen_to_photo (r, ptr[0] - b[1], ptr[1] - b[4], &u, &v))
                {
                  u = u < 0 ? 0 : u > 1 ? 1 : u;
                  v = v < 0 ? 0 : v > 1 ? 1 : v;
                  if (dragging == DRAG_HANDLE)
                    handle_follow (u, v);
                  else
                    {
                      rt_dx = u;
                      rt_dy = v;
                      force_image ();
                    }
                }
            }
          else if (dragging == DRAG_SPOT)
            {
              int r[4];
              float u, v;
              if (photo_rect (r) && screen_to_photo (r, ptr[0] - b[1], ptr[1] - b[4], &u, &v))
                {
                  rt_dx = u < 0 ? 0 : u > 1 ? 1 : u;
                  rt_dy = v < 0 ? 0 : v > 1 ? 1 : v;
                  force_image ();      /* the circles follow; the photo when let go */
                }
            }
          else if (dragging == DRAG_CROP)
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
      if (zoom == 2 && zy >= 0 && dragging != DRAG_PAN)
        zoom_render_until (app_now () + 4);   /* the 100% view follows the slider */
      return;
    }
  /* The full size: wanted now (100% or the loupe) before anything else;
     read ahead for a raw only once the picture on screen is finished. The
     rest goes on meanwhile (the loupe follows the pointer, showing the
     picture on screen enlarged until the full size comes). */
  if (zgen || (zoom == 2 && zy >= 0))
    {
      int wanted = zoom || loupe_on;
      if (wanted || (full_y < 0 && state == V_READY))
        zoom_idle (wanted ? deadline : app_now () + 1);
    }
  if (info_gen)
    {
      int r = thumbgen_step (info_gen, 32);
      if (r != 0)
        {
          if (r > 0 && thumbgen_raw_details (info_gen, &raw_info) == 1)
            {
              raw_info_ok = 1;
              vlens_id = 0;
            }
          thumbgen_free (info_gen);
          info_gen = NULL;
          if (auto_after_info)
            {
              auto_after_info = 0;
              viewer_lens_auto ();
            }
        }
    }
  else if (info_wanted ())
    {
      /* The lens, for Lens Correction: the plug-in's quick look at the raw
         (its camera's preview), not the slow full development. */
      char e[200];
      info_tried = 1;
      info_gen = thumbgen_start (photo_path, NULL, THUMBGEN_PREVIEW_SIDE, photo_orientation, e, sizeof e);
      if (info_gen)
        thumbgen_hurry (info_gen);
    }
  if (!info_gen && !zgen && !(zoom == 2 && zy >= 0) && photo_raw && state == V_READY && full_y < 0 && !zfull.rgb && !crop_mode && !comparing && dragging == -1)
    {
      char e[200];
      zgen = start_full (e, sizeof e);   /* quietly: asked for again if it fails */
    }
  loupe_track ();
  rt_brush_follow ();
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
          photo_raw = from_kept ? photo_is_raw && rawjob_available_kind (photo_kind) : thumbgen_raw_developed (tg);
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
  char text[RECIPE_TEXT_MAX];
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
  if (state == V_CLOSED || id != item.id || crop_mode)
    return;
  {
    char text[RECIPE_TEXT_MAX];
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
  if (state == V_CLOSED)
    return;
  if (previewing)
    {
      current = preview_saved;         /* chosen after a preview: from the photo's own recipe */
      previewing = 0;
    }
  recipe_parse (&r, look);
  recipe_copy_look (&current, &r);
  commit ();
  changed_everything ();
}

/* Shows a look on the photo without keeping it (a style under the pointer
   in the Presets menu): nothing is saved and nothing goes on the undo
   list. NULL puts the photo's own recipe back. */
void viewer_preview_look (const char *look)
{
  recipe r;
  if (state == V_CLOSED || crop_mode)
    return;
  if (!look)
    {
      if (!previewing)
        return;
      previewing = 0;
      current = preview_saved;
      changed_everything ();
      return;
    }
  if (!previewing)
    {
      preview_saved = current;
      previewing = 1;
    }
  current = preview_saved;
  recipe_parse (&r, look);
  recipe_copy_look (&current, &r);
  changed_everything ();
}

int viewer_previewing (void) { return previewing; }

/* The look as text (what a preset or Copy keeps). */
void viewer_look (char *out, size_t n)
{
  recipe_format_look (&current, out, n);
}

cat_id viewer_item (void) { return state != V_CLOSED ? item.id : 0; }

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

/* Noise Reduction's Auto: the photo's noise measured on it developed
   (before noise reduction), at the proxy's size. */
static void auto_nr (void)
{
  uint32_t *px;
  int lum, col;
  if (!src || !geo || gw <= 0 || gh <= 0 || !(px = malloc ((size_t) gw * gh * 4)))
    return;
  develop_region (dev, gw, gh, 0, 0);
  develop_rows (dev, geo, gw, 0, gh, px, NULL);
  denoise_estimate (px, gw, gh, photo_long > 0 ? (double) (gw > gh ? gw : gh) / photo_long : 1, &lum, &col);
  free (px);
  recipe_set (&current, P_NOISE_LUM, (float) lum);
  recipe_set (&current, P_NOISE_COL, (float) col);
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
  retouching = 0;
  if (comparing)
    compare_stop ();
  if (zoom)
    zoom_to (0, 0);
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
  db[0] = drag_win;
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

static void link_click (int g, int id, int mx, int my)
{
  int p;
  switch (id)
    {
    case LINK_RESET:
      for (p = recipe_groups[g].first; p <= recipe_groups[g].last; p++)
        current.v[p] = recipe_params[p].def;
      if (recipe_groups[g].curves)
        memset (current.curve, 0, sizeof current.curve);
      if (recipe_groups[g].first == P_LENS_DIST)
        memset (&current.lens, 0, sizeof current.lens);   /* and the profile */
      if (recipe_groups[g].first == P_ROTATE && !crop_mode)
        for (p = P_CROP_X0; p <= P_CROP_Y1; p++)   /* the crop was of the turned picture */
          current.v[p] = recipe_params[p].def;
      if (!crop_mode)
        commit ();
      changed_everything ();
      break;
    case LINK_PICK: viewer_pick_white (); break;
    case LINK_AUTO_WB: auto_wb (); break;
    case LINK_AUTO_NR: auto_nr (); break;
    case LINK_AUTO_LENS: viewer_lens_auto (); break;
    case LINK_LENS_MENU: if (src) menus_open_lens_menu (mx, my, 0); break;
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
    case TOOL_RETOUCH: viewer_toggle_retouch (); break;
    case TOOL_BRUSH: viewer_toggle_brush (); break;
    }
}

static void force_curves (void)
{
  int r[4];
  if (curve_graph (r))
    pforce (r[0] - 12, r[1] - 12, r[2] + 12, r[3] + 12);
}

/* Select: drag the point there, or add one; Adjust: remove the point. */
static void curve_click (int *b, const int *st, int wx, int wy)
{
  int r[4], i, best = -1;
  double fx, fy, bd = 1e9;
  recipe_curve *cv = &current.curve[curve_ch];
  if (!curve_graph (r) || !src)
    return;
  fx = (double) (wx - r[0]) / (r[2] - r[0]);
  fy = (double) (wy - r[1]) / (r[3] - r[1]);
  fx = fx < 0 ? 0 : fx > 1 ? 1 : fx;
  fy = fy < 0 ? 0 : fy > 1 ? 1 : fy;
  for (i = 0; i < cv->n; i++)
    {
      double dx = (cv->x[i] - fx) * (r[2] - r[0]), dy = (cv->y[i] - fy) * (r[3] - r[1]);
      double d2 = dx * dx + dy * dy;
      if (d2 < bd)
        {
          bd = d2;
          best = i;
        }
    }
  if (best >= 0 && bd > 24 * 24)
    best = -1;
  if (b[2] == Button_Adjust)
    {
      if (best > 0 && best < cv->n - 1)
        {
          recipe_curve_remove (cv, best);
          if (recipe_curve_is_identity (cv))
            cv->n = 0;
          commit ();
          refresh (1);
          force_curves ();
        }
      return;
    }
  if (b[2] != Button_Select)
    return;
  if (best < 0)
    {
      best = recipe_curve_add (cv, (float) fx);
      if (best < 0)
        return;
    }
  before = 0;
  recipe_curve_move (cv, best, (float) fx, (float) fy);
  curve_drag = best;
  refresh (0);
  force_curves ();
  force_hist ();
  start_drag (b[0], b[1], st[1] + r[0], st[4] + r[1], st[1] + r[2], st[4] + r[3], DRAG_CURVE);
}

static void curve_follow (int wx, int wy)
{
  int r[4];
  if (!curve_graph (r) || curve_drag < 0)
    return;
  recipe_curve_move (&current.curve[curve_ch], curve_drag, (float) (wx - r[0]) / (r[2] - r[0]),
                     (float) (wy - r[1]) / (r[3] - r[1]));
  refresh (0);
  force_curves ();
}

/* A click on the photo (panel 0) or on the panel in the inspector (1). */
static void click_in (int *b, int w, int panel)
{
  int st[9], wx, wy, what = -1, h;
  st[0] = w;
  if (state == V_CLOSED || !is_open || _swix (Wimp_GetWindowState, _IN (1), st))
    return;
  if (b[2] == Button_Menu)
    {
      menus_open_viewer_menu (b[0], b[1]);
      return;
    }
  _swix (Wimp_SetCaretPosition, _INR (0, 5), mainwin_window (), -1, 0, 0, 1 << 25, -1);
  st[1] -= st[5];                      /* the work area origin, on the screen */
  st[4] -= st[6];
  drag_win = w;
  wx = b[0] - st[1];
  wy = b[1] - st[4];
  h = hit (wx, wy, &what, panel);
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
  else if (h >= HIT_MASK_LINE && h <= HIT_MASK_RT)
    masks_panel_click (b, h, what, wx, st);
  else if (h == HIT_IMAGE && retouching && (b[2] == Button_Select || b[2] == Button_Adjust))
    retouch_click (b, wx, wy);
  else if (h == HIT_IMAGE && b[2] == Button_Adjust && loupe_on)
    viewer_set_loupe_mag (loupe_mag == 4 ? 1 : loupe_mag * 2);
  else if (h == HIT_IMAGE && b[2] == Button_Select)
    {
      int r[4];
      double fx, fy;
      if (loupe_on && !crop_mode && !picking && zoom == 0)
        {
          if (b[2] == Button_Select)
            {
              loupe_pinned = !loupe_pinned;
              if (loupe_shown)
                loupe_force ();
            }
          return;
        }
      if (zoom == 2)
        {
          /* Drag the view about (anywhere on the screen). */
          pan_start_x = b[0];
          pan_start_y = b[1];
          pan_dx = pan_dy = 0;
          start_drag (b[0], b[1], -0x10000000, -0x10000000, 0x10000000, 0x10000000, DRAG_PAN);
          return;
        }
      if (!photo_rect (r) || wx < r[0] - HANDLE || wx > r[2] + HANDLE || wy < r[1] - HANDLE || wy > r[3] + HANDLE)
        return;
      fx = (double) (wx - r[0]) / (r[2] - r[0]);
      fy = (double) (r[3] - wy) / (r[3] - r[1]);
      if (picking)
        {
          if (fx >= 0 && fx <= 1 && fy >= 0 && fy <= 1)
            pick_at (fx, fy);
        }
      else if (!crop_mode && !zoom)
        {
          /* A click on the photo: 100% there. */
          if (fx >= 0 && fx <= 1 && fy >= 0 && fy <= 1)
            zoom_to (fx, fy);
        }
      else if (crop_mode)
        {
          crop_edges = crop_grab (fx, fy, (double) HANDLE / (r[2] - r[0]), (double) HANDLE / (r[3] - r[1]));
          start_drag (b[0], b[1], st[1] + r[0], st[4] + r[1], st[1] + r[2], st[4] + r[3], DRAG_CROP);
        }
    }
  else if (h == HIT_CURVE_TAB && b[2] == Button_Select)
    {
      curve_ch = what;
      force_panel ();
    }
  else if (h == HIT_LABEL && b[2] == Button_Select && item.id)
    {
      /* That label (the one it has already: none). */
      int lab = what == item.label ? 0 : what;
      cat_set_label (lib_cat (app_library ()), &item.id, 1, lab);
      item.label = lab;
      browser_after_change ();
      mainwin_info_changed ();
      pforce (0, info_top () - INFO_H, PANEL_W, info_top ());
    }
  else if (h == HIT_HSL_BAND && (b[2] == Button_Select || b[2] == Button_Adjust))
    {
      hsl_band = what;
      force_panel ();
    }
  else if (h == HIT_CURVE)
    curve_click (b, st, wx, wy);
  else if (h == HIT_LINK && b[2] == Button_Select)
    link_click (what / 8, what % 8, b[0], b[1]);
  else if (h == HIT_LENS_CAMERA && b[2] == Button_Select && src)
    menus_open_lens_menu (b[0], b[1], 1);
  else if (h == HIT_GROUP && what < MAX_GROUPS && b[2] == Button_Select)
    {
      folded[what] = !folded[what];
      folded_by_hand[what] = 1;
      fit_panel ();
      force_panel ();
    }
  else if (h == HIT_GROUP && what < MAX_GROUPS && b[2] == Button_Adjust)
    {
      /* Adjust: like a concertina, this group open and the others folded
         (on the only one open: all open again). */
      int g, others = 0;
      for (g = 0; g < recipe_ngroups && g < MAX_GROUPS; g++)
        if (g != what && group_shown (g) && !folded[g])
          others = 1;
      for (g = 0; g < recipe_ngroups && g < MAX_GROUPS; g++)
        {
          folded[g] = others || folded[what] ? g != what : 0;
          folded_by_hand[g] = 1;
        }
      masks_folded = others || folded[what] ? 1 : 0;
      fit_panel ();
      force_panel ();
    }
  else if (h == HIT_FOLD_ALL && b[2] == Button_Select)
    {
      int g;
      for (g = 0; g < recipe_ngroups && g < MAX_GROUPS; g++)
        {
          folded[g] = !what;
          folded_by_hand[g] = 1;
        }
      masks_folded = !what;
      fit_panel ();
      force_panel ();
    }
  else if (h == HIT_TOOL && b[2] == Button_Select)
    tool_click (what, b[0], b[1]);
  else if (h == HIT_UNDO && b[2] == Button_Select)
    viewer_undo ();
  else if (h == HIT_REDO && b[2] == Button_Select)
    viewer_redo ();
  else if (h == HIT_RESET_ALL && b[2] == Button_Select)
    viewer_reset_all ();
}

void viewer_click (int *b) { click_in (b, win, 0); }
void viewer_panel_click (int *b) { click_in (b, mainwin_inspector (), 1); }

/* User_Drag_Box: the button was let go. */
void viewer_drag_end (void)
{
  int p = dragging;
  if (dragging == -1)
    return;
  dragging = -1;
  if (p == DRAG_CURVE)
    {
      curve_drag = -1;
      if (recipe_curve_is_identity (&current.curve[curve_ch]))
        current.curve[curve_ch].n = 0;
      commit ();
      force_curves ();
      restart_full ();
      return;
    }
  if (p == DRAG_STROKE)
    {
      stroke_end ();
      return;
    }
  if (p <= DRAG_MASK_SLIDER && p > DRAG_MASK_SLIDER - M_COUNT)
    {
      int k = DRAG_MASK_SLIDER - p;
      mask_value (k, mb_mask ()->v[k], 0);
      return;
    }
  if (p == DRAG_HANDLE)
    {
      spots_changed ();
      return;
    }
  if (p == DRAG_SHAPE)
    {
      shape_end (rt_dx, rt_dy);
      return;
    }
  if (p == DRAG_RT_SIZE || p == DRAG_RT_FEATHER || p == DRAG_RT_STRENGTH)
    {
      rt_apply (0);
      return;
    }
  if (p == DRAG_SPOT)
    {
      if (rt_drag >= 0 && rt_drag < current.nspots)
        {
          /* The spot moves (its source keeps its place beside it), or
             only its source. */
          recipe_spot_move (&current, rt_drag, rt_dx - rt_gu, rt_dy - rt_gv, rt_drag_src);
          rt_drag = -1;
          spots_changed ();
        }
      rt_drag = -1;
      return;
    }
  if (p == DRAG_PAN)
    {
      /* Moving the pointer right shows what was to the left. */
      zcx -= (double) pan_dx / (1 << draw_xeig ());
      zcy += (double) pan_dy / (1 << draw_yeig ());
      pan_dx = pan_dy = 0;
      if (abs (pan_start_x - drag_x) < 8 && abs (pan_start_y - drag_y) < 8)
        zoom_to (0, 0);                /* a click, not a drag: back to fit */
      else
        zoom_layout (0);
      return;
    }
  if (p >= 0)
    {
      force_row (p);
      if (p >= P_FIRST_GEOMETRY)
        force_image ();                /* the straightening grid goes */
      if (!crop_mode || p < P_FIRST_GEOMETRY)
        commit ();
      if (p == P_NOISE_RAW)
        {
          refresh (1);                 /* developed again with it */
          return;
        }
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
  if (zoom && key == KEY_ESCAPE)
    {
      zoom_to (0, 0);
      return 1;
    }
  if (loupe_on && key == KEY_ESCAPE)
    {
      viewer_toggle_loupe ();
      return 1;
    }
  if (retouching && (key == KEY_ESCAPE || key == KEY_RETURN))
    {
      local_tool (brushing);
      return 1;
    }
  if (retouching && (key == '[' || key == ']'))
    {
      rt_size = key == ']' ? (rt_size * 1.25f > rt_size_max () ? rt_size_max () : rt_size * 1.25f)
                           : (rt_size / 1.25f < RT_SIZE_MIN ? RT_SIZE_MIN : rt_size / 1.25f);
      rt_apply (0);
      return 1;
    }
  if (retouching && (key == '{' || key == '}'))
    {
      rt_feather += key == '}' ? 0.1f : -0.1f;
      rt_feather = rt_feather < 0 ? 0 : rt_feather > 1 ? 1 : rt_feather;
      rt_apply (0);
      return 1;
    }
  if (comparing && key == KEY_ESCAPE)
    {
      compare_stop ();
      return 1;
    }
  if (comparing && key == KEY_RETURN)
    {
      viewer_compare ();               /* this one pinned instead, the next beside it */
      return 1;
    }
  switch (key)
    {
    case 'r':
    case 'R':
      viewer_toggle_retouch ();
      return 1;
    case 'b':
    case 'B':
      viewer_toggle_brush ();
      return 1;
    case 'o':
    case 'O':
      if (comparing)
        compare_stop ();
      else
        viewer_compare ();
      return 1;
    case KEY_LEFT:
    case KEY_RIGHT:
      {
        int i = index_in_list + (key == KEY_LEFT ? -1 : 1);
        if (crop_mode)
          viewer_crop_finish (1);
        if (i >= 0 && i < browser_count ())
          {
            browser_focus_on (i);      /* in Split View that shows it here too */
            if (index_in_list != i)
              viewer_show (i);
          }
        return 1;
      }
    case KEY_ESCAPE:
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
    case 'z':
    case 'Z':
      if (loupe_on)
        viewer_toggle_loupe ();
      viewer_toggle_zoom ();
      return 1;
    case '`':
      viewer_toggle_loupe ();
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
    case 0x04:                         /* Ctrl-D: a duplicate version, shown */
      viewer_new_version (1);
      return 1;
    case 0x0B:                         /* Ctrl-K: caption and keywords */
      {
        int ptr[5];
        if (_swix (Wimp_GetPointerInfo, _IN (1), ptr) == NULL)
          tagbox_open (1, ptr[0], ptr[1]);
      }
      return 1;
    case 0x10:                         /* Ctrl-P: print */
      {
        int ptr[5];
        if (crop_mode)
          viewer_crop_finish (1);
        if (_swix (Wimp_GetPointerInfo, _IN (1), ptr) == NULL)
          printbox_open (1, ptr[0], ptr[1]);
      }
      return 1;
    case 0x05:                         /* Ctrl-E: export */
      {
        int ptr[5];
        if (crop_mode)
          viewer_crop_finish (1);
        if (_swix (Wimp_GetPointerInfo, _IN (1), ptr) == NULL)
          exportbox_open (1, ptr[0], ptr[1]);
      }
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
      pforce (0, info_top () - INFO_H, PANEL_W, info_top ());
      mainwin_info_changed ();
      return 1;
    }
  return 0;
}

/* --- drawing */
/* A disclosure triangle: pointing down when open, right when folded. */
static void draw_disclosure (int cx, int cy, int is_folded)
{
  int ax, ay, bx, by, qx, qy;
  if (is_folded)
    {
      ax = cx - 7; ay = cy + 10; bx = cx - 7; by = cy - 10; qx = cx + 9; qy = cy;
    }
  else
    {
      ax = cx - 10; ay = cy + 7; bx = cx + 10; by = cy + 7; qx = cx; qy = cy - 9;
    }
  draw_triangle (ax, ay, bx, by, qx, qy, COL_QUIET);
}

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
  if (p >= P_LENS_DIST && p <= P_LENS_VIG)
    snprintf (value, sizeof value, "%.0f%%", (double) current.v[p]);
  if (p == P_NOISE_RAW)
    {
      static const char *const levels[3] = { "Off", "Light", "Strong" };
      int k = (int) current.v[p];
      snprintf (value, sizeof value, "%s", levels[k < 0 ? 0 : k > 2 ? 2 : k]);
    }
  draw_text (ox + PANEL_W - PAD - draw_text_width (value), mid - 9, value, COL_TEXT, COL_PANEL);
}

/* The Colour group's swatches: the chosen band framed, a dot on those changed. */
static void draw_hsl_bands (int ox, int oy)
{
  static const unsigned colours[HSL_BANDS] = {
    0x3C3CDC00u, 0x288CEB00u, 0x32D2E600u, 0x46AA4600u, 0xBEBE3C00u, 0xDC6E3C00u, 0xC8508C00u, 0xAA46D200u };
  int k, r[4];
  for (k = 0; k < HSL_BANDS; k++)
    {
      int c, changed = 0;
      if (!hsl_swatch (k, r))
        return;
      for (c = 0; c < 3; c++)
        if (current.v[HSL_PARAM (k, c)] != 0)
          changed = 1;
      if (k == hsl_band)
        draw_rect (ox + r[0] - 5, oy + r[1] - 5, ox + r[2] + 5, oy + r[3] + 5, COL_TEXT);
      draw_rect (ox + r[0] - 1, oy + r[1] - 1, ox + r[2] + 1, oy + r[3] + 1, COL_FRAME);
      draw_rect (ox + r[0], oy + r[1], ox + r[2], oy + r[3], colours[k]);
      if (changed)
        draw_rect (ox + (r[0] + r[2]) / 2 - 5, oy + (r[1] + r[3]) / 2 - 5, ox + (r[0] + r[2]) / 2 + 5,
                   oy + (r[1] + r[3]) / 2 + 5, COL_KNOB);
    }
  hsl_swatch (0, r);
  draw_text (ox + r[0], oy + r[1] - 40, recipe_hsl_bands[hsl_band], COL_TEXT, COL_PANEL);
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

/* The histogram: red, green and blue laid over
   each other on a dark ground, where they overlap the colours add (red
   and green make yellow, all three a light grey), so the shape of each
   channel and of the whole stays readable. One column per screen pixel,
   smoothed; scaled to the tallest column away from the ends (the
   ends hold clipped pixels, which would flatten the rest); quarter
   guides behind; a mark in the top corner when shadows or highlights
   clip, in the colour of the channels clipping. */
static const unsigned hist_mix[8] = {
  0, 0x4848D800u, 0x48B84800u, 0x48C8D800u,      /* -, R, G, R+G yellow */
  0xE0704800u, 0xC858C000u, 0xD0C04800u, 0xD8D8D800u   /* B, R+B magenta, G+B cyan, all */
};

static void draw_histogram (int x0, int y0, int x1, int y1)
{
  int px = 1 << draw_xeig (), py = 1 << draw_yeig ();
  int ix0 = x0 + 6, ix1 = x1 - 6, iy0 = y0 + 6, ih = y1 - y0 - 12;
  int cols = (ix1 - ix0) / px, c, k, x;
  unsigned total = 0, peak = 1;
  static unsigned colv[3][1024];
  draw_rect (x0, y0, x1, y1, COL_FRAME);
  draw_rect (x0 + 2, y0 + 2, x1 - 2, y1 - 2, COL_HIST_BG);
  for (k = 1; k < 4; k++)
    draw_rect (ix0 + (ix1 - ix0) * k / 4, iy0, ix0 + (ix1 - ix0) * k / 4 + px, iy0 + ih, COL_HIST_GUIDE);
  if (cols > 1024)
    cols = 1024;
  for (k = 0; k < DEV_HIST_BINS; k++)
    total += hist.c[HIST_LUM][k];
  if (cols < 2 || total == 0)
    return;
  /* Each column: the mean of its bins, then a 1-2-1 blur across columns. */
  for (c = 0; c < 3; c++)
    {
      static unsigned raw[1024];
      for (x = 0; x < cols; x++)
        {
          int b0 = x * DEV_HIST_BINS / cols, b1 = (x + 1) * DEV_HIST_BINS / cols, b;
          unsigned long long sum = 0;
          if (b1 <= b0)
            b1 = b0 + 1;
          for (b = b0; b < b1; b++)
            sum += hist.c[c][b];
          raw[x] = (unsigned) (sum * 16 / (unsigned) (b1 - b0));
        }
      /* Two passes of 1-4-6-4-1 (a gaussian about three columns wide):
         8-bit levels after a tone curve leave a comb of empty and doubled
         bins that says nothing about the photo. */
      {
        int pass;
        for (pass = 0; pass < 2; pass++)
          {
            unsigned *in = pass ? colv[c] : raw, *out = pass ? raw : colv[c];
            for (x = 0; x < cols; x++)
              {
                unsigned a = in[x > 1 ? x - 2 : 0], b = in[x > 0 ? x - 1 : 0];
                unsigned d = in[x < cols - 1 ? x + 1 : cols - 1], e = in[x < cols - 2 ? x + 2 : cols - 1];
                out[x] = (a + 4 * b + 6 * in[x] + 4 * d + e) / 16;
              }
          }
        for (x = 0; x < cols; x++)
          {
            colv[c][x] = raw[x];
            if (x > 3 && x < cols - 4 && colv[c][x] > peak)
              peak = colv[c][x];
          }
      }
    }
  for (x = 0; x < cols; x++)
    {
      int h[3], order[3] = { 0, 1, 2 }, mask = 7, base = 0, i, j;
      for (c = 0; c < 3; c++)
        {
          unsigned v = colv[c][x] > peak ? peak : colv[c][x];
          h[c] = (int) ((unsigned long long) v * (unsigned) ih / peak) / py * py;
        }
      for (i = 0; i < 2; i++)          /* lowest first */
        for (j = i + 1; j < 3; j++)
          if (h[order[j]] < h[order[i]])
            {
              int t = order[i]; order[i] = order[j]; order[j] = t;
            }
      /* Up to the lowest all three overlap; then the other two; then the tallest alone. */
      for (i = 0; i < 3; i++)
        {
          int top = h[order[i]];
          if (top > base)
            draw_rect (ix0 + x * px, iy0 + base, ix0 + x * px + px - 1, iy0 + top - 1, hist_mix[mask]);
          base = top > base ? top : base;
          mask &= ~(1 << order[i]);
        }
    }
  /* Clipping: more than 0.5% of pixels at 0 or 255 in a channel. */
  for (k = 0; k < 2; k++)
    {
      int mask = 0, bin = k ? DEV_HIST_BINS - 1 : 0, mx = k ? ix1 - 14 : ix0 + 2;
      for (c = 0; c < 3; c++)
        if (hist.c[c][bin] * 200u > total)
          mask |= 1 << c;
      if (mask)
        draw_rect (mx, iy0 + ih - 14, mx + 12, iy0 + ih - 2, hist_mix[mask]);
    }
}

static const unsigned curve_col[CURVE_COUNT] = { 0xEEEEEE00u, 0x5050E000u, 0x50C85000u, 0xF0805000u };

static void draw_curves (int ox, int oy)
{
  static const char *const names[CURVE_COUNT] = { "RGB", "Red", "Green", "Blue" };
  int r[4], k, c, x, gw, gh, step = 2 << draw_xeig ();
  unsigned peak = 1;
  for (k = 0; k < CURVE_COUNT; k++)
    {
      int t[4];
      curve_tab_rect (k, t);
      draw_button_rect (t, names[k], 1, k == curve_ch, ox, oy);
    }
  if (!curve_graph (r))
    return;
  r[0] += ox; r[2] += ox; r[1] += oy; r[3] += oy;
  gw = r[2] - r[0];
  gh = r[3] - r[1];
  draw_rect (r[0] - 10, r[1] - 10, r[2] + 10, r[3] + 10, COL_FRAME);
  draw_rect (r[0] - 8, r[1] - 8, r[2] + 8, r[3] + 8, COL_HIST_BG);
  /* The channel's histogram, faint, behind. */
  {
    int ch = curve_ch == CURVE_RGB ? HIST_LUM : curve_ch - 1, b;
    for (b = 2; b < DEV_HIST_BINS - 2; b++)
      if (hist.c[ch][b] > peak)
        peak = hist.c[ch][b];
    for (x = 0; x < gw; x += step)
      {
        int b0 = x * DEV_HIST_BINS / gw, b1 = (x + step) * DEV_HIST_BINS / gw;
        unsigned v = 0;
        for (b = b0; b < b1 && b < DEV_HIST_BINS; b++)
          v = hist.c[ch][b] > v ? hist.c[ch][b] : v;
        if (v > peak)
          v = peak;
        if (v)
          draw_rect (r[0] + x, r[1], r[0] + x + step - 1, r[1] + (int) ((unsigned long long) v * (unsigned) gh / peak), 0x34343400u);
      }
  }
  for (k = 1; k < 4; k++)
    {
      draw_rect (r[0] + gw * k / 4, r[1], r[0] + gw * k / 4 + 1, r[3], COL_HIST_GUIDE);
      draw_rect (r[0], r[1] + gh * k / 4, r[2], r[1] + gh * k / 4 + 1, COL_HIST_GUIDE);
    }
  draw_line (r[0], r[1], r[2], r[3], 0x50505000u);
  /* Every curve that does something, the chosen one last and brightest. */
  for (k = 0; k <= CURVE_COUNT; k++)
    {
      int prevx = r[0], prevy = 0, first = 1;
      const recipe_curve *cv;
      c = k < CURVE_COUNT ? k : curve_ch;
      if ((k < CURVE_COUNT && c == curve_ch) || (k < CURVE_COUNT && recipe_curve_is_identity (&current.curve[c])))
        continue;
      cv = &current.curve[c];
      for (x = 0; x <= gw; x += step)
        {
          int y = r[1] + (int) lround (recipe_curve_eval (cv, (double) x / gw) * gh);
          if (!first)
            {
              draw_line (prevx, prevy, r[0] + x, y, k < CURVE_COUNT ? (curve_col[c] >> 1) & 0x7F7F7F00u : curve_col[c]);
              if (k == CURVE_COUNT)
                draw_line (prevx, prevy + 2, r[0] + x, y + 2, curve_col[c]);
            }
          prevx = r[0] + x;
          prevy = y;
          first = 0;
        }
      if (k == CURVE_COUNT)
        {
          int i;
          for (i = 0; i < cv->n; i++)
            {
              int px = r[0] + (int) lround (cv->x[i] * gw), py = r[1] + (int) lround (cv->y[i] * gh);
              draw_rect (px - 8, py - 8, px + 8, py + 8, COL_FRAME);
              draw_rect (px - 6, py - 6, px + 6, py + 6, i == curve_drag ? COL_ACCENT : COL_KNOB);
            }
        }
    }
}

static void draw_panel (int ox, int oy)
{
  int px = ox + panel_x0 (), g, k;
  draw_histogram (px + PAD, oy + hist_top () - HIST_H, ox + PANEL_W - PAD, oy + hist_top ());
  /* Place in the list, rating */
  {
    char t[64];
    int y = oy + info_top () - 34;
    snprintf (t, sizeof t, "%d of %d", index_in_list + 1, browser_count ());
    draw_text (px + PAD, y, t, COL_QUIET, COL_PANEL);
    if (item.rating == RATING_REJECT)
      draw_text (ox + PANEL_W - PAD - draw_text_width ("Rejected"), y, "Rejected", COL_QUIET, COL_PANEL);
    else
      for (k = 0; k < 5; k++)
        draw_star (ox + PANEL_W - PAD - 15 - (4 - k) * 34, y + 10, 15, k < item.rating ? COL_ACCENT : COL_STAR_OFF);
    if (item.flag)
      {
        draw_rect (px + 200, y - 4, px + 228, y + 24, COL_FRAME);
        draw_rect (px + 204, y, px + 224, y + 20, 0xB0E6F200u);
      }
    /* The colour label's dots, to pick from; the photo's ringed. */
    for (k = 0; k < CAT_LABELS; k++)
      {
        int cx = px + LABEL_X0 + k * LABEL_DX, cy = y + 10;
        if (k == item.label)
          draw_circle (cx, cy, 12, COL_TEXT);
        draw_label_dot (cx, cy, 8, k);
      }
  }
  /* Tools */
  {
    static const char *const names[TOOL_COUNT] = { "Auto", "Crop", "Grid", "Presets", "Retouch", "Layers" };
    for (k = 0; k < TOOL_COUNT; k++)
      {
        int r[4];
        tool_rect (k, r);
        int on = (k == TOOL_RETOUCH && retouching && !brushing) || (k == TOOL_BRUSH && retouching && brushing);
        draw_button_rect (r, (k == TOOL_CROP && crop_mode) || on ? "Done" : names[k],
                          src != NULL && !(k == TOOL_AUTO && crop_mode),
                          (k == TOOL_CROP && crop_mode) || (k == TOOL_GRID && grid_mode) || on,
                          ox, oy);
      }
  }
  draw_masks_panel (ox, oy);
  if (fold_links_h ())
    {
      int y = oy + fold_links_top () - 32, x1 = ox + PANEL_W - PAD, x0 = x1 - draw_text_width ("Open all"), any_open = 0, any_folded = 0;
      for (g = 0; g < recipe_ngroups && g < MAX_GROUPS; g++)
        if (group_shown (g))
          {
            if (folded[g])
              any_folded = 1;
            else
              any_open = 1;
          }
      if (masks_shown () && current.nmasks)
        {
          if (masks_folded)
            any_folded = 1;
          else
            any_open = 1;
        }
      draw_text (x0, y, "Open all", any_folded ? COL_ACCENT : COL_STAR_OFF, COL_PANEL);
      x1 = x0 - 40;
      draw_text (x1 - draw_text_width ("Fold all"), y, "Fold all", any_open ? COL_ACCENT : COL_STAR_OFF, COL_PANEL);
    }
  /* Groups of sliders */
  for (g = 0; g < recipe_ngroups; g++)
    {
      int top = oy + group_top (g), p, changed = 0, ids[3], n;
      const char *names[3];
      if (!group_shown (g))
        continue;
      changed = group_changed (g);
      if (recipe_groups[g].first == P_ROTATE && !crop_mode)
        for (p = P_CROP_X0; p <= P_CROP_Y1; p++)   /* its Reset takes the crop away too */
          if (current.v[p] != recipe_params[p].def)
            changed = 1;
      /* Its header: a bar across the panel; a click on it folds the group. */
      draw_rect (px + PAD, top - GROUP_H + 6, ox + PANEL_W - PAD, top, COL_HEADER);
      draw_disclosure (px + PAD + 16, top - 24, g < MAX_GROUPS && folded[g]);
      draw_text (px + PAD + 40, top - 36, recipe_groups[g].name, COL_TEXT, COL_HEADER);
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
          draw_text (ox + x0, top - 36, names[k], src ? (col == COL_STAR_OFF ? COL_QUIET : col) : COL_QUIET, COL_HEADER);
        }
      if (group_pre (g) && !(g < MAX_GROUPS && folded[g]))
        {
          char a[160], bl[160];
          int y = top - GROUP_H - 36, camw = lens_camera_link () ? draw_text_width ("Camera") + 24 : 0;
          lens_lines (a, bl, sizeof a);
          fit_text (a, PANEL_W - 2 * PAD - 8);
          fit_text (bl, PANEL_W - 2 * PAD - 8 - camw);
          draw_text (px + PAD + 8, y, a, current.lens.on ? COL_TEXT : COL_QUIET, COL_PANEL);
          draw_text (px + PAD + 8, y - 44, bl, COL_QUIET, COL_PANEL);
          if (camw)
            draw_text (ox + PANEL_W - PAD - draw_text_width ("Camera"), y - 44, "Camera", src ? COL_ACCENT : COL_QUIET, COL_PANEL);
        }
      if (recipe_groups[g].curves && !(g < MAX_GROUPS && folded[g]))
        draw_curves (ox, oy);
      if (recipe_groups[g].hsl && !(g < MAX_GROUPS && folded[g]))
        {
          draw_hsl_bands (ox, oy);
          for (p = 0; p < 3; p++)
            draw_slider (HSL_PARAM (hsl_band, p), ox, oy);
        }
      if (!(g < MAX_GROUPS && folded[g]))
        for (p = recipe_groups[g].first; p <= recipe_groups[g].last; p++)
          if (!param_hidden (p))
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

/* --- retouch: screen <-> photo */

/* A point of the photo (fractions, upright) on the screen; 0 if no plan. */
static int photo_to_screen (const int out[4], float u, float v, int *x, int *y)
{
  double gx, gy;
  if (!src || gw <= 0 || gh <= 0)
    return 0;
  geom_from_source (&plan_full, u * sw, v * sh, &gx, &gy);
  *x = out[0] + (int) lround (gx / gw * (out[2] - out[0]));
  *y = out[3] - (int) lround (gy / gh * (out[3] - out[1]));
  return 1;
}

/* A point on the screen (work area) as a fraction of the photo, upright. */
static int screen_to_photo (const int out[4], int wx, int wy, float *u, float *v)
{
  double sx, sy;
  if (!src || out[2] <= out[0] || out[3] <= out[1])
    return 0;
  geom_to_source (&plan_full, (double) (wx - out[0]) / (out[2] - out[0]) * gw,
                  (double) (out[3] - wy) / (out[3] - out[1]) * gh, &sx, &sy);
  *u = (float) (sx / sw);
  *v = (float) (sy / sh);
  return 1;
}

/* A radius (fraction of the longer side) in screen units, about (u, v). */
static int radius_on_screen (const int out[4], float u, float v, float r)
{
  int x0, y0, x1, y1, longer = sw > sh ? sw : sh;
  if (!photo_to_screen (out, u, v, &x0, &y0) || !photo_to_screen (out, u + r * longer / sw, v, &x1, &y1))
    return 0;
  return (int) lround (sqrt ((double) (x1 - x0) * (x1 - x0) + (double) (y1 - y0) * (y1 - y0)));
}

/* The tools' controls, in a box across the top of the photo's area: the
   tool's buttons (Repair and Clone; Add and Erase for masks) and Done;
   below, sliders for a new spot's or stroke's size, feather (and, for
   masks, flow), which change the one last placed or moved too. The
   masks themselves (the list, new ones, the chosen one's sliders,
   Invert, Show mask and Delete) are in the panel: masks_* above. */

static int rt_nbuttons (void) { return brushing ? BRUSH_KINDS + 1 : 3; }
static int rt_nsliders (void) { return brushing ? 3 : 2; }
static float rt_size_max (void) { return brushing ? 0.2f : RT_SIZE_MAX; }

static void rt_box (int r[4])
{
  int ir[4];
  image_rect (ir);
  r[0] = ir[0] + PAD - 12;
  r[2] = r[0] + 24 + rt_nsliders () * RT_SLOT;
  r[3] = ir[3] - 2;
  r[1] = r[3] - 2 * RT_ROW - 18;
}

/* Done is the last, at the right. */
static void rt_button (int k, int r[4])
{
  int box[4];
  rt_box (box);
  if (k == rt_nbuttons () - 1)
    {
      r[2] = box[2] - 12;
      r[0] = r[2] - 140;
    }
  else
    {
      r[0] = box[0] + 12 + k * 190;
      r[2] = r[0] + 180;
    }
  r[3] = box[3] - 10;
  r[1] = r[3] - 52;
}

/* A slider's track (the knob's travel); its label is to the left, its value to the right. */
static int mask_rt_top (int k);
static void rt_track (int k, int r[4])
{
  int box[4];
  if (brushing)
    {
      /* In the panel (the Layers section). */
      r[0] = track_x0 ();
      r[2] = track_x1 ();
      r[3] = mask_rt_top (k);
      r[1] = r[3] - ROW_H;
      return;
    }
  rt_box (box);
  r[0] = box[0] + 12 + k * RT_SLOT + RT_LABEL_W;
  r[2] = r[0] + RT_TRACK_W;
  r[3] = box[3] - 10 - RT_ROW;
  r[1] = r[3] - 52;
}

/* Size runs on a log scale: small spots need fine steps. */
static float rt_slider_value (int k, int x)
{
  int t[4];
  float f;
  rt_track (k, t);
  f = (float) (x - t[0]) / (t[2] - t[0]);
  f = f < 0 ? 0 : f > 1 ? 1 : f;
  if (k != RT_SIZE)
    return f;
  return RT_SIZE_MIN * (float) pow (rt_size_max () / RT_SIZE_MIN, f);
}

static float *rt_slider_var (int k)
{
  return k == RT_SIZE ? &rt_size : k == RT_FEATHER ? &rt_feather : &rt_strength;
}

static int rt_slider_x (int k)
{
  int t[4];
  float f = k == RT_SIZE ? (float) (log (rt_size / RT_SIZE_MIN) / log (rt_size_max () / RT_SIZE_MIN)) : *rt_slider_var (k);
  rt_track (k, t);
  f = f < 0 ? 0 : f > 1 ? 1 : f;
  return t[0] + (int) lround (f * (t[2] - t[0]));
}

/* The adjustments the sliders show: the chosen mask's, or (none chosen)
   those a new one starts with. */
static recipe_mask *mb_mask (void)
{
  return mk_sel >= 0 && mk_sel < current.nmasks ? &current.mask[mk_sel] : &mk_last;
}

static float mb_slider_value (int p, int x)
{
  const recipe_param *d = &recipe_mask_params[p];
  float f = (float) (x - track_x0 ()) / (track_x1 () - track_x0 ());
  f = f < 0 ? 0 : f > 1 ? 1 : f;
  return d->min + f * (d->max - d->min);
}

/* A mask's pin: where it is shown on the photo, to choose it. */
static void mask_pin (int i, float *u, float *v)
{
  const recipe_mask *m = &current.mask[i];
  int k;
  if (m->kind == MASK_GRADIENT)
    {
      *u = (m->x0 + m->x1) / 2;
      *v = (m->y0 + m->y1) / 2;
      return;
    }
  if (m->kind == MASK_RADIAL)
    {
      *u = m->x0;
      *v = m->y0;
      return;
    }
  *u = *v = 0.5f;
  for (k = 0; k < current.nstrokes; k++)
    if (current.stroke[k].mask == i && current.stroke[k].kind == BRUSH_ADD)
      {
        *u = current.pt[current.stroke[k].first][0] / 10000.0f;
        *v = current.pt[current.stroke[k].first][1] / 10000.0f;
        return;
      }
}

/* The chosen mask's handles on the photo: gradient 0 start, 1 end, 2 the
   middle (moves both); radial 0 centre, 1 width, 2 height. */
static int mask_handle (int h, float *u, float *v)
{
  const recipe_mask *m;
  if (mk_sel < 0 || mk_sel >= current.nmasks)
    return 0;
  m = &current.mask[mk_sel];
  if (m->kind == MASK_GRADIENT)
    {
      *u = h == 0 ? m->x0 : h == 1 ? m->x1 : (m->x0 + m->x1) / 2;
      *v = h == 0 ? m->y0 : h == 1 ? m->y1 : (m->y0 + m->y1) / 2;
      return 1;
    }
  if (m->kind == MASK_RADIAL)
    {
      *u = h == 1 ? m->x0 + m->x1 : m->x0;
      *v = h == 2 ? m->y0 + m->y1 : m->y0;
      return 1;
    }
  return 0;
}

static const unsigned brush_colours[BRUSH_KINDS] = { 0x40E0F000u, 0xC0C0C000u };

/* A line kept inside the photo (out); nothing if it misses it. */
static void draw_line_in (const int out[4], int x0, int y0, int x1, int y1, unsigned colour)
{
  double t0 = 0, t1 = 1, dx = x1 - x0, dy = y1 - y0;
  double p[4] = { -dx, dx, -dy, dy }, q[4] = { x0 - out[0], out[2] - x0, y0 - out[1], out[3] - y0 };
  int k;
  for (k = 0; k < 4; k++)
    {
      if (p[k] == 0)
        {
          if (q[k] < 0)
            return;
          continue;
        }
      {
        double t = q[k] / p[k];
        if (p[k] < 0) { if (t > t1) return; if (t > t0) t0 = t; }
        else { if (t < t0) return; if (t < t1) t1 = t; }
      }
    }
  draw_line ((int) lround (x0 + t0 * dx), (int) lround (y0 + t0 * dy), (int) lround (x0 + t1 * dx), (int) lround (y0 + t1 * dy),
             colour);
}

/* A stroke's path on the screen (out: on the screen, as draw_overlays). */
static void draw_path (const int out[4], const float *xy, int n, unsigned colour, int bold)
{
  int j, px = 0, py = 0;
  for (j = 0; j < n; j++)
    {
      int x, y;
      if (!photo_to_screen (out, xy[2 * j], xy[2 * j + 1], &x, &y))
        return;
      if (j)
        {
          draw_line (px, py - 2, x, y - 2, COL_FRAME);
          draw_line (px, py, x, y, colour);
          if (bold)
            draw_line (px, py + 2, x, y + 2, colour);
        }
      else
        draw_circle (x, y, bold ? 6 : 4, colour);
      px = x;
      py = y;
    }
}

static void stroke_points (const recipe_stroke *st, float *xy)
{
  int j;
  for (j = 0; j < st->n; j++)
    {
      xy[2 * j] = current.pt[st->first + j][0] / 10000.0f;
      xy[2 * j + 1] = current.pt[st->first + j][1] / 10000.0f;
    }
}

/* A painted spot's path (moved by du, dv), as fractions. */
static int spot_points (int i, float du, float dv, float *xy)
{
  const recipe_spot *sp = &current.spot[i];
  int j, n = sp->n >= 2 ? sp->n : 1;
  for (j = 0; j < n; j++)
    {
      recipe_spot_point (&current, i, j, &xy[2 * j], &xy[2 * j + 1]);
      xy[2 * j] += du;
      xy[2 * j + 1] += dv;
    }
  return n;
}

/* An oval on the photo (centre, half-widths as fractions), through the geometry. */
static void draw_oval (const int out[4], float cu, float cv, float ru, float rv, unsigned colour)
{
  int k, px = 0, py = 0, ok = 0;
  for (k = 0; k <= 48; k++)
    {
      double a = 2 * 3.14159265358979 * k / 48;
      int x, y;
      if (!photo_to_screen (out, cu + (float) (cos (a) * ru), cv + (float) (sin (a) * rv), &x, &y))
        return;
      if (ok)
        {
          draw_line (px, py - 2, x, y - 2, COL_FRAME);
          draw_line (px, py, x, y, colour);
        }
      px = x;
      py = y;
      ok = 1;
    }
}

static void draw_handle (const int out[4], float u, float v, int on)
{
  int x, y;
  if (!photo_to_screen (out, u, v, &x, &y))
    return;
  draw_rect (x - 12, y - 12, x + 12, y + 12, COL_FRAME);
  draw_rect (x - 9, y - 9, x + 9, y + 9, on ? COL_ACCENT : COL_KNOB);
}

/* The chosen gradient's or oval's lines and handles; the masks' pins. */
static void draw_masks (const int out[4])
{
  int i;
  static float xy[RECIPE_STROKE_MAX * 2];
  for (i = 0; i < current.nstrokes; i++)
    if (current.stroke[i].mask == mk_sel)
      {
        stroke_points (&current.stroke[i], xy);
        draw_path (out, xy, current.stroke[i].n, brush_colours[current.stroke[i].kind], i == rt_sel);
      }
  if (mk_sel >= 0 && mk_sel < current.nmasks)
    {
      const recipe_mask *m = &current.mask[mk_sel];
      float u, v;
      if (m->kind == MASK_GRADIENT)
        {
          /* The line from all of it to none, and across at each end. */
          double du = m->x1 - m->x0, dv = m->y1 - m->y0, l;
          int k;
          {
            float a[4] = { m->x0, m->y0, m->x1, m->y1 };
            draw_path (out, a, 2, COL_KNOB, 0);
          }
          l = sqrt (du * du * sw * sw + dv * dv * sh * sh);
          if (l > 0)
            for (k = 0; k < 2; k++)
              {
                /* Perpendicular, in the photo's pixels, a long way each side. */
                double px = -dv * sh / l, py = du * sw / l, cu = k ? m->x1 : m->x0, cv = k ? m->y1 : m->y0, len = 3.0 * (sw > sh ? sw : sh);
                int ax, ay, bx, by;
                if (photo_to_screen (out, (float) (cu + px * len / sw), (float) (cv + py * len / sh), &ax, &ay)
                    && photo_to_screen (out, (float) (cu - px * len / sw), (float) (cv - py * len / sh), &bx, &by))
                  {
                    draw_line_in (out, ax, ay - 2, bx, by - 2, COL_FRAME);
                    draw_line_in (out, ax, ay, bx, by, k ? 0xA0A0A000u : COL_KNOB);
                  }
              }
        }
      else if (m->kind == MASK_RADIAL)
        {
          draw_oval (out, m->x0, m->y0, m->x1, m->y1, COL_KNOB);
          draw_oval (out, m->x0, m->y0, m->x1 * (1 - (m->f < 0.02f ? 0.02f : m->f)), m->y1 * (1 - (m->f < 0.02f ? 0.02f : m->f)),
                     0xA0A0A000u);
        }
      for (i = 0; i < 3 && mask_handle (i, &u, &v); i++)
        draw_handle (out, u, v, dragging == DRAG_HANDLE && mk_handle == i);
    }
  for (i = 0; i < current.nmasks; i++)
    {
      float u, v;
      int x, y;
      if (i == mk_sel && current.mask[i].kind != MASK_BRUSH)
        continue;                      /* its handles show where it is */
      mask_pin (i, &u, &v);
      if (!photo_to_screen (out, u, v, &x, &y))
        continue;
      draw_circle (x, y, 15, COL_FRAME);
      draw_circle (x, y, 12, i == mk_sel ? COL_ACCENT : COL_KNOB);
      draw_circle (x, y, 6, i == mk_sel ? COL_KNOB : COL_ACCENT);
    }
  /* A new gradient or oval being drawn. */
  if (dragging == DRAG_SHAPE)
    {
      if (mk_new == MASK_GRADIENT)
        {
          float a[4] = { mk_du0, mk_dv0, rt_dx, rt_dy };
          draw_path (out, a, 2, COL_KNOB, 1);
        }
      else
        draw_oval (out, mk_du0, mk_dv0, fabsf (rt_dx - mk_du0) > 0.005f ? fabsf (rt_dx - mk_du0) : 0.005f,
                   fabsf (rt_dy - mk_dv0) > 0.005f ? fabsf (rt_dy - mk_dv0) : 0.005f, COL_KNOB);
    }
}

/* A mask's name ("Gradient 2") and what it does ("-1.00 EV, Shadows +30"). */
static void mask_words (int i, char *name, size_t nn, char *what, size_t wn)
{
  static const char *const kinds[MASK_KINDS] = { "Brush", "Gradient", "Radial" };
  const recipe_mask *m = &current.mask[i];
  size_t used = 0;
  int k;
  snprintf (name, nn, "%s %d", kinds[m->kind], i + 1);
  what[0] = '\0';
  for (k = 0; k < M_COUNT && used + 1 < wn; k++)
    if (m->v[k] != 0)
      {
        int n = k == M_EV ? snprintf (what + used, wn - used, "%s%+.2f EV", used ? ", " : "", (double) m->v[k])
                          : snprintf (what + used, wn - used, "%s%s %+d", used ? ", " : "", recipe_mask_params[k].label,
                                      (int) lround (m->v[k]));
        if (n < 0)
          break;
        used += (size_t) n;
      }
  if (!what[0])
    snprintf (what, wn, "nothing yet");
  if (m->invert && used + 12 < wn)
    snprintf (what + strlen (what), wn - strlen (what), ", inverted");
  /* Kept to the panel's width. */
  {
    int room = PANEL_W - 2 * PAD - 52 - 180;
    size_t l = strlen (what);
    if (draw_text_width (what) > room)
      {
        while (l > 4)
          {
            l--;
            memcpy (what + l, "...", 4);
            if (draw_text_width (what) <= room)
              break;
            what[l] = '\0';
          }
      }
  }
}

static void draw_masks_panel (int ox, int oy)
{
  static const char *const buttons[MB_BUTTONS] = { "Brush", "Gradient", "Radial", "Invert", "Show mask", "Delete" };
  int px = ox + panel_x0 (), top = oy + masks_top (), i, k, r[4], chosen = mk_sel >= 0 && mk_sel < current.nmasks;
  char name[32], what[120], t[24];
  if (!masks_shown ())
    return;
  draw_rect (px + PAD, top - GROUP_H + 6, ox + PANEL_W - PAD, top, COL_HEADER);
  if (!masks_tool ())
    draw_disclosure (px + PAD + 16, top - 24, masks_folded);
  draw_text (px + PAD + 40, top - 36, "Layers", COL_TEXT, COL_HEADER);
  if (!masks_tool ())
    {
      draw_text (ox + PANEL_W - PAD - 12 - draw_text_width ("Edit"), top - 36, "Edit", COL_ACCENT, COL_HEADER);
      if (masks_folded)
        {
          char t[24];
          snprintf (t, sizeof t, "(%d)", current.nmasks);
          draw_text (px + PAD + 40 + draw_text_width ("Layers") + 16, top - 36, t, COL_QUIET, COL_HEADER);
          return;
        }
    }
  if (!current.nmasks)
    draw_text (px + PAD + 12, oy + mask_line_top (0) - 34, "None yet: paint, or choose New layer", COL_QUIET, COL_PANEL);
  for (i = 0; i < current.nmasks; i++)
    {
      int y = oy + mask_line_top (i), on = masks_tool () && i == mk_sel;
      unsigned bg = on ? COL_ACCENT : COL_PANEL, fg = on ? COL_KNOB : COL_TEXT;
      if (on)
        draw_rect (px + PAD, y - MASK_LINE_H + 4, ox + PANEL_W - PAD, y - 2, COL_ACCENT);
      /* The tick: on (applied) or off. */
      draw_rect (px + PAD + 10, y - 40, px + PAD + 38, y - 12, COL_FRAME);
      draw_rect (px + PAD + 13, y - 37, px + PAD + 35, y - 15, COL_BUTTON);
      if (!current.mask[i].off)
        {
          draw_line (px + PAD + 17, y - 27, px + PAD + 23, y - 33, COL_TEXT);
          draw_line (px + PAD + 23, y - 33, px + PAD + 32, y - 19, COL_TEXT);
          draw_line (px + PAD + 17, y - 25, px + PAD + 23, y - 31, COL_TEXT);
          draw_line (px + PAD + 23, y - 31, px + PAD + 32, y - 17, COL_TEXT);
        }
      mask_words (i, name, sizeof name, what, sizeof what);
      draw_text (px + PAD + 52, y - 36, name, current.mask[i].off ? COL_STAR_OFF : fg, bg);
      draw_text (px + PAD + 52 + 180, y - 36, what, on ? COL_KNOB : COL_QUIET, bg);
    }
  if (!masks_tool ())
    return;
  draw_text (px + PAD, oy + mask_line_top (mask_lines ()) - 36, "New layer", COL_QUIET, COL_PANEL);
  for (k = 0; k < MB_BUTTONS; k++)
    {
      int enabled = 1, on = 0;
      if (k == MB_INVERT)
        {
          enabled = chosen && current.mask[mk_sel].kind != MASK_BRUSH;
          on = enabled && current.mask[mk_sel].invert;
        }
      else if (k == MB_SHOW)
        {
          enabled = chosen;
          on = mk_show && chosen;
        }
      else if (k == MB_DELETE)
        enabled = chosen;
      else
        on = k != MB_NEW_BRUSH ? mk_new == k - MB_NEW_BRUSH : !chosen && mk_new < 0;
      button_at (k % 3, 3, k < 3 ? mask_new_top () : mask_buttons_top (), r);
      draw_button_rect (r, buttons[k], enabled, on, ox, oy);
    }
  /* The brush */
  draw_text (px + PAD, oy + mask_new_top () - BUTTON_H - 36, "Brush", COL_QUIET, COL_PANEL);
  for (k = 0; k < BRUSH_KINDS; k++)
    {
      static const char *const kinds[BRUSH_KINDS] = { "Add", "Erase" };
      button_at (k, 3, mask_brush_top (), r);
      draw_button_rect (r, kinds[k], 1, br_kind == k, ox, oy);
    }
  for (k = 0; k < RT_SLIDERS; k++)
    {
      static const char *const labels[RT_SLIDERS] = { "Size", "Feather", "Flow" };
      static const int drags[RT_SLIDERS] = { DRAG_RT_SIZE, DRAG_RT_FEATHER, DRAG_RT_STRENGTH };
      int mid = oy + mask_rt_top (k) - ROW_H / 2, x0 = ox + track_x0 (), kx = ox + rt_slider_x (k);
      draw_text (px + PAD, mid - 9, labels[k], COL_TEXT, COL_PANEL);
      draw_rect (x0, mid - 4, ox + track_x1 (), mid + 4, COL_TRACK);
      draw_rect (x0, mid - 4, kx, mid + 4, COL_ACCENT);
      draw_rect (kx - 10, mid - 18, kx + 10, mid + 18, COL_FRAME);
      draw_rect (kx - 7, mid - 15, kx + 7, mid + 15, dragging == drags[k] ? COL_ACCENT : COL_KNOB);
      if (k == RT_SIZE)
        snprintf (t, sizeof t, "%d", (int) lround (rt_size * 1000));
      else
        snprintf (t, sizeof t, "%d%%", (int) lround (*rt_slider_var (k) * 100));
      draw_text (ox + PANEL_W - PAD - draw_text_width (t), mid - 9, t, COL_TEXT, COL_PANEL);
    }
  {
    char head[60];
    if (chosen)
      {
        char nm[32], wh[8];
        mask_words (mk_sel, nm, sizeof nm, wh, sizeof wh);
        snprintf (head, sizeof head, "%s: its adjustments", nm);
      }
    else
      snprintf (head, sizeof head, "A new layer's adjustments");
    draw_rect (px + PAD, oy + mask_rt_top (RT_SLIDERS) - 14, ox + PANEL_W - PAD, oy + mask_rt_top (RT_SLIDERS) - 12, COL_TRACK);
    draw_text (px + PAD, oy + mask_rt_top (RT_SLIDERS) - 46, head, COL_QUIET, COL_PANEL);
  }
  /* The chosen mask's sliders (none chosen: what a new one starts with). */
  for (k = 0; k < M_COUNT; k++)
    {
      const recipe_param *d = &recipe_mask_params[k];
      float val = mb_mask ()->v[k];
      int mid = oy + mask_slider_top (k) - ROW_H / 2, x0 = ox + track_x0 (), x1 = ox + track_x1 ();
      int kx = x0 + (int) lround ((val - d->min) / (d->max - d->min) * (x1 - x0));
      int zx = x0 + (int) lround ((0 - d->min) / (d->max - d->min) * (x1 - x0));
      draw_text (px + PAD, mid - 9, d->label, COL_TEXT, COL_PANEL);
      draw_rect (x0, mid - 4, x1, mid + 4, COL_TRACK);
      if (kx != zx)
        draw_rect (kx < zx ? kx : zx, mid - 4, kx < zx ? zx : kx, mid + 4, COL_ACCENT);
      draw_rect (kx - 10, mid - 18, kx + 10, mid + 18, COL_FRAME);
      draw_rect (kx - 7, mid - 15, kx + 7, mid + 15, dragging == DRAG_MASK_SLIDER - k ? COL_ACCENT : COL_KNOB);
      if (val == 0)
        snprintf (t, sizeof t, "0");
      else if (k == M_EV)
        snprintf (t, sizeof t, "%+.2f", (double) val);
      else
        snprintf (t, sizeof t, "%+d", (int) lround (val));
      draw_text (ox + PANEL_W - PAD - draw_text_width (t), mid - 9, t, COL_TEXT, COL_PANEL);
    }
}

static void draw_retouch (int ox, int oy, const int out[4])
{
  int i, k, r[4];
  static const char *const spot_names[2] = { "Repair", "Clone" };
  static const char *const brush_names[BRUSH_KINDS] = { "Add", "Erase" };
  static const char *const labels[RT_SLIDERS] = { "Size", "Feather", "Flow" };
  static float xy[RECIPE_STROKE_MAX * 2];
  if (brushing)
    {
      draw_masks (out);
      if (dragging == DRAG_STROKE)
        draw_path (out, br_pts, br_n, brush_colours[br_kind], 1);
    }
  else
    {
      for (i = 0; i < current.nspots; i++)
        {
          const recipe_spot *sp = &current.spot[i];
          float du = 0, dv = 0, su = 0, sv = 0;
          int dx, dy, qx, qy, rad, inner, n;
          if (i == rt_drag)
            {
              su = rt_dx - rt_gu;
              sv = rt_dy - rt_gv;
              if (!rt_drag_src)
                {
                  du = su;
                  dv = sv;
                }
            }
          /* The source: the shape moved by the spot's offset. */
          n = spot_points (i, du + (sp->sx - sp->x) + (rt_drag_src && i == rt_drag ? su : 0),
                           dv + (sp->sy - sp->y) + (rt_drag_src && i == rt_drag ? sv : 0), xy);
          if (!photo_to_screen (out, xy[0], xy[1], &qx, &qy))
            continue;
          rad = radius_on_screen (out, xy[0], xy[1], sp->r);
          if (rad < 4)
            rad = 4;
          if (n > 1)
            draw_path (out, xy, n, 0xA0A0A000u, 0);
          else
            draw_circle (qx, qy, rad, 0xA0A0A000u);
          n = spot_points (i, du, dv, xy);
          if (!photo_to_screen (out, xy[0], xy[1], &dx, &dy))
            continue;
          draw_line (dx, dy, qx, qy, 0x80808000u);
          if (n > 1)
            {
              draw_path (out, xy, n, i == rt_sel ? COL_ACCENT : COL_CROP, 1);
              continue;
            }
          /* out is on the screen already (draw_overlays); the controls below are in the work area. */
          draw_circle (dx, dy, rad, i == rt_sel ? COL_ACCENT : COL_CROP);
          draw_circle (dx, dy, rad + 2, COL_FRAME);
          /* The feather: inside this ring the spot is painted out fully. */
          inner = (int) lround (rad * (1 - (sp->f < 0.02f ? 0.02f : sp->f)));
          if (i == rt_sel && inner > 3 && inner < rad - 2)
            draw_circle (dx, dy, inner, 0xA0A0A000u);
        }
      if (dragging == DRAG_STROKE)
        draw_path (out, br_pts, br_n, COL_KNOB, 1);
    }
  /* The brush: a new spot's (stroke's) size and feather, under the pointer. */
  if (rt_hover && (dragging == -1 || dragging == DRAG_STROKE) && !(brushing && mk_new > MASK_BRUSH))
    {
      int inner = (int) lround (rt_hr * (1 - (rt_feather < 0.02f ? 0.02f : rt_feather)));
      draw_circle (ox + rt_hx, oy + rt_hy, rt_hr + 2, COL_FRAME);
      draw_circle (ox + rt_hx, oy + rt_hy, rt_hr, brushing ? brush_colours[br_kind] : COL_KNOB);
      if (inner > 3 && inner < rt_hr - 2)
        draw_circle (ox + rt_hx, oy + rt_hy, inner, 0xA0A0A000u);
    }
  if (brushing)
    return;                            /* the Layers tool's controls are in the panel */
  rt_box (r);
  draw_rect (ox + r[0], oy + r[1], ox + r[2], oy + r[3], 0x20202000u);
  for (k = 0; k < rt_nbuttons (); k++)
    {
      int done = k == rt_nbuttons () - 1;
      rt_button (k, r);
      draw_button_rect (r, done ? "Done" : brushing ? brush_names[k] : spot_names[k], 1,
                        !done && (brushing ? k == br_kind : k == rt_clone), ox, oy);
    }
  for (k = 0; k < rt_nsliders (); k++)
    {
      char t[24];
      int mid, kx;
      static const int drags[RT_SLIDERS] = { DRAG_RT_SIZE, DRAG_RT_FEATHER, DRAG_RT_STRENGTH };
      rt_track (k, r);
      mid = (r[1] + r[3]) / 2;
      kx = rt_slider_x (k);
      draw_text (ox + r[0] - RT_LABEL_W, oy + mid - 9, labels[k], COL_LIGHT, 0x20202000u);
      draw_rect (ox + r[0], oy + mid - 4, ox + r[2], oy + mid + 4, COL_TRACK);
      draw_rect (ox + r[0], oy + mid - 4, ox + kx, oy + mid + 4, COL_ACCENT);
      draw_rect (ox + kx - 10, oy + mid - 18, ox + kx + 10, oy + mid + 18, COL_FRAME);
      draw_rect (ox + kx - 7, oy + mid - 15, ox + kx + 7, oy + mid + 15, dragging == drags[k] ? COL_ACCENT : COL_KNOB);
      if (k == RT_SIZE)
        snprintf (t, sizeof t, "%d", (int) lround (rt_size * 1000));
      else
        snprintf (t, sizeof t, "%d%%", (int) lround (*rt_slider_var (k) * 100));
      draw_text (ox + r[2] + 20, oy + mid - 9, t, COL_LIGHT, 0x20202000u);
    }
}

/* Null events while retouching: the brush follows the pointer over the photo. */
static void rt_hover_force (void)
{
  force (rt_hx - rt_hr - 8, rt_hy - rt_hr - 8, rt_hx + rt_hr + 8, rt_hy + rt_hr + 8);
}

/* Over one of the tools' boxes (work area)? */
static int in_tool_boxes (int wx, int wy)
{
  int box[4];
  if (brushing)
    return 0;
  rt_box (box);
  return wx >= box[0] && wx <= box[2] && wy >= box[1] && wy <= box[3];
}

static void rt_brush_follow (void)
{
  int ptr[5], st[9], r[4], wx, wy, over = 0, rad = 0;
  float u, v;
  if (!retouching || (dragging != -1 && dragging != DRAG_STROKE) || !src)
    {
      if (rt_hover)
        {
          rt_hover = 0;
          rt_hover_force ();
        }
      return;
    }
  st[0] = win;
  if (_swix (Wimp_GetPointerInfo, _IN (1), ptr) || _swix (Wimp_GetWindowState, _IN (1), st))
    return;
  wx = ptr[0] - st[1] + st[5];
  wy = ptr[1] - st[4] + st[6];
  if ((ptr[3] == win || dragging == DRAG_STROKE) && photo_rect (r) && wx >= r[0] && wx < r[2] && wy >= r[1] && wy < r[3]
      && !in_tool_boxes (wx, wy) && screen_to_photo (r, wx, wy, &u, &v))
    {
      over = 1;
      rad = radius_on_screen (r, u, v, rt_size);
      if (rad < 4)
        rad = 4;
    }
  if (over == rt_hover && (!over || (wx == rt_hx && wy == rt_hy && rad == rt_hr)))
    return;
  if (rt_hover)
    rt_hover_force ();
  rt_hover = over;
  rt_hx = wx;
  rt_hy = wy;
  rt_hr = rad;
  if (over)
    rt_hover_force ();
}

/* A painted spot's source found again (its path's offset). */
static void spot_auto_source (int i)
{
  static float xy[RECIPE_STROKE_MAX * 2];
  recipe_spot *sp = &current.spot[i];
  int n = spot_points (i, 0, 0, xy);
  float dx, dy;
  if (!src)
    return;
  retouch_auto_offset (src_orig ? src_orig : src, sw, sh, xy, n, sp->r, &dx, &dy);
  sp->sx = sp->x + dx;
  sp->sy = sp->y + dy;
}

/* The size, feather or strength changed (slider or keys): for new spots
   (strokes), and the chosen one too. live: only the overlay follows (a
   drag); the photo when let go. */
static void rt_apply (int live)
{
  if (brushing)
    force_panel ();                    /* its sliders are there */
  if (brushing && rt_sel >= 0 && rt_sel < current.nstrokes)
    {
      recipe_stroke *st = &current.stroke[rt_sel];
      st->r = rt_size;
      st->f = rt_feather;
      st->amount = rt_strength;
      if (!live)
        {
          spots_changed ();
          return;
        }
    }
  else if (!brushing && rt_sel >= 0 && rt_sel < current.nspots)
    {
      recipe_spot *sp = &current.spot[rt_sel];
      sp->r = rt_size;
      sp->f = rt_feather;
      if (!live)
        {
          if (rt_sel_auto && src)
            spot_auto_source (rt_sel);
          spots_changed ();
          return;
        }
    }
  force_image ();
}

/* --- retouch and masks: the tools */

/* The sliders' values are each tool's own. */
static float tool_vals[2][3] = { { 0.015f, 0.3f, 0 }, { 0.03f, 0.5f, 1.0f } };

static void local_tool (int brush)
{
  int showing = retouching && brushing && mk_show;
  if (!src && !retouching)
    return;
  if (retouching)
    {
      tool_vals[brushing][0] = rt_size;
      tool_vals[brushing][1] = rt_feather;
      tool_vals[brushing][2] = rt_strength;
    }
  rt_drag = -1;
  rt_sel = -1;
  rt_hover = 0;
  br_n = 0;
  mk_new = -1;
  mk_show = 0;
  if (retouching && brushing == brush)
    retouching = 0;
  else
    {
      if (crop_mode)
        viewer_crop_finish (1);
      if (zoom)
        zoom_to (0, 0);
      if (loupe_on)
        viewer_toggle_loupe ();
      if (comparing)
        compare_stop ();
      picking = 0;
      retouching = 1;
      brushing = brush;
      rt_size = tool_vals[brush][0];
      rt_feather = tool_vals[brush][1];
      rt_strength = tool_vals[brush][2];
      mk_sel = brush && current.nmasks ? current.nmasks - 1 : -1;
      /* The masks' sliders are in the Adjustments tab: show it. */
      if (brush)
        {
          if (!mainwin_inspector_on ())
            mainwin_toggle_inspector ();
          if (mainwin_tab () != TAB_ADJUST)
            mainwin_set_tab (TAB_ADJUST);
        }
      if (!mk_last_set)
        {
          recipe_mask_init (&mk_last, MASK_BRUSH);
          recipe_mask_set (&mk_last, M_EV, 0.5f);   /* something to see where it's painted */
          mk_last_set = 1;
        }
    }
  if (showing)
    refresh (1);                       /* the mask's red goes */
  update_title ();
  fit_panel ();
  force_all ();
}

void viewer_toggle_retouch (void) { local_tool (0); }
void viewer_toggle_brush (void) { local_tool (1); }
int viewer_retouching (void) { return retouching && !brushing; }
int viewer_brushing (void) { return retouching && brushing; }
int viewer_mask_chosen (void) { return retouching && brushing ? mk_sel : -1; }
int viewer_mask_showing (void) { return retouching && brushing && mk_show && applied_show == mk_sel && mk_sel >= 0; }

/* Tests: the Masks box's button k (MB_*), or with k >= 100 the slider
   for adjustment k - 100, in work area coordinates. */
void viewer_mask_box_part (int k, int r[4])
{
  if (k >= 200)
    {
      /* line k - 200 (with k >= 300: its tick) */
      int i = k >= 300 ? k - 300 : k - 200;
      r[0] = panel_x0 () + PAD + (k >= 300 ? 10 : 100);
      r[2] = k >= 300 ? panel_x0 () + PAD + 40 : PANEL_W - PAD;
      r[3] = mask_line_top (i) - 4;
      r[1] = mask_line_top (i) - MASK_LINE_H + 8;
    }
  else if (k >= 100)
    {
      r[0] = track_x0 ();
      r[2] = track_x1 ();
      r[3] = mask_slider_top (k - 100);
      r[1] = r[3] - ROW_H;
    }
  else
    button_at (k % 3, 3, k < 3 ? mask_new_top () : mask_buttons_top (), r);
}

/* The spot whose shape (or, with source, its source's) is under (u, v), or -1. */
static int spot_at (float u, float v, int source)
{
  static float xy[RECIPE_STROKE_MAX * 2];
  int i, j, longer = sw > sh ? sw : sh;
  double kx = (double) sw / longer, ky = (double) sh / longer;
  for (i = current.nspots - 1; i >= 0; i--)
    {
      const recipe_spot *sp = &current.spot[i];
      int n = spot_points (i, source ? sp->sx - sp->x : 0, source ? sp->sy - sp->y : 0, xy);
      for (j = 0; j < n; j++)
        {
          int k = j + 1 < n ? j + 1 : j;
          double ax = xy[2 * j] * kx, ay = xy[2 * j + 1] * ky, bx = xy[2 * k] * kx, by = xy[2 * k + 1] * ky;
          double dx = bx - ax, dy = by - ay, l = dx * dx + dy * dy, t = 0, px = u * kx, py = v * ky;
          if (l > 0)
            {
              t = ((px - ax) * dx + (py - ay) * dy) / l;
              t = t < 0 ? 0 : t > 1 ? 1 : t;
            }
          dx = ax + t * dx - px;
          dy = ay + t * dy - py;
          if (dx * dx + dy * dy <= (double) sp->r * sp->r * 1.2)
            return i;
        }
    }
  return -1;
}

/* The chosen mask's stroke whose brush covers (u, v) (the last painted first), or -1. */
static int stroke_at (float u, float v)
{
  int i, j, longer = sw > sh ? sw : sh;
  double kx = (double) sw / longer, ky = (double) sh / longer;
  for (i = current.nstrokes - 1; i >= 0; i--)
    {
      const recipe_stroke *st = &current.stroke[i];
      if (st->mask != mk_sel)
        continue;
      for (j = 0; j < st->n; j++)
        {
          int k = j + 1 < st->n ? j + 1 : j;
          double ax = current.pt[st->first + j][0] / 10000.0 * kx, ay = current.pt[st->first + j][1] / 10000.0 * ky;
          double bx = current.pt[st->first + k][0] / 10000.0 * kx, by = current.pt[st->first + k][1] / 10000.0 * ky;
          double dx = bx - ax, dy = by - ay, l = dx * dx + dy * dy, t = 0, px = u * kx, py = v * ky;
          if (l > 0)
            {
              t = ((px - ax) * dx + (py - ay) * dy) / l;
              t = t < 0 ? 0 : t > 1 ? 1 : t;
            }
          dx = ax + t * dx - px;
          dy = ay + t * dy - py;
          if (dx * dx + dy * dy <= (double) st->r * st->r)
            return i;
        }
    }
  return -1;
}

static void spots_changed (void)
{
  before = 0;
  commit ();
  refresh (1);
  force_image ();
  fit_panel ();
  force_panel ();
}

/* While a stroke is painted: a point added when the pointer has gone a
   third of the brush's radius from the last, and that piece drawn. */
static void stroke_follow (int wx, int wy)
{
  int r[4], longer = sw > sh ? sw : sh, x0, y0, x1, y1;
  float u, v;
  double dx, dy;
  if (br_n <= 0 || br_n >= RECIPE_STROKE_MAX || !photo_rect (r) || !screen_to_photo (r, wx, wy, &u, &v))
    return;
  u = u < 0 ? 0 : u > 1 ? 1 : u;
  v = v < 0 ? 0 : v > 1 ? 1 : v;
  dx = (u - br_pts[2 * br_n - 2]) * sw / longer;
  dy = (v - br_pts[2 * br_n - 1]) * sh / longer;
  if (dx * dx + dy * dy < (double) rt_size * rt_size / 9)
    return;
  br_pts[2 * br_n] = u;
  br_pts[2 * br_n + 1] = v;
  br_n++;
  if (photo_to_screen (r, br_pts[2 * br_n - 4], br_pts[2 * br_n - 3], &x0, &y0) && photo_to_screen (r, u, v, &x1, &y1))
    force ((x0 < x1 ? x0 : x1) - 12, (y0 < y1 ? y0 : y1) - 12, (x0 > x1 ? x0 : x1) + 12, (y0 > y1 ? y0 : y1) + 12);
}

/* A new mask (the adjustments new ones start with); its index, or -1. */
static int new_mask (int kind)
{
  recipe_mask m;
  int i;
  recipe_mask_init (&m, kind);
  memcpy (m.v, mk_last.v, sizeof m.v);
  if ((i = recipe_mask_add (&current, &m)) < 0)
    app_report ("A photo can have 32 layers at most. Delete one to add another.");
  return i;
}

static void stroke_end (void)
{
  if (br_n <= 0)
    return;
  if (!brushing)
    {
      /* Retouch: a click a round spot, a drag a painted one. */
      recipe_spot sp;
      int n = br_n;
      br_n = 0;
      sp.x = br_pts[0];
      sp.y = br_pts[1];
      sp.r = rt_size;
      sp.clone = rt_clone;
      sp.f = rt_feather;
      sp.first = sp.n = 0;
      if (n == 1)
        retouch_auto_source (src, sw, sh, sp.x, sp.y, rt_size, &sp.sx, &sp.sy);
      else
        {
          float dx, dy;
          retouch_auto_offset (src_orig ? src_orig : src, sw, sh, br_pts, n, rt_size, &dx, &dy);
          sp.sx = sp.x + dx;
          sp.sy = sp.y + dy;
        }
      rt_sel_auto = 1;
      if ((rt_sel = recipe_spot_add_path (&current, &sp, br_pts, n)) < 0)
        {
          app_report ("There's no room for another spot on this photo (48 spots, 512 points painted in all). "
                      "Take some away (Adjust-click) to add more.");
          force_image ();
          return;
        }
      spots_changed ();
      return;
    }
  if (mk_sel < 0 || mk_sel >= current.nmasks)
    {
      if (br_kind == BRUSH_ERASE)
        {
          br_n = 0;
          force_image ();
          return;                      /* nothing to erase */
        }
      if ((mk_sel = new_mask (MASK_BRUSH)) < 0)
        {
          br_n = 0;
          force_image ();
          return;
        }
    }
  rt_sel = recipe_stroke_add (&current, mk_sel, br_kind, rt_size, rt_feather, rt_strength, br_pts, br_n);
  br_n = 0;
  if (rt_sel < 0)
    {
      app_report ("There's no room for another stroke on this photo (64 strokes, 1024 points in all). "
                  "Take some away (Adjust-click) to add more.");
      force_image ();
      return;
    }
  spots_changed ();
}

/* The chosen mask changed: Show mask follows it. */
static void mask_chosen (int i)
{
  mk_sel = i;
  rt_sel = -1;
  if (mk_show)
    refresh (1);
  force_image ();
}

static void mask_value (int p, float v, int live)
{
  recipe_mask *m = mb_mask ();
  recipe_mask_set (m, p, v);
  force_panel ();
  if (m == &mk_last)
    return;
  mk_last = *m;                        /* new masks start like the one last changed */
  if (live)
    {
      before = 0;
      refresh (1);
      force_image ();
    }
  else
    spots_changed ();
}

/* A click on the panel's Masks section (hit's what). */
static void masks_panel_click (const int *b, int h, int what, int wx, const int *st)
{
  int chosen = mk_sel >= 0 && mk_sel < current.nmasks;
  if (h == HIT_MASK_TICK)
    {
      current.mask[what].off = !current.mask[what].off;
      spots_changed ();
      return;
    }
  if (h == HIT_MASK_LINE)
    {
      if (what == -2)
        {
          masks_folded = !masks_folded;
          fit_panel ();
          force_panel ();
          return;
        }
      if (!masks_tool ())
        local_tool (1);
      if (what < 0)
        return;
      mk_new = -1;
      mask_chosen (what);
      force_panel ();
      return;
    }
  if (h == HIT_MASK_BUTTON)
    {
      switch (what)
        {
        case MB_NEW_BRUSH:
          /* The next stroke starts a new brush mask. */
          mk_new = -1;
          mask_chosen (-1);
          break;
        case MB_NEW_GRADIENT:
        case MB_NEW_RADIAL:
          mk_new = mk_new == what - MB_NEW_BRUSH ? -1 : what - MB_NEW_BRUSH;   /* drawn by the next drag */
          break;
        case MB_INVERT:
          if (chosen && current.mask[mk_sel].kind != MASK_BRUSH)
            {
              current.mask[mk_sel].invert = !current.mask[mk_sel].invert;
              spots_changed ();
            }
          break;
        case MB_SHOW:
          if (chosen)
            {
              mk_show = !mk_show;
              refresh (1);
              force_image ();
            }
          break;
        case MB_DELETE:
          if (chosen)
            {
              recipe_mask_remove (&current, mk_sel);
              mk_sel = current.nmasks ? (mk_sel > 0 ? mk_sel - 1 : 0) : -1;
              rt_sel = -1;
              spots_changed ();
            }
          break;
        }
      force_panel ();
      force_image ();
      return;
    }
  if (h == HIT_MASK_BRUSH)
    {
      br_kind = what;
      rt_sel = -1;                     /* a new kind starts a new stroke */
      force_panel ();
      return;
    }
  if (h == HIT_MASK_RT && b[2] == Button_Select)
    {
      static const int drags[RT_SLIDERS] = { DRAG_RT_SIZE, DRAG_RT_FEATHER, DRAG_RT_STRENGTH };
      *rt_slider_var (what) = rt_slider_value (what, wx);
      rt_apply (1);
      force_panel ();
      start_drag (b[0], b[1], st[1] + track_x0 () - 4, st[4] + mask_rt_top (what) - ROW_H + 4,
                  st[1] + track_x1 () + 4, st[4] + mask_rt_top (what) - 4, drags[what]);
      if (dragging != drags[what])
        rt_apply (0);
      return;
    }
  if (h == HIT_MASK_SLIDER)
    {
      if (b[2] == Button_Adjust)
        {
          mask_value (what, 0, 0);     /* Adjust: back to none */
          return;
        }
      if (b[2] != Button_Select)
        return;
      mask_value (what, mb_slider_value (what, wx), 1);
      start_drag (b[0], b[1], st[1] + track_x0 () - 4, st[4] + mask_slider_top (what) - ROW_H + 4,
                  st[1] + track_x1 () + 4, st[4] + mask_slider_top (what) - 4, DRAG_MASK_SLIDER - what);
      if (dragging != DRAG_MASK_SLIDER - what)
        mask_value (what, mb_mask ()->v[what], 0);
    }
}

/* While a handle is dragged: the chosen mask's shape follows. */
static void handle_follow (float u, float v)
{
  recipe_mask *m;
  if (mk_sel < 0 || mk_sel >= current.nmasks)
    return;
  m = &current.mask[mk_sel];
  if (m->kind == MASK_GRADIENT)
    {
      if (mk_handle == 0) { m->x0 = u; m->y0 = v; }
      else if (mk_handle == 1) { m->x1 = u; m->y1 = v; }
      else
        {
          float du = u - (m->x0 + m->x1) / 2, dv = v - (m->y0 + m->y1) / 2;
          m->x0 += du; m->x1 += du; m->y0 += dv; m->y1 += dv;
        }
    }
  else if (m->kind == MASK_RADIAL)
    {
      if (mk_handle == 0) { m->x0 = u; m->y0 = v; }
      else if (mk_handle == 1) m->x1 = fabsf (u - m->x0) > 0.005f ? fabsf (u - m->x0) : 0.005f;
      else m->y1 = fabsf (v - m->y0) > 0.005f ? fabsf (v - m->y0) : 0.005f;
    }
  before = 0;
  refresh (0);
  force_image ();
}

/* A new gradient or oval drawn from (mk_du0, mk_dv0) to (u, v); a click
   (no drag) gives the usual one there. */
static void shape_end (float u, float v)
{
  int kind = mk_new, i;
  recipe_mask *m;
  mk_new = -1;
  if ((i = new_mask (kind)) < 0)
    return;
  m = &current.mask[i];
  if (fabsf (u - mk_du0) + fabsf (v - mk_dv0) > 0.01f)
    {
      if (kind == MASK_GRADIENT)
        {
          m->x0 = mk_du0; m->y0 = mk_dv0; m->x1 = u; m->y1 = v;
        }
      else
        {
          m->x0 = mk_du0; m->y0 = mk_dv0;
          m->x1 = fabsf (u - mk_du0) > 0.005f ? fabsf (u - mk_du0) : 0.005f;
          m->y1 = fabsf (v - mk_dv0) > 0.005f ? fabsf (v - mk_dv0) : 0.005f;
        }
    }
  else if (kind == MASK_RADIAL)
    {
      m->x0 = mk_du0;
      m->y0 = mk_dv0;
    }
  else
    {
      /* A gradient from the click, fading over a quarter of the height. */
      m->x0 = mk_du0; m->y0 = mk_dv0; m->x1 = mk_du0; m->y1 = mk_dv0 + 0.25f > 1 ? 1 : mk_dv0 + 0.25f;
    }
  mk_sel = i;
  rt_sel = -1;
  spots_changed ();
}

/* A click in the photo's area while retouching (masks): the controls,
   a spot to move or (Adjust) take away, or a new spot with a source
   found for it (a drag: a painted one); masks, a pin to choose one, the
   chosen one's handles, a new gradient or oval to draw, a stroke to
   paint, or (Adjust) take away. */
static void retouch_click (const int *b, int wx, int wy)
{
  int out[4], k, r[4], i;
  float u, v;
  for (k = 0; k < (brushing ? 0 : rt_nbuttons ()); k++)
    {
      rt_button (k, r);
      if (wx >= r[0] && wx <= r[2] && wy >= r[1] && wy <= r[3])
        {
          if (k == rt_nbuttons () - 1)
            {
              local_tool (brushing);
              return;
            }
          if (brushing)
            {
              br_kind = k;
              rt_sel = -1;               /* a new kind starts a new stroke */
            }
          else
            rt_clone = k;
          force_image ();
          return;
        }
    }
  for (k = 0; k < (brushing ? 0 : rt_nsliders ()); k++)
    {
      rt_track (k, r);
      if (wx >= r[0] - 24 && wx <= r[2] + 24 && wy >= r[1] && wy <= r[3] && photo_rect (out))
        {
          static const int drags[RT_SLIDERS] = { DRAG_RT_SIZE, DRAG_RT_FEATHER, DRAG_RT_STRENGTH };
          *rt_slider_var (k) = rt_slider_value (k, wx);
          rt_apply (1);
          start_drag (b[0], b[1], b[0] - (wx - r[0]) - 24, b[1], b[0] + (r[2] - wx) + 24, b[1], drags[k]);
          if (dragging != drags[k])
            rt_apply (0);
          return;
        }
    }
  if (in_tool_boxes (wx, wy))
    return;                            /* the controls' box, between them */
  if (!photo_rect (out) || !screen_to_photo (out, wx, wy, &u, &v) || u < 0 || u > 1 || v < 0 || v > 1)
    return;
  if (brushing)
    {
      /* A pin, or the chosen mask's handles. */
      for (i = current.nmasks - 1; i >= 0 && b[2] == Button_Select && mk_new < 0; i--)
        {
          float pu, pv;
          int px, py;
          mask_pin (i, &pu, &pv);
          if (photo_to_screen (out, pu, pv, &px, &py) && abs (px - wx) <= 18 && abs (py - wy) <= 18 && i != mk_sel)
            {
              mask_chosen (i);
              return;
            }
        }
      for (i = 0; i < 3 && b[2] == Button_Select && mk_new < 0; i++)
        {
          float hu, hv;
          int hx, hy;
          if (mask_handle (i, &hu, &hv) && photo_to_screen (out, hu, hv, &hx, &hy) && abs (hx - wx) <= 16 && abs (hy - wy) <= 16)
            {
              mk_handle = i;
              start_drag (b[0], b[1], b[0] - (wx - out[0]), b[1] - (wy - out[1]), b[0] + (out[2] - wx), b[1] + (out[3] - wy),
                          DRAG_HANDLE);
              if (dragging != DRAG_HANDLE)
                spots_changed ();
              force_image ();
              return;
            }
        }
      if (b[2] == Button_Adjust)
        {
          if ((i = stroke_at (u, v)) >= 0)
            {
              recipe_stroke_remove (&current, i);
              rt_sel = i == rt_sel ? -1 : i < rt_sel ? rt_sel - 1 : rt_sel;
              spots_changed ();
            }
          return;
        }
      if (b[2] != Button_Select)
        return;
      if (mk_new > MASK_BRUSH)
        {
          /* Drawing a new gradient or oval. */
          mk_du0 = rt_dx = u;
          mk_dv0 = rt_dy = v;
          start_drag (b[0], b[1], b[0] - (wx - out[0]), b[1] - (wy - out[1]), b[0] + (out[2] - wx), b[1] + (out[3] - wy),
                      DRAG_SHAPE);
          if (dragging != DRAG_SHAPE)
            shape_end (u, v);
          return;
        }
    }
  else if ((i = spot_at (u, v, 0)) >= 0 || (i = spot_at (u, v, 1)) >= 0)
    {
      int is_src = spot_at (u, v, 0) != i;
      if (b[2] == Button_Adjust)
        {
          recipe_spot_remove (&current, i);
          rt_sel = i == rt_sel ? -1 : i < rt_sel ? rt_sel - 1 : rt_sel;
          spots_changed ();
          return;
        }
      /* Drag it (or its source) somewhere else; it is the chosen spot,
         whose size and feather the sliders show. */
      if (i != rt_sel || is_src)
        rt_sel_auto = 0;               /* a source placed by hand stays where it is */
      rt_sel = i;
      rt_size = current.spot[i].r;
      rt_feather = current.spot[i].f;
      rt_drag = i;
      rt_drag_src = is_src;
      rt_gu = rt_dx = u;
      rt_gv = rt_dy = v;
      start_drag (b[0], b[1], b[0] - (wx - out[0]), b[1] - (wy - out[1]), b[0] + (out[2] - wx), b[1] + (out[3] - wy),
                  DRAG_SPOT);
      return;
    }
  if (b[2] != Button_Select)
    return;
  /* Paint: a stroke on the chosen mask (a new one if none), or a spot. */
  br_pts[0] = u;
  br_pts[1] = v;
  br_n = 1;
  start_drag (b[0], b[1], b[0] - (wx - out[0]), b[1] - (wy - out[1]), b[0] + (out[2] - wx), b[1] + (out[3] - wy),
              DRAG_STROKE);
  if (dragging != DRAG_STROKE)
    stroke_end ();                     /* a dab, or a round spot */
  else
    force_image ();
}

static void draw_overlays (int ox, int oy, const int out[4])
{
  if (retouching && !crop_mode)
    {
      draw_retouch (ox, oy, out);
      return;
    }
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
}

/* Compare: the pinned photo in the left half, named, with a line between. */
static void draw_compared (int ox, int oy)
{
  int x1 = vis_w / 2, out[4];
  char t[90];
  draw_rect (ox, oy - vis_h, ox + x1, oy, COL_GROUND);
  if (cmp_spr.area)
    {
      draw_set_smoothing (1);
      draw_sprite_rect (cmp_spr.area, (const unsigned *) cmp_spr.area + 4, cmp_spr.w, cmp_spr.h,
                        ox + PAD, oy - vis_h + PAD + 40, ox + x1 - PAD, oy - PAD, out);
    }
  snprintf (t, sizeof t, "Compare: %s", cmp_item.name);
  draw_text (ox + PAD, oy - vis_h + 20, t, COL_LIGHT, COL_GROUND);
  draw_rect (ox + x1 - 2, oy - vis_h, ox + x1 + 2, oy, 0x55555500u);
}

static void draw_image (int ox, int oy)
{
  int r[4], out[4];
  if (comparing)
    draw_compared (ox, oy);
  image_rect (r);
  {
    /* The photo itself covers its own place: only round it is the ground
       filled, so a change never shows the dark ground first. */
    const sprite32 *s = shown_sprite ();
    int fit[4];
    if (!(zoom == 2 && zspr.area) && (state == V_READY || state == V_LOADING) && src && s->area
        && draw_fit (s->w, s->h, r[0] + PAD, r[1] + PAD, r[2] - PAD, r[3] - PAD, fit))
      {
        draw_rect (ox + r[0], oy + fit[3], ox + r[2], oy + r[3], COL_GROUND);   /* above */
        draw_rect (ox + r[0], oy + r[1], ox + r[2], oy + fit[1], COL_GROUND);   /* below */
        draw_rect (ox + r[0], oy + fit[1], ox + fit[0], oy + fit[3], COL_GROUND);   /* left */
        draw_rect (ox + fit[2], oy + fit[1], ox + r[2], oy + fit[3], COL_GROUND);   /* right */
      }
    else
      draw_rect (ox + r[0], oy + r[1], ox + r[2], oy + r[3], COL_GROUND);
  }
  r[0] += PAD; r[1] += PAD; r[2] -= PAD; r[3] -= PAD;
  if (zoom == 2 && zspr.area)
    {
      /* 100%: the region, a pixel a pixel, centred (and offset while dragged). */
      int pw = zspr.w << draw_xeig (), ph = zspr.h << draw_yeig ();
      int x = ox + (r[0] + r[2] - pw) / 2 + pan_dx, y = oy + (r[1] + r[3] - ph) / 2 + pan_dy;
      char t[40];
      draw_sprite_at (zspr.area, (const unsigned *) zspr.area + 4, x, y);
      snprintf (t, sizeof t, "100%%");
      draw_rect (ox + r[0] + 16, oy + r[3] - 64, ox + r[0] + 56 + draw_text_width (t), oy + r[3] - 16, 0x00000000u);
      draw_text (ox + r[0] + 36, oy + r[3] - 50, t, COL_LIGHT, 0x00000000u);
    }
  else if ((state == V_READY || state == V_LOADING) && (src || state == V_LOADING))
    {
      const sprite32 *s = shown_sprite ();
      if (s->area)
        {
          int pr[4];
          /* Both renders are plotted from their copy at the size shown
             (prepare_shown made it when the pixels changed). */
          draw_set_smoothing (1);
          draw_sprite_rect (s->area, (const unsigned *) s->area + 4, s->w, s->h,
                            ox + r[0], oy + r[1], ox + r[2], oy + r[3], out);
          draw_set_smoothing (1);
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
      draw_loupe (ox, oy);
      if (before || picking || crop_mode || zoom == 1 || (loupe_on && !zfull.rgb))
        {
          const char *t = zoom == 1 || (loupe_on && !zfull.rgb) ? "Reading the full-size photo..." : before ? "Before" : picking ? "Click something that should be grey or white"
            : "Drag the corners or edges; Return to crop, Escape to cancel";
          int tw = draw_text_width (t);
          draw_rect (ox + r[0] + 16, oy + r[3] - 64, ox + r[0] + 56 + tw, oy + r[3] - 16, 0x00000000u);
          draw_text (ox + r[0] + 36, oy + r[3] - 50, t, COL_LIGHT, 0x00000000u);
        }
    }
  else if (state == V_FAILED)
    {
      /* Wrapped to the window, at spaces. */
      const char *p = err;
      int y = oy + r[3] - 60, width = r[2] - r[0] - 48;
      while (*p)
        {
          char line[200];
          int n = 0, brk = 0;
          while (p[n] && n < (int) sizeof line - 1)
            {
              line[n] = p[n];
              line[n + 1] = '\0';
              if (draw_text_width (line) > width)
                break;
              if (p[n] == ' ')
                brk = n + 1;
              n++;
            }
          if (p[n] && brk)
            n = brk;
          if (!n)
            n = 1;
          memcpy (line, p, (size_t) n);
          line[n] = '\0';
          draw_text (ox + r[0] + 24, y, line, COL_LIGHT, COL_GROUND);
          y -= 44;
          p += n;
        }
    }
}

void viewer_redraw (int *b)
{
  int more = 0;
  _swix (Wimp_RedrawWindow, _IN (1) | _OUT (0), b, &more);
  while (more)
    {
      int ox = b[1] - b[5], oy = b[4] - b[6];
      draw_image (ox, oy);
      _swix (Wimp_GetRectangle, _IN (1) | _OUT (0), b, &more);
    }
}

void viewer_mode_changed (void)
{
  read_screen ();
  force_all ();
}

/* The Adjustments tab of the inspector (its work area origin at ox, oy). */
void viewer_panel_draw (int ox, int oy)
{
  if (state == V_CLOSED || !is_open)
    {
      draw_text (ox + PAD, oy - 60, "Choose a photo in Split View or", COL_QUIET, COL_PANEL);
      draw_text (ox + PAD, oy - 104, "the Viewer to adjust it.", COL_QUIET, COL_PANEL);
      return;
    }
  draw_panel (ox, oy);
}

/* For the screenshot harness and tests. */
int viewer_proxy_size (int *w, int *h)
{
  *w = sw;
  *h = sh;
  return src != NULL;
}

/* For the tests: the curves graph (work area), 0 if folded. */
int viewer_curve_rect (int r[4]) { return curve_graph (r); }
int viewer_hsl_swatch (int k, int r[4]) { return hsl_swatch (k, r); }
int viewer_row_top (int p) { return row_top (p); }

/* For the tests: the middle of a bottom button (0 Undo, 1 Redo, 2 Reset all), work area. */
void viewer_button_centre (int k, int *x, int *y)
{
  int r[4];
  button_rect (k, r);
  *x = (r[0] + r[2]) / 2;
  *y = (r[1] + r[3]) / 2;
}

int viewer_shown_size (int *w, int *h)
{
  *w = gw;
  *h = gh;
  return geo != NULL;
}
