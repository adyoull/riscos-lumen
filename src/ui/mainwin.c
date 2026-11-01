/* mainwin.c: Lumen's one main window, laid out as Aperture 3's.

     +--------------------------------------------------------------+
     | toolbar: Inspector Import New | Browser Split Viewer | Loupe  |
     |          Export Full Screen                                  |
     +---------------+----------------------------------------------+
     | Library|Info|Adj (tabs)   |                                  |
     | inspector     |  Viewer (Viewer and Split View layouts)      |
     |  (a nested    |                                              |
     |   window,     +----------------------------------------------+
     |   scrolling)  |  Browser (Browser and Split View layouts)    |
     +---------------+----------------------------------------------+

   The toolbar and the inspector's tabs are drawn in the main window
   itself; the inspector, the Browser and the Viewer are child windows
   nested in it (Wimp_OpenWindow extended syntax, PRM p738: R2 "TASK",
   R3 the parent, R4 0: each child moves with the parent's work area), so
   each scrolls with its own scroll bar. Whenever the main window opens or
   changes size, the children are placed again.

   Keys, as Aperture's: V cycles the layouts, I shows or hides the
   inspector, W cycles its tabs, F is full screen. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <kernel.h>
#include <swis.h>
#include "ui.h"

#define TB_H 120                       /* the toolbar */
#define TAB_H 72                       /* the inspector's tabs */
#define BB_H 56                        /* the bar along the bottom */
#define GRIP_W 56                      /* its resize grip */
#define INSP_W 640                     /* the inspector's visible width */
#define SB_W 44                        /* a vertical scroll bar beside a child */
#define GAP 8
#define LIB_ROW 52

#define COL_TOOLBAR 0xD8D8D800u
#define COL_TOOLBAR_EDGE 0x9A9A9A00u
#define COL_TEXT 0x1A1A1A00u
#define COL_QUIET 0x60606000u
#define COL_ACCENT 0x99440000u
#define COL_WHITE 0xFFFFFF00u
#define COL_PANEL 0xE2E2E200u
#define COL_ICON 0x40404000u

enum { TB_INSPECTOR, TB_IMPORT, TB_NEW, TB_BROWSER, TB_SPLIT, TB_VIEWER, TB_LOUPE, TB_EXPORT, TB_FULL, TB_COUNT };

static int win = -1, insp = -1, is_open, full_screen;
static int mode = LAYOUT_BROWSER, insp_on = 1, tab = TAB_LIBRARY;
static int vis_w = 2800, vis_h = 1700, screen_w = 3840, screen_h = 2160;
static int normal_box[4];              /* where the window was before full screen */
static char title[220];
static int insp_extent = 2000, insp_content = 400, insp_vis = 400;

/* The Library tab's rows: what each shows. */
typedef struct { int kind; cat_id id; char text[80]; int heading; } lib_row;
static lib_row *rows;
static int nrows;
static void build_rows (void);
static void apply_extent (void);

int mainwin_window (void) { return win; }
int mainwin_inspector (void) { return insp; }
int mainwin_mode (void) { return mode; }
int mainwin_tab (void) { return tab; }
int mainwin_inspector_on (void) { return insp_on; }

static void read_screen (void)
{
  int xl = 1919, yl = 1079;
  _swix (OS_ReadModeVariable, _INR (0, 1) | _OUT (2), -1, 11, &xl);
  _swix (OS_ReadModeVariable, _INR (0, 1) | _OUT (2), -1, 12, &yl);
  screen_w = (xl + 1) << draw_xeig ();
  screen_h = (yl + 1) << draw_yeig ();
}

static void window_block (int *b, int flags, unsigned char work_bg, const char *t, int tsize, int title_flags)
{
  memset (b, 0, 23 * sizeof (int));
  b[0] = 0; b[1] = 0; b[2] = 400; b[3] = 400;
  b[6] = -1;
  b[7] = flags;
  ((unsigned char *) b)[32] = 7;  ((unsigned char *) b)[33] = 2;
  ((unsigned char *) b)[34] = 7;  ((unsigned char *) b)[35] = work_bg;
  ((unsigned char *) b)[36] = 3;  ((unsigned char *) b)[37] = 1;
  ((unsigned char *) b)[38] = 12;
  b[10] = 0; b[11] = -screen_h; b[12] = screen_w; b[13] = 0;
  b[14] = title_flags;
  b[15] = 3 << 12;                     /* work area: click */
  b[16] = 1;
  b[17] = (900 << 16) | 1600;
  b[18] = (int) t; b[19] = -1; b[20] = tsize;
  b[21] = 0;
}

int mainwin_create (void)
{
  int b[23];
  read_screen ();
  snprintf (title, sizeof title, "%s", APP_NAME);
  /* The main window: moveable; back, close, title, toggle size, adjust
     size; no scroll bars; we draw the background (colour 255). */
  window_block (b, (int) 0xAF000002, 255, title, sizeof title, 0x13D);
  if (_swix (Wimp_CreateWindow, _IN (1) | _OUT (0), b, &win))
    return -1;
  build_rows ();
  /* The inspector: a child with only a vertical scroll bar, its
     background the panel's grey (we draw it). */
  window_block (b, (int) 0x90000002, 255, "", 1, 0x0);
  b[17] = 0;
  b[11] = -insp_extent;
  b[12] = INSP_W;
  if (_swix (Wimp_CreateWindow, _IN (1) | _OUT (0), b, &insp))
    return -1;
  insp_extent = 0;
  mainwin_inspector_changed ();
  mainwin_update_title ();
  return 0;
}

/* --- layout */
static void toolbar_button (int k, int r[4])
{
  /* Left group: Inspector, Import, New; middle: the three layouts; right:
     Loupe, Export, Full Screen. Each 140 wide, work area coordinates. */
  static const int group_of[TB_COUNT] = { 0, 0, 0, 1, 1, 1, 2, 2, 2 };
  int w = 140, x;
  int g = group_of[k], i = k - (g == 0 ? 0 : g == 1 ? TB_BROWSER : TB_LOUPE);
  if (g == 0)
    x = 16 + i * w;
  else if (g == 1)
    x = vis_w / 2 - 3 * w / 2 + i * w;
  else
    x = vis_w - 16 - (3 - i) * w;
  r[0] = x; r[2] = x + w - 8;
  r[3] = -8; r[1] = -TB_H + 8;
}

static void tab_rect (int k, int r[4])
{
  int w = (INSP_W + SB_W) / 3;
  r[0] = GAP + k * w; r[2] = r[0] + w - 4;
  r[3] = -TB_H - 6; r[1] = -TB_H - TAB_H + 8;
}

/* Opens child w at the given visible area (screen coordinates) inside the main window. */
static void open_child (int w, int x0, int y0, int x1, int y1, int keep_scroll)
{
  int b[9];
  b[0] = w;
  if (keep_scroll && _swix (Wimp_GetWindowState, _IN (1), b) == NULL)
    ;
  else
    {
      b[5] = 0;
      b[6] = 0;
    }
  b[0] = w;
  b[1] = x0; b[2] = y0; b[3] = x1; b[4] = y1;
  b[7] = -1;                           /* on top, within the parent */
  _swix (Wimp_OpenWindow, _INR (1, 4), b, TASK_WORD, win, 0);
}

void mainwin_open_child (int *block)
{
  /* A child's own Open_Window_Request (it scrolled): keep it nested. */
  block[7] = -1;
  _swix (Wimp_OpenWindow, _INR (1, 4), block, TASK_WORD, win, 0);
}

static void close_child (int w)
{
  int b[1];
  b[0] = w;
  _swix (Wimp_CloseWindow, _IN (1), b);
}

/* Places the children for the main window's visible area (screen coordinates). */
static void place_children (int mx0, int my0, int mx1, int my1)
{
  int ax0 = mx0, ay1 = my1 - TB_H, ax1 = mx1, ay0 = my0 + BB_H, b[9];
  int show_browser = mode != LAYOUT_VIEWER, show_viewer = mode != LAYOUT_BROWSER;
  if (insp_on && !full_screen)
    {
      insp_vis = ay1 - TAB_H - ay0;
      apply_extent ();
      open_child (insp, mx0 + GAP, ay0, mx0 + GAP + INSP_W, ay1 - TAB_H, 1);
      ax0 = mx0 + GAP + INSP_W + SB_W + GAP;
    }
  else
    close_child (insp);
  if (show_viewer)
    {
      int vy0 = mode == LAYOUT_SPLIT ? ay0 + (ay1 - ay0) * 30 / 100 : ay0;
      b[0] = viewer_window ();
      b[1] = ax0; b[2] = vy0; b[3] = ax1; b[4] = ay1;
      b[5] = 0; b[6] = 0; b[7] = -1;
      viewer_place (b);
      ay1 = vy0 - GAP;
    }
  else
    viewer_unplace ();
  if (show_browser)
    {
      b[0] = browser_window ();
      b[1] = ax0; b[2] = ay0; b[3] = ax1 - SB_W; b[4] = ay1;
      browser_place (b);
    }
  else
    browser_unplace ();
}

static void open_main (int *b)
{
  b[5] = 0;
  b[6] = 0;
  _swix (Wimp_OpenWindow, _IN (1), b);
  _swix (Wimp_GetWindowState, _IN (1), b);   /* where the Wimp put it */
  vis_w = b[3] - b[1];
  vis_h = b[4] - b[2];
  place_children (b[1], b[2], b[3], b[4]);
  if (!is_open)
    {
      is_open = 1;
      _swix (Wimp_SetCaretPosition, _INR (0, 5), win, -1, 0, 0, 1 << 25, -1);
    }
}

void mainwin_open (int *block)
{
  int b[9];
  if (!block)
    {
      b[0] = win;
      if (is_open && _swix (Wimp_GetWindowState, _IN (1), b) == NULL)
        b[7] = -1;
      else
        {
          int w = screen_w - 160 < 3600 ? screen_w - 160 : 3600;
          int h = screen_h - 300;
          b[0] = win;
          b[1] = (screen_w - w) / 2; b[2] = 160;
          b[3] = b[1] + w; b[4] = 160 + h;
          b[5] = 0; b[6] = 0; b[7] = -1;
        }
      block = b;
    }
  if (block[3] - block[1] != vis_w || block[4] - block[2] != vis_h)
    _swix (Wimp_ForceRedraw, _INR (0, 4), win, 0, -0x10000000, 0x10000000, 0);
  open_main (block);
}

void mainwin_close (void)
{
  int b[1];
  if (!is_open)
    return;
  viewer_shutdown ();
  mode = LAYOUT_BROWSER;               /* it opens again on the Browser */
  full_screen = 0;
  close_child (insp);
  browser_close ();
  b[0] = win;
  _swix (Wimp_CloseWindow, _IN (1), b);
  is_open = 0;
  mainwin_update_title ();
}

int mainwin_is_open (void) { return is_open; }

/* Places everything again (the layout or the inspector changed). */
static void relayout (void)
{
  int b[9];
  b[0] = win;
  if (is_open && _swix (Wimp_GetWindowState, _IN (1), b) == NULL)
    {
      _swix (Wimp_ForceRedraw, _INR (0, 4), win, 0, -0x10000000, 0x10000000, 0);
      place_children (b[1], b[2], b[3], b[4]);
    }
}

void mainwin_set_mode (int m)
{
  if (m < LAYOUT_BROWSER || m > LAYOUT_VIEWER || m == mode)
    return;
  /* The Viewer shows the Browser's focused photo (or the first). */
  if (m != LAYOUT_BROWSER)
    {
      int f = browser_focus ();
      if (f < 0 && !viewer_has_photo () && browser_count () > 0)
        f = 0;
      if (f >= 0 && f != viewer_index ())
        viewer_show (f);
      if (!viewer_has_photo ())
        return;
    }
  mode = m;
  relayout ();
  mainwin_update_title ();
}

void mainwin_cycle_mode (void)
{
  mainwin_set_mode (mode == LAYOUT_VIEWER ? LAYOUT_BROWSER : mode + 1);
}

void mainwin_toggle_inspector (void)
{
  insp_on = !insp_on;
  relayout ();
}

void mainwin_set_tab (int t)
{
  if (t < TAB_LIBRARY || t > TAB_ADJUST)
    return;
  tab = t;
  if (!insp_on)
    insp_on = 1;
  /* Adjusting needs a photo in the Viewer: Split View, as Aperture does. */
  if (tab == TAB_ADJUST && mode == LAYOUT_BROWSER && browser_count () > 0)
    mainwin_set_mode (LAYOUT_SPLIT);
  relayout ();
  mainwin_inspector_changed ();
}

void mainwin_toggle_full_screen (void)
{
  int b[9];
  b[0] = win;
  if (!is_open || _swix (Wimp_GetWindowState, _IN (1), b))
    return;
  if (!full_screen)
    {
      memcpy (normal_box, &b[1], sizeof normal_box);
      full_screen = 1;
      if (mode == LAYOUT_BROWSER)
        mainwin_set_mode (LAYOUT_VIEWER);
      /* The whole screen: the title bar just above the top edge. */
      b[1] = 0; b[2] = 0; b[3] = screen_w; b[4] = screen_h;
    }
  else
    {
      full_screen = 0;
      memcpy (&b[1], normal_box, sizeof normal_box);
    }
  b[7] = -1;
  open_main (b);
  relayout ();
}

/* The inspector's contents changed (tab, photo, panel height). */
void mainwin_inspector_changed (void)
{
  if (tab == TAB_LIBRARY)
    {
      build_rows ();
      mainwin_inspector_extent (nrows * LIB_ROW + 40);
    }
  else if (tab == TAB_ADJUST)
    mainwin_inspector_extent (viewer_panel_height ());
  else
    mainwin_inspector_extent (1400);
  if (insp != -1)
    _swix (Wimp_ForceRedraw, _INR (0, 4), insp, 0, -0x10000000, 0x10000000, 0);
  _swix (Wimp_ForceRedraw, _INR (0, 4), win, 0, -TB_H - TAB_H, INSP_W + SB_W + GAP, -TB_H);
}

/* The inspector's extent: its contents, but never shorter than it is
   shown (the Wimp would shrink a nested window to its extent). */
static void apply_extent (void)
{
  int b[4], h = insp_content > insp_vis ? insp_content : insp_vis;
  if (h == insp_extent || insp == -1)
    return;
  insp_extent = h;
  b[0] = 0; b[1] = -h; b[2] = INSP_W; b[3] = 0;
  _swix (Wimp_SetExtent, _INR (0, 1), insp, b);
}

void mainwin_inspector_extent (int h)
{
  insp_content = h;
  apply_extent ();
}

void mainwin_force_inspector (int x0, int y0, int x1, int y1)
{
  if (insp != -1)
    _swix (Wimp_ForceRedraw, _INR (0, 4), insp, x0, y0, x1, y1);
}

/* --- title */
void mainwin_set_title (const char *t)
{
  if (strcmp (t, title) != 0)
    {
      snprintf (title, sizeof title, "%s", t);
      if (win != -1)
        _swix (Wimp_ForceRedraw, _INR (0, 2), win, TASK_WORD, 3);
    }
}

const char *mainwin_title (void) { return title; }

void mainwin_update_title (void)
{
  char t[220], status[160];
  browser_status (status, sizeof status);
  if (mode != LAYOUT_BROWSER && viewer_has_photo ())
    {
      char v[160];
      viewer_title (v, sizeof v);
      snprintf (t, sizeof t, "%s - %s%s", APP_NAME, v, status);
    }
  else
    {
      char b[120];
      browser_title (b, sizeof b);
      snprintf (t, sizeof t, "%s - %s%s", APP_NAME, b, status);
    }
  mainwin_set_title (t);
}

/* --- the Library tab */
static void add_row (int kind, cat_id id, const char *text, int heading)
{
  lib_row *more = realloc (rows, sizeof *rows * (size_t) (nrows + 1));
  if (!more)
    return;
  rows = more;
  rows[nrows].kind = kind;
  rows[nrows].id = id;
  rows[nrows].heading = heading;
  snprintf (rows[nrows].text, sizeof rows[nrows].text, "%s", text);
  nrows++;
}

static void build_rows (void)
{
  cat_group *g;
  int n, k;
  char t[96];
  nrows = 0;
  add_row (-1, 0, "LIBRARY", 1);
  add_row (SRC_ALL, 0, "Photos", 0);
  add_row (SRC_FLAGGED, 0, "Flagged", 0);
  add_row (SRC_REJECTED, 0, "Rejected", 0);
  add_row (SRC_LAST_IMPORT, 0, "Last Import", 0);
  if (cat_groups (lib_cat (app_library ()), SRC_PROJECT, &g, &n) == 0)
    {
      add_row (-1, 0, "PROJECTS", 1);
      if (!n)
        add_row (-1, 0, "Drag a folder of photos in", 0);
      for (k = 0; k < n; k++)
        {
          snprintf (t, sizeof t, "%s (%d)", g[k].name, g[k].count);
          add_row (SRC_PROJECT, g[k].id, t, 0);
        }
      free (g);
    }
  if (cat_groups (lib_cat (app_library ()), SRC_ALBUM, &g, &n) == 0)
    {
      add_row (-1, 0, "ALBUMS", 1);
      if (!n)
        add_row (-1, 0, "None yet: New makes one", 0);
      for (k = 0; k < n; k++)
        {
          snprintf (t, sizeof t, "%s (%d)", g[k].name, g[k].count);
          add_row (SRC_ALBUM, g[k].id, t, 0);
        }
      free (g);
    }
}

void mainwin_library_changed (void)
{
  if (tab == TAB_LIBRARY)
    {
      build_rows ();
      mainwin_inspector_extent (nrows * LIB_ROW + 40);
      mainwin_force_inspector (0, -0x10000000, 0x10000000, 0);
    }
}

static int row_selected (const lib_row *r)
{
  const cat_source *s = browser_source ();
  if (r->kind < 0 || r->kind != s->kind)
    return 0;
  return r->kind == SRC_PROJECT || r->kind == SRC_ALBUM ? r->id == s->id : 1;
}

/* A small icon for each kind of row. */
static void row_icon (int kind, int x, int y, unsigned col)
{
  switch (kind)
    {
    case SRC_ALL:                      /* a photo: a frame with a mountain */
      draw_frame (x, y, x + 32, y + 26, col);
      draw_triangle (x + 4, y + 4, x + 16, y + 18, x + 26, y + 4, col);
      break;
    case SRC_FLAGGED:                  /* a flag */
      draw_rect (x + 4, y, x + 8, y + 30, col);
      draw_rect (x + 8, y + 14, x + 28, y + 30, col);
      break;
    case SRC_REJECTED:                 /* a cross */
      draw_line (x + 4, y + 2, x + 28, y + 28, col);
      draw_line (x + 4, y + 28, x + 28, y + 2, col);
      draw_line (x + 6, y + 2, x + 30, y + 28, col);
      draw_line (x + 6, y + 28, x + 30, y + 2, col);
      break;
    case SRC_LAST_IMPORT:              /* a clock */
      draw_circle (x + 16, y + 15, 14, col);
      draw_line (x + 16, y + 15, x + 16, y + 26, col);
      draw_line (x + 16, y + 15, x + 24, y + 15, col);
      break;
    case SRC_PROJECT:                  /* a project: a stack of prints */
      draw_frame (x + 6, y + 6, x + 32, y + 28, col);
      draw_frame (x, y, x + 26, y + 22, col);
      break;
    case SRC_ALBUM:                    /* an album: a book */
      draw_frame (x + 2, y, x + 28, y + 30, col);
      draw_rect (x + 2, y, x + 7, y + 30, col);
      break;
    }
}

static void draw_library (int ox, int oy, int cy0, int cy1)
{
  int i;
  for (i = 0; i < nrows; i++)
    {
      int y1 = -16 - i * LIB_ROW, y0 = y1 - LIB_ROW;
      const lib_row *r = &rows[i];
      if (y1 < cy0 - oy || y0 > cy1 - oy)
        continue;
      if (r->heading)
        draw_text (ox + 16, oy + y0 + 12, r->text, COL_QUIET, COL_PANEL);
      else if (row_selected (r))
        {
          draw_rect (ox + 8, oy + y0 + 4, ox + INSP_W - 8, oy + y1 - 4, COL_ACCENT);
          row_icon (r->kind, ox + 32, oy + y0 + 12, COL_WHITE);
          draw_text (ox + 80, oy + y0 + 16, r->text, COL_WHITE, COL_ACCENT);
        }
      else
        {
          row_icon (r->kind, ox + 32, oy + y0 + 12, COL_ACCENT);
          draw_text (ox + (r->kind < 0 ? 32 : 80), oy + y0 + 16, r->text, r->kind < 0 ? COL_QUIET : COL_TEXT, COL_PANEL);
        }
    }
}

static void library_click (int wy)
{
  int i = (-wy - 16) / LIB_ROW;
  cat_source s = *browser_source ();
  if (wy > -16 || i < 0 || i >= nrows || rows[i].kind < 0)
    return;
  s.kind = rows[i].kind;
  s.id = rows[i].id;
  if (s.kind == SRC_REJECTED)
    s.min_rating = 0;
  browser_set_source (&s);
  if (mode == LAYOUT_VIEWER)
    mainwin_set_mode (LAYOUT_BROWSER);
  mainwin_force_inspector (0, -0x10000000, 0x10000000, 0);
}

/* --- the Info tab */
static void info_line (int *y, int ox, int oy, const char *label, const char *value)
{
  draw_text (ox + 24, oy + *y, label, COL_QUIET, COL_PANEL);
  draw_text (ox + 210, oy + *y, value, COL_TEXT, COL_PANEL);
  *y -= 48;
}

static void draw_info (int ox, int oy)
{
  cat_item it;
  cat_details d;
  char v[160];
  int y = -60, i = mode != LAYOUT_BROWSER ? viewer_index () : browser_focus ();
  if (i < 0 || !browser_item (i, &it) || cat_get_details (lib_cat (app_library ()), it.id, &d))
    {
      draw_text (ox + 24, oy + y, "Select a photo to see its details.", COL_QUIET, COL_PANEL);
      return;
    }
  draw_text (ox + 24, oy + y, it.name, COL_TEXT, COL_PANEL);
  y -= 64;
  draw_rect (ox + 16, oy + y + 40, ox + INSP_W - 16, oy + y + 42, 0xB0B0B000u);
  snprintf (v, sizeof v, "%d x %d", d.width, d.height);
  info_line (&y, ox, oy, "Size", v);
  info_line (&y, ox, oy, "Taken", d.taken[0] ? d.taken : "-");
  snprintf (v, sizeof v, "%s %s", d.make, d.model);
  info_line (&y, ox, oy, "Camera", d.make[0] || d.model[0] ? v : "-");
  if (d.exposure > 0)
    {
      if (d.exposure < 1)
        snprintf (v, sizeof v, "1/%.0f s", 1 / d.exposure);
      else
        snprintf (v, sizeof v, "%.1f s", d.exposure);
      info_line (&y, ox, oy, "Shutter", v);
    }
  if (d.fnumber > 0)
    {
      snprintf (v, sizeof v, "f/%.1f", d.fnumber);
      info_line (&y, ox, oy, "Aperture", v);
    }
  if (d.iso > 0)
    {
      snprintf (v, sizeof v, "%d", d.iso);
      info_line (&y, ox, oy, "ISO", v);
    }
  if (d.focal > 0)
    {
      snprintf (v, sizeof v, "%.0f mm", d.focal);
      info_line (&y, ox, oy, "Focal length", v);
    }
  snprintf (v, sizeof v, "%s", it.rating == RATING_REJECT ? "Rejected" : it.rating ? "" : "Not rated");
  if (it.rating > 0)
    {
      int k;
      for (k = 0; k < it.rating; k++)
        strcat (v, "*");
    }
  info_line (&y, ox, oy, "Rating", v);
  info_line (&y, ox, oy, "Flag", it.flag ? "Flagged" : "-");
  info_line (&y, ox, oy, "Adjusted", it.edited ? "Yes" : "No");
  y -= 8;
  draw_text (ox + 24, oy + y, "File", COL_QUIET, COL_PANEL);
  y -= 44;
  {
    /* The path, wrapped to the panel's width. */
    const char *p = d.path;
    while (*p)
      {
        char line[160];
        int n = 0;
        while (p[n] && n < (int) sizeof line - 1)
          {
            line[n] = p[n];
            line[n + 1] = '\0';
            if (draw_text_width (line) > INSP_W - 48)
              break;
            n++;
          }
        if (n == 0)
          n = 1;
        memcpy (line, p, (size_t) n);
        line[n] = '\0';
        draw_text (ox + 24, oy + y, line, COL_TEXT, COL_PANEL);
        y -= 40;
        p += n;
      }
  }
}

void mainwin_info_changed (void)
{
  mainwin_status_changed ();
  if (tab == TAB_INFO)
    mainwin_force_inspector (0, -0x10000000, 0x10000000, 0);
}

/* --- drawing */
static void draw_icon (int k, int cx, int cy, unsigned col)
{
  switch (k)
    {
    case TB_INSPECTOR:                 /* a page with lines */
      draw_frame (cx - 16, cy - 20, cx + 16, cy + 20, col);
      draw_rect (cx - 10, cy + 8, cx + 10, cy + 11, col);
      draw_rect (cx - 10, cy, cx + 10, cy + 3, col);
      draw_rect (cx - 10, cy - 8, cx + 4, cy - 5, col);
      break;
    case TB_IMPORT:                    /* an arrow down into a tray */
      draw_rect (cx - 4, cy - 2, cx + 4, cy + 22, col);
      draw_triangle (cx - 14, cy, cx + 14, cy, cx, cy - 14, col);
      draw_rect (cx - 22, cy - 22, cx + 22, cy - 18, col);
      break;
    case TB_NEW:                       /* a plus */
      draw_rect (cx - 4, cy - 18, cx + 4, cy + 18, col);
      draw_rect (cx - 18, cy - 4, cx + 18, cy + 4, col);
      break;
    case TB_BROWSER:                   /* a grid */
      {
        int i, j;
        for (i = 0; i < 3; i++)
          for (j = 0; j < 3; j++)
            draw_rect (cx - 21 + i * 15, cy - 21 + j * 15, cx - 9 + i * 15, cy - 9 + j * 15, col);
      }
      break;
    case TB_SPLIT:                     /* a picture over a strip */
      draw_rect (cx - 22, cy - 2, cx + 22, cy + 20, col);
      draw_rect (cx - 22, cy - 20, cx - 10, cy - 8, col);
      draw_rect (cx - 6, cy - 20, cx + 6, cy - 8, col);
      draw_rect (cx + 10, cy - 20, cx + 22, cy - 8, col);
      break;
    case TB_VIEWER:                    /* one picture */
      draw_rect (cx - 22, cy - 18, cx + 22, cy + 18, col);
      break;
    case TB_LOUPE:                     /* a magnifier */
      draw_circle (cx - 4, cy + 4, 14, col);
      draw_circle (cx - 4, cy + 4, 12, col);
      draw_line (cx + 6, cy - 6, cx + 20, cy - 20, col);
      draw_line (cx + 8, cy - 6, cx + 22, cy - 20, col);
      break;
    case TB_EXPORT:                    /* an arrow up out of a tray */
      draw_rect (cx - 4, cy - 12, cx + 4, cy + 10, col);
      draw_triangle (cx - 14, cy + 8, cx + 14, cy + 8, cx, cy + 22, col);
      draw_rect (cx - 22, cy - 22, cx + 22, cy - 18, col);
      break;
    case TB_FULL:                      /* corners */
      draw_rect (cx - 22, cy + 14, cx - 8, cy + 18, col);
      draw_rect (cx - 22, cy + 4, cx - 18, cy + 18, col);
      draw_rect (cx + 8, cy - 18, cx + 22, cy - 14, col);
      draw_rect (cx + 18, cy - 18, cx + 22, cy - 4, col);
      break;
    }
}

static int tool_on (int k)
{
  switch (k)
    {
    case TB_INSPECTOR: return insp_on && !full_screen;
    case TB_BROWSER: return mode == LAYOUT_BROWSER;
    case TB_SPLIT: return mode == LAYOUT_SPLIT;
    case TB_VIEWER: return mode == LAYOUT_VIEWER;
    case TB_LOUPE: return viewer_loupe () != 0;
    case TB_FULL: return full_screen;
    }
  return 0;
}

static void draw_toolbar (int ox, int oy)
{
  static const char *const labels[TB_COUNT] = { "Inspector", "Import", "New", "Browser", "Split View", "Viewer",
                                                "Loupe", "Export", "Full Screen" };
  int k;
  draw_rect (ox, oy - TB_H, ox + vis_w, oy, COL_TOOLBAR);
  draw_rect (ox, oy - TB_H, ox + vis_w, oy - TB_H + 2, COL_TOOLBAR_EDGE);
  for (k = 0; k < TB_COUNT; k++)
    {
      int r[4], cx;
      unsigned col = COL_ICON, bg = COL_TOOLBAR;
      toolbar_button (k, r);
      cx = (r[0] + r[2]) / 2;
      if (tool_on (k))
        {
          bg = 0xB8B8B800u;
          draw_rect (ox + r[0], oy + r[1], ox + r[2], oy + r[3], bg);
          col = COL_ACCENT;
        }
      draw_icon (k, ox + cx, oy - 46, col);
      draw_text (ox + cx - draw_text_width (labels[k]) / 2, oy - TB_H + 18, labels[k], COL_TEXT, bg);
    }
}

static void draw_tabs (int ox, int oy)
{
  static const char *const names[3] = { "Library", "Info", "Adjustments" };
  int k;
  if (!insp_on || full_screen)
    return;
  for (k = 0; k < 3; k++)
    {
      int r[4];
      unsigned bg = k == tab ? COL_ACCENT : 0xC8C8C800u, fg = k == tab ? COL_WHITE : COL_TEXT;
      tab_rect (k, r);
      draw_rect (ox + r[0], oy + r[1], ox + r[2], oy + r[3], bg);
      draw_text (ox + (r[0] + r[2] - draw_text_width (names[k])) / 2, oy + r[1] + 18, names[k], fg, bg);
    }
}

/* The bar along the bottom, as Aperture's: what is shown and selected,
   the thumbnail size, and a grip to resize the window by (the window has
   no scroll bar for the Wimp's own Adjust Size icon to sit on). */
static void size_rect (int k, int r[4])
{
  r[0] = vis_w - GRIP_W - 16 - (3 - k) * 116;
  r[2] = r[0] + 108;
  r[1] = -vis_h + 8;
  r[3] = -vis_h + BB_H - 8;
}

static void draw_bottom (int ox, int oy)
{
  static const char *const sizes[3] = { "Small", "Medium", "Large" };
  char t[120];
  cat_id *ids;
  int nsel = browser_selection (&ids), k, y0 = oy - vis_h, gx = ox + vis_w;
  free (ids);
  draw_rect (ox, y0, ox + vis_w, y0 + BB_H, COL_TOOLBAR);
  draw_rect (ox, y0 + BB_H - 2, ox + vis_w, y0 + BB_H, COL_TOOLBAR_EDGE);
  if (browser_count () == 1)
    snprintf (t, sizeof t, "1 photo");
  else
    snprintf (t, sizeof t, "%d photos", browser_count ());
  if (nsel)
    snprintf (t + strlen (t), sizeof t - strlen (t), "  \xB7  %d selected", nsel);
  draw_text (ox + 24, y0 + 16, t, COL_TEXT, COL_TOOLBAR);
  for (k = 0; k < 3; k++)
    {
      int r[4], on = browser_size () == k;
      unsigned bg = on ? COL_ACCENT : COL_TOOLBAR;
      size_rect (k, r);
      draw_rect (ox + r[0], oy + r[1], ox + r[2], oy + r[3], bg);
      draw_text (ox + (r[0] + r[2] - draw_text_width (sizes[k])) / 2, oy + r[1] + 8, sizes[k],
                 on ? COL_WHITE : COL_TEXT, bg);
    }
  /* The grip: three diagonal lines in the corner. */
  for (k = 0; k < 3; k++)
    {
      int d = 12 + k * 12;
      draw_line (gx - 8 - d, y0 + 8, gx - 8, y0 + 8 + d, COL_QUIET);
      draw_line (gx - 8 - d + 2, y0 + 8, gx - 8, y0 + 8 + d - 2, COL_WHITE);
    }
}

void mainwin_status_changed (void)
{
  if (win != -1 && is_open)
    _swix (Wimp_ForceRedraw, _INR (0, 4), win, 0, -vis_h, vis_w, -vis_h + BB_H);
}

void mainwin_redraw (int *b)
{
  int more = 0;
  _swix (Wimp_RedrawWindow, _IN (1) | _OUT (0), b, &more);
  while (more)
    {
      int ox = b[1] - b[5], oy = b[4] - b[6];
      draw_rect (ox, oy - vis_h - 64, ox + vis_w + 64, oy, 0x9C9C9C00u);   /* behind the panes */
      draw_toolbar (ox, oy);
      draw_tabs (ox, oy);
      draw_bottom (ox, oy);
      _swix (Wimp_GetRectangle, _IN (1) | _OUT (0), b, &more);
    }
}

void mainwin_inspector_redraw (int *b)
{
  int more = 0;
  _swix (Wimp_RedrawWindow, _IN (1) | _OUT (0), b, &more);
  while (more)
    {
      int ox = b[1] - b[5], oy = b[4] - b[6];
      draw_rect (b[7], b[8], b[9], b[10], COL_PANEL);
      if (tab == TAB_LIBRARY)
        draw_library (ox, oy, b[8], b[10]);
      else if (tab == TAB_INFO)
        draw_info (ox, oy);
      else
        viewer_panel_draw (ox, oy);
      _swix (Wimp_GetRectangle, _IN (1) | _OUT (0), b, &more);
    }
}

/* --- input */
static void toolbar_click (int k)
{
  int ptr[5] = { 0 };
  _swix (Wimp_GetPointerInfo, _IN (1), ptr);
  switch (k)
    {
    case TB_INSPECTOR: mainwin_toggle_inspector (); break;
    case TB_IMPORT:
      app_report ("To import, drag photos or a folder of photos from a directory display into the Lumen window, or onto its icon on the icon bar.");
      break;
    case TB_NEW: menus_open_new_menu (ptr[0], ptr[1]); break;
    case TB_BROWSER: mainwin_set_mode (LAYOUT_BROWSER); break;
    case TB_SPLIT: mainwin_set_mode (LAYOUT_SPLIT); break;
    case TB_VIEWER: mainwin_set_mode (LAYOUT_VIEWER); break;
    case TB_LOUPE:
      if (mode == LAYOUT_BROWSER)
        mainwin_set_mode (LAYOUT_SPLIT);
      viewer_toggle_loupe ();
      break;
    case TB_EXPORT: exportbox_open (mode != LAYOUT_BROWSER, ptr[0], ptr[1]); break;
    case TB_FULL: mainwin_toggle_full_screen (); break;
    }
  _swix (Wimp_ForceRedraw, _INR (0, 4), win, 0, -TB_H, 0x10000000, 0);
}

void mainwin_click (int *b)
{
  int st[9], wx, wy, k;
  st[0] = win;
  if (_swix (Wimp_GetWindowState, _IN (1), st))
    return;
  if (b[2] == Button_Menu)
    {
      menus_open_window_menu (b[0], b[1]);
      return;
    }
  _swix (Wimp_SetCaretPosition, _INR (0, 5), win, -1, 0, 0, 1 << 25, -1);
  wx = b[0] - (st[1] - st[5]);
  wy = b[1] - (st[4] - st[6]);
  for (k = 0; k < TB_COUNT; k++)
    {
      int r[4];
      toolbar_button (k, r);
      if (wx >= r[0] && wx <= r[2] && wy >= r[1] && wy <= r[3])
        {
          toolbar_click (k);
          return;
        }
    }
  if (wy < -vis_h + BB_H)
    {
      if (wx > vis_w - GRIP_W)
        {
          /* Wimp_DragBox type 2: the Wimp drags the window's size. */
          int db[10] = { 0 };
          db[0] = win;
          db[1] = 2;
          _swix (Wimp_DragBox, _IN (1), db);
          return;
        }
      for (k = 0; k < 3; k++)
        {
          int r[4];
          size_rect (k, r);
          if (wx >= r[0] && wx <= r[2])
            {
              browser_set_size (k);
              mainwin_status_changed ();
            }
        }
      return;
    }
  if (insp_on && !full_screen)
    for (k = 0; k < 3; k++)
      {
        int r[4];
        tab_rect (k, r);
        if (wx >= r[0] && wx <= r[2] && wy >= r[1] && wy <= r[3])
          {
            mainwin_set_tab (k);
            return;
          }
      }
}

void mainwin_inspector_click (int *b)
{
  int st[9];
  if (b[2] == Button_Menu)
    {
      if (tab == TAB_ADJUST)
        menus_open_viewer_menu (b[0], b[1]);
      else
        menus_open_window_menu (b[0], b[1]);
      return;
    }
  if (tab == TAB_ADJUST)
    {
      viewer_panel_click (b);
      return;
    }
  st[0] = insp;
  if (_swix (Wimp_GetWindowState, _IN (1), st))
    return;
  if (tab == TAB_LIBRARY)
    library_click (b[1] - (st[4] - st[6]));
}

/* Keys for the whole window; 1 if used. */
int mainwin_key (int key)
{
  switch (key)
    {
    case 'v': case 'V': mainwin_cycle_mode (); return 1;
    case 'i': case 'I': mainwin_toggle_inspector (); return 1;
    case 'w': case 'W':
      mainwin_set_tab (tab == TAB_ADJUST ? TAB_LIBRARY : tab + 1);
      return 1;
    case 'f': case 'F': mainwin_toggle_full_screen (); return 1;
    }
  return 0;
}

void mainwin_mode_changed (void)
{
  read_screen ();
  relayout ();
}
