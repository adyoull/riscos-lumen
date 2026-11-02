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
#include "exif.h"
#include "colour.h"
#include "rawjob.h"

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

enum { TB_INSPECTOR, TB_IMPORT, TB_NEW, TB_UNDO, TB_REDO, TB_BROWSER, TB_SPLIT, TB_VIEWER, TB_LOUPE, TB_EXPORT, TB_FULL, TB_COUNT };

static int win = -1, insp = -1, is_open, full_screen;
static int mode = LAYOUT_BROWSER, insp_on = 1, tab = TAB_LIBRARY;
static int vis_w = 2800, vis_h = 1700, screen_w = 3840, screen_h = 2160;
static int normal_box[4];              /* where the window was before full screen */
static int normal_mode;                /* and its layout */
static int frame_change;               /* the next open sets the window flags (full screen on or off) */
#define FLAGS_FRAMED 0xAF000002u       /* moveable; back, close, title, toggle size; new format */
#define FLAGS_BARE 0x80000002u         /* full screen: no frame at all */
static char title[220];
static int search_icon = -1;           /* the bottom bar's writable icon */
static char search_text[64];
static char search_valid[] = "Pptr_write;KN";
static int insp_extent = 2000, insp_content = 400, insp_vis = 400;

/* The Library tab's rows: what each shows. */
typedef struct { int kind; cat_id id; char text[80]; int heading, depth, watched, smart; } lib_row;   /* depth: a folder album's */
static lib_row *rows;
static int nrows;
static void build_rows (void);
static void place_icons (void);
static void info_load (int force);
static int info_height;
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
  window_block (b, (int) FLAGS_FRAMED, 255, title, sizeof title, 0x13D);
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
  /* The search field (Wimp_CreateIcon, PRM p746's neighbour): text, border,
     centred vertically, filled, indirected, writable; black on white. */
  {
    int ib[9];
    ib[0] = win;
    ib[1] = 0; ib[2] = -400; ib[3] = 400; ib[4] = -352;   /* placed by place_icons */
    ib[5] = (int) (0x07000000u | 0xF000u | 0x100u | 0x20u | 0x10u | 0x04u | 0x01u);
    ib[6] = (int) search_text;
    ib[7] = (int) search_valid;
    ib[8] = sizeof search_text;
    if (_swix (Wimp_CreateIcon, _IN (1) | _OUT (0), ib, &search_icon))
      search_icon = -1;
  }
  return 0;
}

/* --- layout */
static void toolbar_button (int k, int r[4])
{
  /* Left group: Inspector, Import, New, Undo, Redo; middle: the three
     layouts; right: Loupe, Export, Full Screen. Each 140 wide (narrower
     in a narrow window, down to 90), work area coordinates; the middle
     group is centred unless that would run into the others. */
  static const int group_of[TB_COUNT] = { 0, 0, 0, 0, 0, 1, 1, 1, 2, 2, 2 };
  int w = (vis_w - 32 - 80) / TB_COUNT, x;
  int g = group_of[k], i = k - (g == 0 ? 0 : g == 1 ? TB_BROWSER : TB_LOUPE);
  w = w > 140 ? 140 : w < 90 ? 90 : w;
  if (g == 0)
    x = 16 + i * w;
  else if (g == 1)
    {
      int lo = 16 + TB_BROWSER * w + 40, hi = vis_w - 16 - 3 * w - 40 - 3 * w;
      x = vis_w / 2 - 3 * w / 2;
      x = x < lo ? lo : x > hi && hi >= lo ? hi : x;
      x += i * w;
    }
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

/* The inspector back to its top (another tab, or another photo's details). */
static void insp_to_top (void)
{
  int st[9];
  st[0] = insp;
  if (insp == -1 || _swix (Wimp_GetWindowState, _IN (1), st) || st[6] == 0 || !(st[8] & (1 << 16)))
    return;
  st[6] = 0;
  st[7] = -1;
  _swix (Wimp_OpenWindow, _INR (1, 4), st, TASK_WORD, win, 0);
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
  if (full_screen)
    {
      /* Only the photo: the viewer over everything. */
      close_child (insp);
      browser_unplace ();
      b[0] = viewer_window ();
      b[1] = mx0; b[2] = my0; b[3] = mx1; b[4] = my1;
      b[5] = 0; b[6] = 0; b[7] = -1;
      viewer_place (b);
      return;
    }
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
  if (frame_change)
    {
      /* Extended syntax (PRM p738): a top-level window (R3 -1), nesting
         flags bit 0: new window flags at +32 - the frame off or on. */
      b[8] = (int) (full_screen ? FLAGS_BARE : FLAGS_FRAMED);
      _swix (Wimp_OpenWindow, _INR (1, 4), b, TASK_WORD, -1, 1);
      frame_change = 0;
    }
  else
    _swix (Wimp_OpenWindow, _IN (1), b);
  _swix (Wimp_GetWindowState, _IN (1), b);   /* where the Wimp put it */
  vis_w = b[3] - b[1];
  vis_h = b[4] - b[2];
  place_icons ();
  place_children (b[1], b[2], b[3], b[4]);
  if (!is_open)
    {
      is_open = 1;
      _swix (Wimp_SetCaretPosition, _INR (0, 5), win, -1, 0, 0, 1 << 25, -1);
      if (app_prefs ()->verify)
        lib_verify_start (app_library ());   /* a disc may be back (or gone) since it was shut */
      app_watch_now ();                /* and anything new in the watched folders */
    }
}

static int closed_at[9], have_closed_at;   /* where the window was when it was last closed */

void mainwin_open (int *block)
{
  int b[9];
  if (!block)
    {
      b[0] = win;
      if (is_open && _swix (Wimp_GetWindowState, _IN (1), b) == NULL)
        b[7] = -1;
      else if (have_closed_at)
        {
          /* Where it was, the size it was. */
          memcpy (b, closed_at, sizeof b);
          b[0] = win;
          b[7] = -1;
        }
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

int mainwin_full_screen (void) { return full_screen; }

void mainwin_close (void)
{
  int b[1];
  if (!is_open)
    return;
  viewer_shutdown ();
  mode = LAYOUT_BROWSER;               /* it opens again on the Browser */
  if (!full_screen)
    {
      closed_at[0] = win;
      have_closed_at = _swix (Wimp_GetWindowState, _IN (1), closed_at) == NULL;
    }
  if (full_screen)
    frame_change = 1;                  /* with its frame back */
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
  if (mode != LAYOUT_VIEWER)
    browser_show_focus ();             /* Split View's short strip: the photo in focus in sight */
  mainwin_update_title ();
  if (tab != TAB_LIBRARY)
    mainwin_inspector_changed ();      /* Adjustments and Info follow the layout's photo */
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
  if (t != tab)
    insp_to_top ();
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
      /* The photo needs to be there: the Viewer's, or the Browser's focus. */
      normal_mode = mode;
      if (mode == LAYOUT_BROWSER)
        mainwin_set_mode (LAYOUT_VIEWER);
      if (mode == LAYOUT_BROWSER)      /* no photos at all */
        return;
      memcpy (normal_box, &b[1], sizeof normal_box);
      full_screen = 1;
      mode = LAYOUT_VIEWER;
      /* The whole screen, without a frame. */
      b[1] = 0; b[2] = 0; b[3] = screen_w; b[4] = screen_h;
    }
  else
    {
      full_screen = 0;
      memcpy (&b[1], normal_box, sizeof normal_box);
      mode = normal_mode;
    }
  frame_change = 1;
  b[7] = -1;
  open_main (b);
  relayout ();
  mainwin_update_title ();
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
    {
      info_load (1);                   /* read again: the photo may have changed while hidden */
      mainwin_inspector_extent (info_height);
    }
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
  char t[220], status[1] = "";        /* what is going on is in the status area now */
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
  rows[nrows].depth = 0;
  rows[nrows].watched = 0;
  rows[nrows].smart = 0;
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
  {
    /* Only when there are any: photos whose files can't be found. */
    int miss = cat_count_missing (lib_cat (app_library ()));
    if (miss)
      {
        snprintf (t, sizeof t, "Missing Files (%d)", miss);
        add_row (SRC_MISSING, 0, t, 0);
      }
  }
  if (cat_groups (lib_cat (app_library ()), SRC_PROJECT, &g, &n) == 0)
    {
      add_row (-1, 0, "PROJECTS", 1);
      if (!n)
        add_row (-1, 0, "Drag a folder of photos in", 0);
      for (k = 0; k < n; k++)
        {
          cat_folder *f;
          int nf, j, w = 0, al = 0;
          snprintf (t, sizeof t, "%s (%d)", g[k].name, g[k].count);
          add_row (SRC_PROJECT, g[k].id, t, 0);
          cat_project_watching (lib_cat (app_library ()), g[k].id, &w, &al);
          if (nrows)
            rows[nrows - 1].watched = w;
          /* Its folders' albums, nested under it. */
          if (al && cat_folder_albums (lib_cat (app_library ()), g[k].id, &f, &nf) == 0)
            {
              for (j = 0; j < nf; j++)
                {
                  snprintf (t, sizeof t, "%s (%d)", f[j].name, f[j].count);
                  add_row (SRC_ALBUM, f[j].id, t, 0);
                  if (nrows)
                    rows[nrows - 1].depth = f[j].depth;
                }
              free (f);
            }
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
          if (nrows)
            rows[nrows - 1].smart = g[k].smart;
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
    case SRC_MISSING:                  /* a frame with a question mark: something gone */
      draw_frame (x, y, x + 32, y + 26, col);
      draw_text (x + 10, y + 2, "?", col, col == COL_WHITE ? COL_ACCENT : COL_PANEL);
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
    case -2:                           /* a folder (an album made from one) */
      draw_frame (x, y, x + 32, y + 22, col);
      draw_rect (x, y + 22, x + 12, y + 26, col);
      break;
    case -3:                           /* a smart album: a magnifying glass */
      draw_circle (x + 13, y + 17, 11, col);
      draw_circle (x + 13, y + 17, 10, col);
      draw_line (x + 20, y + 9, x + 30, y - 1, col);
      draw_line (x + 21, y + 10, x + 31, y, col);
      draw_line (x + 19, y + 9, x + 29, y - 1, col);
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
      int in = r->depth * 32;           /* folder albums sit under their project */
      int kind = r->depth ? -2 : r->smart ? -3 : r->kind;   /* -2: a folder's icon, -3 a smart album's */
      if (r->heading)
        draw_text (ox + 16, oy + y0 + 12, r->text, COL_QUIET, COL_PANEL);
      else if (row_selected (r))
        {
          draw_rect (ox + 8, oy + y0 + 4, ox + INSP_W - 8, oy + y1 - 4, COL_ACCENT);
          row_icon (kind, ox + 32 + in, oy + y0 + 12, COL_WHITE);
          draw_text (ox + 80 + in, oy + y0 + 16, r->text, COL_WHITE, COL_ACCENT);
          if (r->watched)
            draw_text (ox + INSP_W - 24 - draw_text_width ("watched"), oy + y0 + 16, "watched", COL_WHITE, COL_ACCENT);
        }
      else
        {
          row_icon (kind, ox + 32 + in, oy + y0 + 12, COL_ACCENT);
          draw_text (ox + (r->kind < 0 ? 32 : 80 + in), oy + y0 + 16, r->text, r->kind < 0 ? COL_QUIET : COL_TEXT, COL_PANEL);
          if (r->watched)
            draw_text (ox + INSP_W - 24 - draw_text_width ("watched"), oy + y0 + 16, "watched", COL_QUIET, COL_PANEL);
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

/* --- the Info tab: the photo's details, then every EXIF tag in its file */
#define INFO_LINE 40                   /* one line of text */
#define INFO_LABEL_X 24
#define INFO_VALUE_X 300               /* labels left of this, values right */
#define INFO_HEAD 1                    /* a row that is a group heading */
#define INFO_EDIT 2                    /* clicking it opens the Caption and keywords window */
#define INFO_LABELS 4                  /* the colour label's dots to pick from; value: the label now */
#define LABEL_STEP 40                  /* between the dots */

typedef struct { char label[48]; char value[256]; int flags, top, bottom; } info_row;
static info_row *irows;
static int nirows;
static cat_id info_id = -1;            /* the version irows describe */
static int info_has_exif;

static void info_add (const char *label, const char *value, int flags)
{
  info_row *more = realloc (irows, sizeof *irows * (size_t) (nirows + 1));
  if (!more)
    return;
  irows = more;
  snprintf (irows[nirows].label, sizeof irows[nirows].label, "%s", label);
  snprintf (irows[nirows].value, sizeof irows[nirows].value, "%s", value);
  irows[nirows].flags = flags;
  nirows++;
}

static const char *exif_group_seen;

static void info_exif_tag (void *ctx, const char *group, const char *name, const char *value)
{
  (void) ctx;
  if (!exif_group_seen || strcmp (exif_group_seen, group) != 0)
    {
      char h[40];
      size_t k;
      snprintf (h, sizeof h, "%s", group);
      for (k = 0; h[k]; k++)
        if (h[k] >= 'a' && h[k] <= 'z')
          h[k] = (char) (h[k] - 32);
      info_add (h, "", INFO_HEAD);
      exif_group_seen = group;
    }
  info_add (name, value, 0);
}

/* The photo the Info tab describes: the Viewer's, else the Browser's focus. */
static int info_index (void)
{
  return mode != LAYOUT_BROWSER && viewer_index () >= 0 ? viewer_index () : browser_focus ();
}

/* Splits s into lines no wider than width (at spaces or full stops where it
   can). Returns the number of lines; with draw, plots them from (x, y) down. */
static int wrap (const char *s, int width, int draw, int x, int y, unsigned fg)
{
  int lines = 0;
  if (!*s)
    return 1;
  while (*s)
    {
      char line[260];
      int n = 0, brk = 0;
      while (s[n] && n < (int) sizeof line - 1)
        {
          line[n] = s[n];
          line[n + 1] = '\0';
          if (draw_text_width (line) > width)
            break;
          if (s[n] == ' ' || s[n] == '.' || s[n] == ',')
            brk = n + 1;
          n++;
        }
      if (s[n] && brk > 0)
        n = brk;                       /* back to the last space or stop */
      if (n == 0)
        n = 1;
      memcpy (line, s, (size_t) n);
      line[n] = '\0';
      if (draw)
        draw_text (x, y - lines * INFO_LINE, line, fg, COL_PANEL);
      lines++;
      s += n;
      while (*s == ' ')
        s++;
    }
  return lines;
}

/* Lays the rows out from the top; with draw, plots those in cy0..cy1
   (screen y). Returns the height used. */
static int info_layout (int draw, int ox, int oy, int cy0, int cy1)
{
  int y = -24, k;
  for (k = 0; k < nirows; k++)
    {
      const info_row *r = &irows[k];
      int top = y, h;
      if (r->flags & INFO_HEAD)
        {
          y -= k ? 20 : 0;
          if (draw && oy + y >= cy0 && oy + y - 56 <= cy1)
            {
              draw_rect (ox + 16, oy + y - 2, ox + INSP_W - 16, oy + y, 0xB0B0B000u);
              draw_text (ox + INFO_LABEL_X, oy + y - 44, r->label, COL_QUIET, COL_PANEL);
            }
          y -= 60;
          continue;
        }
      if (!r->value[0])                /* a line of its own: the name, the file */
        {
          h = wrap (r->label, INSP_W - 2 * INFO_LABEL_X, 0, 0, 0, 0);
          if (draw && oy + top >= cy0 && oy + top - h * INFO_LINE <= cy1)
            wrap (r->label, INSP_W - 2 * INFO_LABEL_X, 1, ox + INFO_LABEL_X, oy + top - 32, COL_TEXT);
          y -= h * INFO_LINE + 8;
          continue;
        }
      {
        int hl = wrap (r->label, INFO_VALUE_X - INFO_LABEL_X - 16, 0, 0, 0, 0);
        int hv = wrap (r->value, INSP_W - INFO_VALUE_X - 16, 0, 0, 0, 0);
        h = hl > hv ? hl : hv;
        if (r->flags & INFO_LABELS)
          h = 1;
        irows[k].top = top;
        irows[k].bottom = top - h * INFO_LINE - 8;
        if (draw && oy + top >= cy0 && oy + top - h * INFO_LINE <= cy1)
          {
            wrap (r->label, INFO_VALUE_X - INFO_LABEL_X - 16, 1, ox + INFO_LABEL_X, oy + top - 32, COL_QUIET);
            if (r->flags & INFO_LABELS)
              {
                /* None, then the seven colours; the photo's ringed. */
                int j, now = atoi (r->value);
                for (j = 0; j < CAT_LABELS; j++)
                  {
                    int cx = ox + INFO_VALUE_X + 12 + j * LABEL_STEP, cy = oy + top - 20;
                    if (j == now)
                      {
                        draw_circle (cx, cy, 17, COL_TEXT);
                        draw_circle (cx, cy, 16, COL_TEXT);
                      }
                    draw_label_dot (cx, cy, 12, j);
                  }
              }
            else
            wrap (r->value, INSP_W - INFO_VALUE_X - 16, 1, ox + INFO_VALUE_X, oy + top - 32,
                  (r->flags & INFO_EDIT) && !strncmp (r->value, "None: click", 11) ? COL_ACCENT : COL_TEXT);
          }
        y -= h * INFO_LINE + 8;
      }
    }
  return -y + 24;
}

/* Reads the details and the file's EXIF again if the photo changed. */
static void info_load (int force)
{
  cat_item it;
  cat_details d;
  char v[160];
  int i = info_index ();
  cat_id id = i >= 0 && browser_item (i, &it) ? it.id : 0;
  if (id == info_id && !force)
    return;
  if (id != info_id)
    insp_to_top ();                    /* another photo's details start at the top */
  info_id = id;
  nirows = 0;
  info_has_exif = 0;
  exif_group_seen = NULL;
  if (!id || cat_get_details (lib_cat (app_library ()), it.id, &d))
    {
      info_add ("Select a photo to see its details.", "", 0);
      info_height = info_layout (0, 0, 0, 0, 0);
      return;
    }
  info_add (it.name, "", 0);
  snprintf (v, sizeof v, "%d x %d pixels", d.width, d.height);
  info_add ("Size", v, 0);
  if (it.rating == RATING_REJECT)
    snprintf (v, sizeof v, "Rejected");
  else if (it.rating > 0)
    snprintf (v, sizeof v, "%d star%s", it.rating, it.rating == 1 ? "" : "s");
  else
    snprintf (v, sizeof v, "Not rated");
  info_add ("Rating", v, 0);
  info_add ("Flag", it.flag ? "Flagged" : "Not flagged", 0);
  snprintf (v, sizeof v, "%d", it.label > 0 && it.label < CAT_LABELS ? it.label : 0);
  info_add ("Label", v, INFO_LABELS);
  info_add ("Adjusted", it.edited ? "Yes" : "No", 0);
  {
    char cap[CAT_CAPTION_MAX], kw[CAT_KEYWORDS_MAX];
    cat_get_tags (lib_cat (app_library ()), it.id, cap, sizeof cap, kw, sizeof kw);
    if (strlen (cap) > 240)
      strcpy (cap + 237, "...");
    if (strlen (kw) > 240)
      strcpy (kw + 237, "...");
    info_add ("Caption", cap[0] ? cap : "None: click to add one", INFO_EDIT);
    info_add ("Keywords", kw[0] ? kw : "None: click to add some", INFO_EDIT);
  }
  if (d.versions > 1)
    {
      snprintf (v, sizeof v, "%d of %d of this photo", d.version_no, d.versions);
      info_add ("Version", v, 0);
    }
  if (it.stack)
    {
      snprintf (v, sizeof v, "%d photos%s, %s", it.stack_count, it.stack_pick ? ", this the pick" : "",
                it.stack_open ? "open" : "closed");
      info_add ("Stack", v, 0);
    }
  info_add ("File", d.path, 0);
  if (d.missing)
    info_add ("Missing", "The file can't be found there. If it has moved, drag the folder it is in now into Lumen.", 0);
  /* The EXIF block is near the start of the file: 256 KB is plenty. */
  {
    FILE *f = fopen (d.path, "rb");
    unsigned char *buf = malloc (256 * 1024);
    size_t got = 0;
    if (f && buf)
      got = fread (buf, 1, 256 * 1024, f);
    if (f)
      fclose (f);
    if (buf && got && img_probe (buf, got) == IMG_JPEG)
      {
        /* The colour space: Adobe RGB and Display P3 are converted (colour.h). */
        char prof[64], cv[120];
        int cs = colour_from_jpeg (buf, got, prof, sizeof prof);
        if (cs == CS_ADOBE || cs == CS_P3)
          snprintf (cv, sizeof cv, "%s, converted to sRGB", prof[0] ? prof : colour_name (cs));
        else if (cs == CS_OTHER)
          snprintf (cv, sizeof cv, "%s, shown as sRGB", prof[0] ? prof : "Unknown");
        else
          snprintf (cv, sizeof cv, "%s", prof[0] ? prof : "sRGB");
        info_add ("Colour", cv, 0);
      }
    if (buf && got && img_probe_named (buf, got, d.path) == IMG_RAW)
      {
        /* A camera raw file: through the plug-in. TIFF-based ones (NEF,
           CR2, ARW, DNG...) have their EXIF where a TIFF has. */
        info_add ("Format", rawjob_available () ? "Camera raw, developed by the LibRaw plug-in"
                                                : "Camera raw: needs the LibRaw plug-in (LumenRaw, from PackMan)", 0);
        if ((buf[0] == 'I' && buf[1] == 'I') || (buf[0] == 'M' && buf[1] == 'M'))
          info_has_exif = exif_list_tiff (buf, got, info_exif_tag, NULL) > 0;
      }
    else if (buf && got)
      info_has_exif = exif_list (buf, got, info_exif_tag, NULL) > 0;
    free (buf);
    if (!info_has_exif)
      {
        info_add ("EXIF", "", INFO_HEAD);
        info_add (!f ? "The file can't be read." : "No EXIF data in this file.", "", 0);
      }
  }
  info_height = info_layout (0, 0, 0, 0, 0);
}

/* The Info tab's rows as text, "Label: value" a line (for the tests). */
const char *mainwin_info_text (void)
{
  static char t[4096];
  int k;
  size_t len = 0;
  t[0] = '\0';
  for (k = 0; k < nirows && len < sizeof t - 1; k++)
    {
      int w = snprintf (t + len, sizeof t - len, "%s%s%s\n", irows[k].label, irows[k].value[0] ? ": " : "", irows[k].value);
      if (w < 0)
        break;
      len += (size_t) w;
    }
  return t;
}

static void draw_info (int ox, int oy, int cy0, int cy1)
{
  info_layout (1, ox, oy, cy0, cy1);
}

void mainwin_info_changed (void)
{
  mainwin_status_changed ();
  if (tab == TAB_INFO)
    {
      info_load (1);
      mainwin_inspector_extent (info_height);
      mainwin_force_inspector (0, -0x10000000, 0x10000000, 0);
    }
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
    case TB_UNDO:                      /* an arrow turning back to the left */
    case TB_REDO:                      /* and its mirror */
      {
        int s = k == TB_UNDO ? 1 : -1;
#define MX(v) (cx + s * (v))
        draw_triangle (MX (-24), cy + 8, MX (-8), cy + 22, MX (-8), cy - 6, col);
        draw_rect (MX (-9), cy + 4, MX (10), cy + 12, col);      /* along the top */
        draw_rect (MX (2), cy - 20, MX (10), cy + 12, col);      /* down the side */
        draw_rect (MX (-8), cy - 20, MX (10), cy - 12, col);     /* back along the bottom */
#undef MX
      }
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

/* Undo and Redo work on the photo being adjusted (in the Viewer or Split
   View); shaded when there is nothing to take back or do again. */
static int tool_enabled (int k)
{
  if (k == TB_UNDO)
    return mode != LAYOUT_BROWSER && viewer_has_photo () && viewer_can_undo ();
  if (k == TB_REDO)
    return mode != LAYOUT_BROWSER && viewer_has_photo () && viewer_can_redo ();
  return 1;
}

/* After every event: redraws Undo and Redo when they change. */
void mainwin_tick (void)
{
  static int was = -1;
  int now = tool_enabled (TB_UNDO) | tool_enabled (TB_REDO) << 1;
  if (now != was && win != -1 && is_open)
    {
      int r0[4], r1[4];
      toolbar_button (TB_UNDO, r0);
      toolbar_button (TB_REDO, r1);
      _swix (Wimp_ForceRedraw, _INR (0, 4), win, r0[0], -TB_H, r1[2], 0);
    }
  was = now;
}

void mainwin_toolbar_button (int k, int r[4])
{
  toolbar_button (k, r);
}

static void draw_toolbar (int ox, int oy)
{
  static const char *const labels[TB_COUNT] = { "Inspector", "Import", "New", "Undo", "Redo", "Browser", "Split View",
                                                "Viewer", "Loupe", "Export", "Full Screen" };
  int k;
  draw_rect (ox, oy - TB_H, ox + vis_w, oy, COL_TOOLBAR);
  draw_rect (ox, oy - TB_H, ox + vis_w, oy - TB_H + 2, COL_TOOLBAR_EDGE);
  for (k = 0; k < TB_COUNT; k++)
    {
      int r[4], cx;
      unsigned col = tool_enabled (k) ? COL_ICON : 0xB0B0B000u, bg = COL_TOOLBAR;
      toolbar_button (k, r);
      cx = (r[0] + r[2]) / 2;
      if (tool_on (k))
        {
          bg = 0xB8B8B800u;
          draw_rect (ox + r[0], oy + r[1], ox + r[2], oy + r[3], bg);
          col = COL_ACCENT;
        }
      draw_icon (k, ox + cx, oy - 46, col);
      draw_text (ox + cx - draw_text_width (labels[k]) / 2, oy - TB_H + 18, labels[k],
                 tool_enabled (k) ? COL_TEXT : 0x90909000u, bg);
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

/* The rating filter's button and the search field, left of the sizes. */
#define SEARCH_W 420
#define RATING_W 250
static void search_rect (int r[4])
{
  int sr[4];
  size_rect (0, sr);
  r[2] = sr[0] - 24;
  r[0] = r[2] - SEARCH_W;
  r[1] = -vis_h + 8;
  r[3] = -vis_h + BB_H - 8;
}

static void rating_rect (int r[4])
{
  int q[4];
  search_rect (q);
  r[2] = q[0] - 24 - draw_text_width ("Search") - 12;
  r[0] = r[2] - RATING_W;
  r[1] = q[1];
  r[3] = q[3];
}

/* Moves the search field with the bottom bar. */
static void place_icons (void)
{
  int r[4];
  if (search_icon < 0)
    return;
  search_rect (r);
  _swix (Wimp_ResizeIcon, _INR (0, 5), win, search_icon, r[0], r[1], r[2], r[3]);
}

int mainwin_search_icon (void) { return search_icon; }

static const char *rating_label (void)
{
  static const char *const names[6] = { "Any rating", "1 star +", "2 stars +", "3 stars +", "4 stars +", "5 stars" };
  int m = browser_source ()->min_rating;
  return names[m >= 0 && m <= 5 ? m : 0];
}

/* Searches for what is in the field now. */
static void search_apply (void)
{
  cat_source s = *browser_source ();
  if (strcmp (s.search, search_text) == 0)
    return;
  snprintf (s.search, sizeof s.search, "%s", search_text);
  browser_set_source (&s);
  if (mode != LAYOUT_BROWSER && browser_count () > 0)
    browser_focus_on (0);
  else if (browser_count () == 0 && mode != LAYOUT_BROWSER)
    mainwin_set_mode (LAYOUT_BROWSER);
  mainwin_status_changed ();
}

void mainwin_set_search (const char *text)
{
  snprintf (search_text, sizeof search_text, "%s", text);
  if (search_icon >= 0)
    _swix (Wimp_SetIconState, _INR (1, 1), (int[]) { win, search_icon, 0, 0 });
  search_apply ();
}

/* A key in the search field: Return or any typing searches (the KN
   validation tells us of every key); Escape clears it. 1 if used. */
int mainwin_search_key (int key)
{
  if (key == KEY_ESCAPE)
    {
      search_text[0] = '\0';
      if (search_icon >= 0)
        {
          _swix (Wimp_SetIconState, _INR (1, 1), (int[]) { win, search_icon, 0, 0 });
          _swix (Wimp_SetCaretPosition, _INR (0, 5), win, search_icon, 0, 0, -1, 0);
        }
      search_apply ();
      return 1;
    }
  if (key == KEY_RETURN || (key >= 0x20 && key < 0x7F) || key == 8 || key == 0x7F || (key >= 0xA0 && key < 0x100)
      || key == KEY_LEFT || key == KEY_RIGHT || key == 0x18B || key == KEY_END || key == KEY_HOME)
    {
      search_apply ();
      return 1;
    }
  return 0;
}

/* The status area: what Lumen is doing, with a progress donut; from
   after the photo count to the rating button. Click: the Activity window. */
static int stat_x0 = -1, stat_x1 = -1;

static void draw_status (int ox, int y0, int x0, int x1)
{
  char text[200], fit[220];
  int pct, busy, cx = x0 + 20, cy = y0 + BB_H / 2;
  stat_x0 = x0;
  stat_x1 = x1;
  if (x1 - x0 < 48)
    {
      stat_x0 = stat_x1 = -1;
      return;
    }
  activity_status (text, sizeof text, &pct, &busy);
  if (busy)
    draw_donut (ox + cx, cy, 18, 10, pct, pct < 0 ? activity_spin () : 0, 0xCC770000u, COL_WHITE, COL_TOOLBAR);
  else
    {
      /* Nothing running: an empty ring, a quiet label. */
      draw_donut (ox + cx, cy, 18, 10, 0, 0, COL_QUIET, 0xB8B8B800u, COL_TOOLBAR);
    }
  activity_fit (text, x1 - (x0 + 52), fit, sizeof fit);
  if (fit[0])
    draw_text (ox + x0 + 52, y0 + 16, fit, busy ? COL_TEXT : COL_QUIET, COL_TOOLBAR);
}

int mainwin_status_at (int wx)
{
  return stat_x0 >= 0 && wx >= stat_x0 && wx <= stat_x1;
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
  {
    /* As much as fits left of the rating button: then without the
       selection, then nothing (a narrow window). */
    int r[4], room, used = 0;
    char full[140];
    rating_rect (r);
    room = r[0] - 24 - 24;
    snprintf (full, sizeof full, "%s", t);
    if (nsel)
      snprintf (full + strlen (full), sizeof full - strlen (full), "  \xB7  %d selected", nsel);
    /* The count and selection, leaving the status area at least 360 (it
       has the rest); then without the selection; then nothing. */
    if (draw_text_width (full) <= room - 360)
      used = draw_text_width (full), draw_text (ox + 24, y0 + 16, full, COL_TEXT, COL_TOOLBAR);
    else if (draw_text_width (t) <= room - 200)
      used = draw_text_width (t), draw_text (ox + 24, y0 + 16, t, COL_TEXT, COL_TOOLBAR);
    if (used)
      draw_rect (ox + 24 + used + 22, y0 + 12, ox + 24 + used + 24, y0 + BB_H - 12, COL_TOOLBAR_EDGE);
    draw_status (ox, y0, used ? 24 + used + 46 : 16, r[0] - 24);
  }
  {
    int r[4], q[4];
    const char *rl = rating_label ();
    rating_rect (r);
    search_rect (q);
    draw_rect (ox + r[0], oy + r[1], ox + r[2], oy + r[3], COL_WHITE);
    draw_frame (ox + r[0], oy + r[1], ox + r[2], oy + r[3], COL_TOOLBAR_EDGE);
    draw_text (ox + r[0] + 14, oy + r[1] + 8, rl, browser_source ()->min_rating ? COL_ACCENT : COL_TEXT, COL_WHITE);
    draw_triangle (ox + r[2] - 34, oy + r[1] + 26, ox + r[2] - 14, oy + r[1] + 26, ox + r[2] - 24, oy + r[1] + 14, COL_ICON);
    draw_text (ox + q[0] - 12 - draw_text_width ("Search"), y0 + 16, "Search", COL_TEXT, COL_TOOLBAR);
  }
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
        draw_info (ox, oy, b[8], b[10]);
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
    case TB_UNDO: if (tool_enabled (k)) viewer_undo (); break;
    case TB_REDO: if (tool_enabled (k)) viewer_redo (); break;
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
  if (search_icon >= 0 && b[4] == search_icon)
    return;                            /* the Wimp puts the caret in the field */
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
      {
        int r[4];
        rating_rect (r);
        if (wx >= r[0] && wx <= r[2])
          menus_open_rating_menu (b[0] - 64, st[4] - st[6] + r[3] + 4);
      }
      if (mainwin_status_at (wx))
        activitywin_open ();
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

/* What part of the main window a screen point is over, for help.c. */
int mainwin_part_at (int x, int y, int icon)
{
  int st[9], wx, wy, k;
  st[0] = win;
  if (_swix (Wimp_GetWindowState, _IN (1), st))
    return MW_NONE;
  if (search_icon >= 0 && icon == search_icon)
    return MW_SEARCH;
  wx = x - (st[1] - st[5]);
  wy = y - (st[4] - st[6]);
  for (k = 0; k < TB_COUNT; k++)
    {
      int r[4];
      toolbar_button (k, r);
      if (wx >= r[0] && wx <= r[2] && wy >= r[1] && wy <= r[3])
        return MW_TOOLBAR + k;
    }
  if (wy < -vis_h + BB_H)
    {
      int r[4];
      if (wx > vis_w - GRIP_W)
        return MW_GRIP;
      for (k = 0; k < 3; k++)
        {
          size_rect (k, r);
          if (wx >= r[0] && wx <= r[2])
            return MW_SIZE;
        }
      rating_rect (r);
      if (wx >= r[0] && wx <= r[2])
        return MW_RATING;
      return mainwin_status_at (wx) ? MW_ACTIVITY : MW_STATUS;
    }
  if (insp_on && !full_screen)
    for (k = 0; k < 3; k++)
      {
        int r[4];
        tab_rect (k, r);
        if (wx >= r[0] && wx <= r[2] && wy >= r[1] && wy <= r[3])
          return MW_TAB + k;
      }
  return MW_NONE;
}

void mainwin_inspector_click (int *b)
{
  int st[9];
  if (b[2] == Button_Menu)
    {
      if (tab == TAB_ADJUST)
        menus_open_viewer_menu (b[0], b[1]);
      else
        {
          /* Over a project or album: its own menu (rename, delete). */
          int i = -1;
          st[0] = insp;
          if (tab == TAB_LIBRARY && _swix (Wimp_GetWindowState, _IN (1), st) == NULL)
            {
              int wy = b[1] - (st[4] - st[6]);
              i = wy <= -16 ? (-wy - 16) / LIB_ROW : -1;
            }
          if (i >= 0 && i < nrows && (rows[i].kind == SRC_PROJECT || rows[i].kind == SRC_ALBUM))
            menus_open_group_menu (b[0], b[1], rows[i].kind, rows[i].id);
          else
            menus_open_window_menu (b[0], b[1]);
        }
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
  else if (tab == TAB_INFO && info_id > 0)
    {
      /* The caption or keywords: the window to change them, for this photo. */
      int wy = b[1] - (st[4] - st[6]), wx = b[0] - (st[1] - st[5]), k;
      for (k = 0; k < nirows; k++)
        if ((irows[k].flags & INFO_EDIT) && wy <= irows[k].top && wy > irows[k].bottom)
          {
            tagbox_open_ids (&info_id, 1, b[0], b[1]);
            break;
          }
        else if ((irows[k].flags & INFO_LABELS) && wy <= irows[k].top && wy > irows[k].bottom)
          {
            /* A dot: that label (the one it has already: none). */
            int j = (wx - INFO_VALUE_X - 12 + LABEL_STEP / 2) / LABEL_STEP;
            if (wx >= INFO_VALUE_X - 8 && j >= 0 && j < CAT_LABELS)
              {
                cat_set_label (lib_cat (app_library ()), &info_id, 1, j == atoi (irows[k].value) ? 0 : j);
                browser_after_change ();
                mainwin_info_changed ();
              }
            break;
          }
    }
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
