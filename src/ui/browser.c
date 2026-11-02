/* browser.c: the library window. A grid of thumbnails with names, star
   ratings and flags, scrolling vertically; the number of columns follows
   the window's width. The window is built in code (Wimp_CreateWindow block,
   PRM p720ff); the work area origin is its top left, y going negative. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <kernel.h>
#include <swis.h>
#include "ui.h"

#define MARGIN 16
#define TEXT_H 36
#define STARS_H 40

/* Aperture's dark grey browser, with RISC OS's light blue (Wimp colour
   15, #00BBFF) for what is rated and focused. &BBGGRR00. */
#define COL_BG 0x3C3C3C00u
#define COL_SELECTED 0x60606000u
#define COL_PLACEHOLDER 0x55555500u
#define COL_FAILED 0x40407800u
#define COL_TEXT 0xDCDCDC00u
#define COL_TEXT_SEL 0xFFFFFF00u
#define COL_BADGE_TEXT 0x1A1A1A00u
#define COL_ACCENT 0xFFBB0000u         /* #00BBFF */
#define COL_ACCENT_ON_DARK 0xFFBB0000u
#define COL_STAR_OFF 0x6A6A6A00u
#define COL_STAR_OFF_SEL 0x8A8A8A00u
#define COL_FLAG 0xB0E6F200u           /* #F2E6B0 */
#define COL_REJECT 0x7070F000u
#define COL_MISSING 0x2020C000u        /* #C02020 */
#define COL_STACK 0x4C4C4C00u          /* behind the photos of an open stack */
#define COL_STACK_BADGE 0x26262600u

static const int box_sizes[3] = { 160, 224, 320 };

static int win = -1;
static char title[200];
static cat_source src = { SRC_ALL, 0, 0, "" };
static cat_item *items;
static unsigned char *sel;
static int (*badge)[4];                /* each cell's stack badge (work area), as last drawn; x0 > x1: none */
static int n, cols = 1, size = 1, focus = -1, is_open;
static int layout_w = 256;            /* a cell's width: the window's width shared out */
static int vis_w = 1200, vis_h = 800;
static int screen_w = 1920, screen_h = 1080;

static void open_nested (int *b);
static void n_reset (void);
static int box (void) { return box_sizes[size]; }
static int min_cell_w (void) { return box () + 32; }
static int cell_w (void) { return layout_w; }
static int cell_h (void) { return box () + 24 + TEXT_H + STARS_H; }

int browser_window (void) { return win; }
const cat_source *browser_source (void) { return &src; }
int browser_count (void) { return n; }
int browser_size (void) { return size; }

static void read_screen (void)
{
  int xl = 1919, yl = 1079;
  _swix (OS_ReadModeVariable, _INR (0, 1) | _OUT (2), -1, 11, &xl);
  _swix (OS_ReadModeVariable, _INR (0, 1) | _OUT (2), -1, 12, &yl);
  screen_w = (xl + 1) << draw_xeig ();
  screen_h = (yl + 1) << draw_yeig ();
}

int browser_create (void)
{
  int b[23];
  read_screen ();
  memset (b, 0, sizeof b);
  b[0] = 96; b[1] = 160; b[2] = 96 + vis_w; b[3] = 160 + vis_h;
  b[6] = -1;
  /* A child of the main window (mainwin.c) with a vertical scroll bar. */
  b[7] = (int) 0x90000002;
  ((unsigned char *) b)[32] = 7;  ((unsigned char *) b)[33] = 2;   /* title fg, bg */
  ((unsigned char *) b)[34] = 7;  ((unsigned char *) b)[35] = 255; /* work area fg; we draw the background */
  ((unsigned char *) b)[36] = 3;  ((unsigned char *) b)[37] = 1;   /* scroll bar outer, inner */
  ((unsigned char *) b)[38] = 12;                                  /* title bg with input focus */
  b[10] = 0; b[11] = -screen_h; b[12] = screen_w; b[13] = 0;      /* work area extent */
  b[14] = 0x13D;              /* title: text, border, centred, filled, indirected */
  b[15] = 3 << 12;            /* work area: click */
  b[16] = 1;                  /* Wimp sprite area */
  b[17] = (64 << 16) | 64;    /* minimum height, width */
  snprintf (title, sizeof title, "%s", APP_NAME);
  b[18] = (int) title; b[19] = -1; b[20] = sizeof title;
  b[21] = 0;
  if (_swix (Wimp_CreateWindow, _IN (1) | _OUT (0), b, &win))
    return -1;
  browser_reload ();
  return 0;
}

/* --- geometry */
static void cell_rect (int i, int *x0, int *y0, int *x1, int *y1)
{
  int col = i % cols, row = i / cols;
  *x0 = MARGIN + col * cell_w ();
  *x1 = *x0 + cell_w ();
  *y1 = -(MARGIN + row * cell_h ());
  *y0 = *y1 - cell_h ();
}

static int index_at (int wx, int wy)
{
  int col, row, i, x0, y0, x1, y1;
  if (wx < MARGIN || wy > -MARGIN)
    return -1;
  col = (wx - MARGIN) / cell_w ();
  row = (-wy - MARGIN) / cell_h ();
  if (col >= cols)
    return -1;
  i = row * cols + col;
  if (i >= n)
    return -1;
  cell_rect (i, &x0, &y0, &x1, &y1);
  /* Clicks in the gap between cells select nothing. */
  if (wx < x0 + 8 || wx > x1 - 8 || wy > y1 - 4 || wy < y0 + 4)
    return -1;
  return i;
}

static int content_height (void)
{
  int rows = (n + cols - 1) / cols;
  return 2 * MARGIN + rows * cell_h ();
}

static void set_extent (void)
{
  int b[4];
  int h = content_height ();
  if (h < screen_h)
    h = screen_h;
  b[0] = 0; b[1] = -h; b[2] = screen_w; b[3] = 0;
  _swix (Wimp_SetExtent, _INR (0, 1), win, b);
}

static void redraw_all (void)
{
  if (win != -1)
    _swix (Wimp_ForceRedraw, _INR (0, 4), win, 0, -0x10000000, 0x10000000, 0);
}

static void redraw_cell (int i)
{
  int x0, y0, x1, y1;
  if (i < 0 || i >= n || win == -1)
    return;
  cell_rect (i, &x0, &y0, &x1, &y1);
  _swix (Wimp_ForceRedraw, _INR (0, 4), win, x0, y0, x1, y1);
}

static int columns_for (int width)
{
  int c = (width - 2 * MARGIN) / min_cell_w ();
  return c < 1 ? 1 : c;
}

/* Columns and cell width for a window this wide. 1 if they changed. */
static int relayout (int width)
{
  int c = columns_for (width), w = (width - 2 * MARGIN) / c;
  if (w < min_cell_w ())
    w = min_cell_w ();
  if (c == cols && w == layout_w)
    return 0;
  cols = c;
  layout_w = w;
  return 1;
}

/* --- title */
/* The source and how many photos: for the main window's title. */
void browser_title (char *out, size_t size)
{
  char name[80];
  switch (src.kind)
    {
    case SRC_FLAGGED: snprintf (name, sizeof name, "Flagged"); break;
    case SRC_REJECTED: snprintf (name, sizeof name, "Rejected"); break;
    case SRC_LAST_IMPORT: snprintf (name, sizeof name, "Last import"); break;
    case SRC_MISSING: snprintf (name, sizeof name, "Missing files"); break;
    case SRC_PROJECT:
    case SRC_ALBUM: cat_group_name (lib_cat (app_library ()), src.kind, src.id, name, sizeof name); break;
    default: snprintf (name, sizeof name, "All photos"); break;
    }
  {
    static const char *const rated[6] = { "", "  \xB7  1 star and up", "  \xB7  2 stars and up",
                                          "  \xB7  3 stars and up", "  \xB7  4 stars and up", "  \xB7  5 stars" };
    snprintf (out, size, "%s (%d)%s", name, n, src.min_rating > 0 && src.min_rating <= 5 ? rated[src.min_rating] : "");
  }
  if (src.search[0] && strlen (out) + strlen (src.search) + 12 < size)
    snprintf (out + strlen (out), size - strlen (out), "  \xB7  \"%s\"", src.search);
}

/* Importing, thumbnails, exports: what's going on in the background. */
void browser_status (char *status, size_t size)
{
  lib_progress p;
  lib_get_progress (app_library (), &p);
  status[0] = '\0';
  if (p.importing)
    snprintf (status, size, "  \xB7  Importing: %d found", p.found);
  else if (p.thumbs_pending > 0)
    snprintf (status, size, "  \xB7  Making thumbnails: %d to go", p.thumbs_pending);
  if (exports_busy ())
    {
      char e[60];
      exports_status (e, sizeof e);
      snprintf (status + strlen (status), size - strlen (status), "  \xB7  %s", e);
    }
}

void browser_update_title (void)
{
  mainwin_update_title ();
}

/* --- the list */
static int cmp_id (const void *a, const void *b)
{
  cat_id x = *(const cat_id *) a, y = *(const cat_id *) b;
  return x < y ? -1 : x > y;
}

void browser_reload (void)
{
  cat_item *ni = NULL;
  int nn = 0, i, nsel = 0;
  cat_id *keep = NULL, focus_id = focus >= 0 && focus < n ? items[focus].id : -1;
  int old_rows = (n + cols - 1) / cols;
  if (cat_list (lib_cat (app_library ()), &src, &ni, &nn))
    return;
  /* Keep the selection and focus by id. */
  if (n)
    keep = malloc (sizeof *keep * (size_t) n);
  for (i = 0; keep && i < n; i++)
    if (sel[i])
      keep[nsel++] = items[i].id;
  if (nsel)
    qsort (keep, (size_t) nsel, sizeof *keep, cmp_id);
  free (items);
  free (sel);
  items = ni;
  n = nn;
  sel = calloc ((size_t) (n ? n : 1), 1);
  free (badge);
  badge = calloc ((size_t) (n ? n : 1), sizeof *badge);
  focus = -1;
  for (i = 0; i < n; i++)
    {
      if (nsel && sel && bsearch (&items[i].id, keep, (size_t) nsel, sizeof *keep, cmp_id))
        sel[i] = 1;
      if (items[i].id == focus_id)
        focus = i;
    }
  free (keep);
  if ((n + cols - 1) / cols != old_rows)
    set_extent ();
  redraw_all ();
  viewer_list_changed ();
  browser_update_title ();
  mainwin_library_changed ();
  mainwin_status_changed ();
}

void browser_after_change (void)
{
  browser_reload ();
}

void browser_set_source (const cat_source *s)
{
  int b[9];
  src = *s;
  free (sel);
  sel = NULL;
  free (items);
  items = NULL;
  n = 0;
  focus = -1;
  thumbcache_clear_queue ();
  browser_reload ();
  /* Back to the top. */
  b[0] = win;
  if (is_open && _swix (Wimp_GetWindowState, _IN (1), b) == NULL)
    {
      b[6] = 0;
      open_nested (b);
    }
  mainwin_info_changed ();
}

void browser_set_size (int s)
{
  int b[9];
  if (s < 0 || s > 2 || s == size)
    return;
  size = s;
  relayout (vis_w);
  set_extent ();
  redraw_all ();
  b[0] = win;
  if (is_open && _swix (Wimp_GetWindowState, _IN (1), b) == NULL)
    {
      /* Keep the focused photo in view. */
      if (focus >= 0)
        {
          int x0, y0, x1, y1;
          cell_rect (focus, &x0, &y0, &x1, &y1);
          b[6] = y1 + MARGIN;
        }
      open_nested (b);
    }
}

int browser_selection (cat_id **ids)
{
  int i, k = 0;
  *ids = malloc (sizeof **ids * (size_t) (n ? n : 1));
  if (!*ids)
    return 0;
  for (i = 0; i < n; i++)
    if (sel[i])
      (*ids)[k++] = items[i].id;
  if (k == 0 && focus >= 0)            /* nothing selected: the focused photo */
    (*ids)[k++] = items[focus].id;
  return k;
}

void browser_select_all (int on)
{
  int i;
  for (i = 0; i < n; i++)
    if (sel[i] != (unsigned char) on)
      {
        sel[i] = (unsigned char) on;
        redraw_cell (i);
      }
  if (!on && focus >= 0)
    {
      /* Nothing selected means nothing: the focus goes too (else it would
         still count as the photo the keys work on). */
      int old = focus;
      focus = -1;
      redraw_cell (old);
      mainwin_info_changed ();
    }
  mainwin_status_changed ();
}

/* --- opening: always inside the main window (mainwin.c) */
static void open_nested (int *b)
{
  b[7] = -1;
  _swix (Wimp_OpenWindow, _INR (1, 4), b, TASK_WORD, mainwin_window (), 0);
}

/* At block's place: mainwin.c's layout, or our own Open_Window_Request (scrolling). */
void browser_place (int *block)
{
  vis_w = block[3] - block[1];
  vis_h = block[4] - block[2];
  if (relayout (vis_w))
    {
      set_extent ();
      redraw_all ();
    }
  open_nested (block);
  is_open = 1;
}

void browser_open (int *block)
{
  if (block)
    browser_place (block);
  else
    mainwin_open (NULL);
}

void browser_unplace (void)
{
  int b[1];
  if (!is_open)
    return;
  b[0] = win;
  _swix (Wimp_CloseWindow, _IN (1), b);
  is_open = 0;
}

void browser_close (void)
{
  browser_unplace ();
  thumbcache_clear_queue ();
}

void browser_mode_changed (void)
{
  read_screen ();
  set_extent ();
  redraw_all ();
}

/* --- drawing */
static void draw_name (const char *name, int x0, int x1, int y, unsigned fg, unsigned bg)
{
  char buf[72];
  int width = x1 - x0, len;
  snprintf (buf, sizeof buf, "%s", name);
  len = (int) strlen (buf);
  if (draw_text_width (buf) > width)
    {
      while (len > 1)
        {
          buf[--len] = '\0';
          if (len >= 3)
            {
              char t[76];
              snprintf (t, sizeof t, "%s...", buf);
              if (draw_text_width (t) <= width)
                {
                  snprintf (buf, sizeof buf, "%s", t);
                  break;
                }
            }
        }
    }
  draw_text (x0 + (width - draw_text_width (buf)) / 2, y, buf, fg, bg);
}

static void draw_cell (int i, int ox, int oy)
{
  int x0, y0, x1, y1, bx0, by0, bx1, by1, k, pic[4];
  const cat_item *it = &items[i];
  const thumb_sprite *s;
  int selected = sel[i];
  unsigned bg = selected ? COL_SELECTED : COL_BG, fg = selected ? COL_TEXT_SEL : COL_TEXT;
  cell_rect (i, &x0, &y0, &x1, &y1);
  x0 += ox; x1 += ox; y0 += oy; y1 += oy;
  bx0 = (x0 + x1 - box ()) / 2; bx1 = bx0 + box ();
  by1 = y1 - 16; by0 = by1 - box ();
  /* A thumbnail being made again (after an edit) keeps showing the old one. */
  s = thumbcache_get (it->id, it->thumb_state == THUMB_READY);
  if (it->thumb_state == THUMB_NONE)
    lib_want_thumb (app_library (), it->id);   /* on screen: made now (thumbnails are made lazily) */
  if (it->stack_count && it->stack_open && !selected)
    draw_rect (x0 + 2, y0 + 2, x1 - 2, y1 - 2, COL_STACK);   /* an open stack's photos, grouped */
  if (selected || i == focus)
    {
      /* Highlight up to just above the photo, not the empty box above a wide one. */
      int top = by0 + box () * 2 / 3;
      if (s)
        top = by0 + s->h * box () / (s->w > s->h ? s->w : s->h);
      top += 12;
      if (selected)
        draw_rect (x0 + 4, y0 + 4, x1 - 4, top, COL_SELECTED);
      if (i == focus)
        draw_frame (x0 + 4, y0 + 4, x1 - 4, top, COL_ACCENT);
    }
  if (s)
    draw_sprite_fit (s->area, s->sprite, s->w, s->h, bx0, by0, bx1, by1, pic);
  else
    {
      /* A 3:2 placeholder where a landscape photo will be. */
      int inset = box () / 8;
      pic[0] = bx0 + inset; pic[2] = bx1 - inset;
      pic[1] = by0; pic[3] = by0 + (pic[2] - pic[0]) * 2 / 3;
      draw_rect (pic[0], pic[1], pic[2], pic[3],
                 it->thumb_state == THUMB_FAILED ? COL_FAILED : COL_PLACEHOLDER);
    }
  if (it->edited && box () >= 224)     /* "Edited" on the photo's bottom right corner */
    {
      int tw = draw_text_width ("Edited");
      draw_rect (pic[2] - tw - 24, pic[1] + 8, pic[2] - 8, pic[1] + 48, 0x30303000u);
      draw_rect (pic[2] - tw - 21, pic[1] + 11, pic[2] - 11, pic[1] + 45, 0xFFFFFF00u);
      draw_text (pic[2] - tw - 16, pic[1] + 18, "Edited", COL_BADGE_TEXT, 0xFFFFFF00u);
    }
  if (it->missing)                     /* its file can't be found: on the bottom left */
    {
      const char *t = box () >= 224 ? "Missing" : "?";
      int tw = draw_text_width (t);
      draw_rect (pic[0] + 8, pic[1] + 8, pic[0] + tw + 24, pic[1] + 48, COL_MISSING);
      draw_text (pic[0] + 16, pic[1] + 18, t, COL_TEXT_SEL, COL_MISSING);
    }
  if (badge)
    badge[i][0] = 1, badge[i][2] = 0;  /* none */
  if (it->stack_count && (it->stack_pick || !it->stack_open))
    {
      /* (closed, the one shown stands for the stack: its pick, or the first
         the view shows if the pick is left out) */
      /* The stack's count on the pick's top right corner, with an arrow:
         right, closed (click to open); left, open. */
      char t[12];
      int tw, bx1 = pic[2] - 8, by1 = pic[3] - 8, bx0, by0 = by1 - 40;
      snprintf (t, sizeof t, "%d", it->stack_count);
      tw = draw_text_width (t);
      bx0 = bx1 - tw - 52;
      draw_rect (bx0, by0, bx1, by1, COL_STACK_BADGE);
      draw_text (bx0 + 10, by0 + 10, t, COL_TEXT_SEL, COL_STACK_BADGE);
      if (it->stack_open)
        draw_triangle (bx1 - 12, by0 + 8, bx1 - 12, by1 - 8, bx1 - 30, (by0 + by1) / 2, COL_ACCENT);
      else
        draw_triangle (bx1 - 30, by0 + 8, bx1 - 30, by1 - 8, bx1 - 12, (by0 + by1) / 2, COL_ACCENT);
      if (badge)
        {
          badge[i][0] = bx0 - ox; badge[i][1] = by0 - oy; badge[i][2] = bx1 - ox; badge[i][3] = by1 - oy;
        }
    }
  else if (it->stack_count && box () >= 224)
    {
      /* An open stack's other photos: a small stack mark. */
      draw_rect (pic[2] - 36, pic[3] - 36, pic[2] - 8, pic[3] - 8, COL_STACK_BADGE);
      draw_frame (pic[2] - 30, pic[3] - 30, pic[2] - 16, pic[3] - 16, COL_TEXT_SEL);
      draw_frame (pic[2] - 26, pic[3] - 26, pic[2] - 12, pic[3] - 12, COL_TEXT_SEL);
    }
  if (it->label > 0 && it->label < CAT_LABELS)
    {
      /* The colour label: a bar under the photo, as wide as it. */
      static const unsigned label_rgb[CAT_LABELS] = { 0, 0x3030D800u, 0x1C8CF000u, 0x20D0F000u, 0x40A83C00u,
                                                      0xD8782800u, 0xB0409000u, 0x90909000u };
      draw_rect (pic[0], pic[1] - 12, pic[2], pic[1] - 4, label_rgb[it->label]);
    }
  if (it->flag)                        /* a flag on the photo's top left corner */
    {
      draw_rect (pic[0] + 8, pic[3] - 40, pic[0] + 36, pic[3] - 8, 0x30303000u);
      draw_rect (pic[0] + 12, pic[3] - 36, pic[0] + 32, pic[3] - 12, COL_FLAG);
    }
  draw_name (it->name, x0 + 12, x1 - 12, by0 - 8 - 28, fg, bg);
  if (it->rating == RATING_REJECT)
    draw_name ("Rejected", x0 + 12, x1 - 12, y0 + 10, selected ? COL_TEXT_SEL : COL_REJECT, bg);
  else
    for (k = 0; k < 5; k++)
      draw_star ((x0 + x1) / 2 + (k - 2) * 34, y0 + 26, 15,
                 k < it->rating ? (selected ? COL_ACCENT_ON_DARK : COL_ACCENT)
                 : (selected ? COL_STAR_OFF_SEL : COL_STAR_OFF));
}

void browser_redraw (int *b)
{
  int more = 0;
  _swix (Wimp_RedrawWindow, _IN (1) | _OUT (0), b, &more);
  while (more)
    {
      int ox = b[1] - b[5], oy = b[4] - b[6];
      int wy0 = b[8] - oy, wy1 = b[10] - oy;
      int r0 = (-wy1 - MARGIN) / cell_h (), r1 = (-wy0 - MARGIN) / cell_h ();
      int r, c;
      draw_rect (b[7], b[8], b[9], b[10], COL_BG);
      if (r0 < 0) r0 = 0;
      for (r = r0; r <= r1; r++)
        for (c = 0; c < cols; c++)
          {
            int i = r * cols + c;
            if (i >= 0 && i < n)
              draw_cell (i, ox, oy);
          }
      if (n == 0)
        {
          char none[120];
          if (src.search[0])
            snprintf (none, sizeof none, "No photos match \"%s\".", src.search);
          else if (src.min_rating > 0)
            snprintf (none, sizeof none, "No photos rated that highly here.");
          else
            snprintf (none, sizeof none, "%s", src.kind == SRC_ALL ? "Drag photos or a folder of photos here to import them."
                                                                : "Nothing here.");
          draw_text (ox + MARGIN * 3, oy - MARGIN * 3 - 32, none, COL_TEXT, COL_BG);
        }
      _swix (Wimp_GetRectangle, _IN (1) | _OUT (0), b, &more);
    }
}

/* --- input */
int browser_item (int i, cat_item *out)
{
  if (i < 0 || i >= n)
    return 0;
  *out = items[i];
  return 1;
}

int browser_focus (void)
{
  return focus;
}

static void set_focus (int i, int extend);

/* Scrolls photo i into view. */
static void scroll_to (int i)
{
  int b[9], x0, y0, x1, y1;
  b[0] = win;
  if (is_open && _swix (Wimp_GetWindowState, _IN (1), b) == NULL)
    {
      int top = b[6], bottom = b[6] - (b[4] - b[2]);
      cell_rect (i, &x0, &y0, &x1, &y1);
      if (y1 > top || y0 < bottom)
        {
          b[6] = y1 > top ? y1 + MARGIN : y0 + (b[4] - b[2]) - MARGIN;
          open_nested (b);
        }
    }
}

void browser_focus_on (int i)
{
  if (i >= 0 && i < n)
    set_focus (i, 0);
  if (is_open)
    _swix (Wimp_SetCaretPosition, _INR (0, 5), mainwin_window (), -1, 0, 0, 1 << 25, -1);
}

static void set_focus (int i, int extend)
{
  int old = focus, k;
  if (i < 0 || i >= n)
    return;
  if (!extend)
    for (k = 0; k < n; k++)
      if (sel[k] && k != i)
        {
          sel[k] = 0;
          redraw_cell (k);
        }
  sel[i] = 1;
  focus = i;
  redraw_cell (old);
  redraw_cell (i);
  if (mainwin_mode () == LAYOUT_SPLIT && viewer_index () != i)
    viewer_show (i);                   /* Split View: the Viewer follows */
  mainwin_info_changed ();
  scroll_to (i);
}

/* After the layout changes (Split View's strip is short): the focused
   photo back in sight. */
void browser_show_focus (void)
{
  if (focus >= 0 && focus < n)
    scroll_to (focus);
}

void browser_click (int *block)
{
  int b[9], i, buttons = block[2];
  b[0] = win;
  if (_swix (Wimp_GetWindowState, _IN (1), b))
    return;
  if (buttons == Button_Menu)
    {
      menus_open_window_menu (block[0], block[1]);
      return;
    }
  _swix (Wimp_SetCaretPosition, _INR (0, 5), mainwin_window (), -1, 0, 0, 1 << 25, -1);
  i = index_at (block[0] - (b[1] - b[5]), block[1] - (b[4] - b[6]));
  if (i >= 0 && badge && (buttons == Button_Select || buttons == Button_Adjust))
    {
      int wx = block[0] - (b[1] - b[5]), wy = block[1] - (b[4] - b[6]);
      if (badge[i][0] < badge[i][2] && wx >= badge[i][0] && wx <= badge[i][2] && wy >= badge[i][1] && wy <= badge[i][3])
        {
          /* The stack's badge: open or close it. */
          cat_stack_open (lib_cat (app_library ()), items[i].id, -1);
          browser_reload ();
          return;
        }
    }
  if (buttons == Button_Select)
    {
      static int last_i = -1;
      static unsigned last_t;
      if (i < 0)
        browser_select_all (0);
      else if (i == last_i && (int) (app_now () - last_t) < 50)
        {
          last_i = -1;                 /* a double click: open it */
          viewer_open (i);
          return;
        }
      else
        set_focus (i, sel[i]);         /* clicking a selected photo keeps the selection */
      last_i = i;
      last_t = app_now ();
    }
  else if (buttons == Button_Adjust && i >= 0)
    browser_toggle_select (i);
}

/* Adjust: photo i in or out of the selection, the focus on it. */
void browser_toggle_select (int i)
{
  int old = focus;
  if (i < 0 || i >= n)
    return;
  sel[i] = !sel[i];
  focus = i;
  redraw_cell (old);                   /* its focus frame goes (found in the ROL 5.31-5 test) */
  redraw_cell (i);
  mainwin_status_changed ();
}

/* Compare: the two selected photos side by side in the Viewer, or the
   focused one with the one after it. */
void browser_compare (void)
{
  int i, a = -1, b = -1, k = 0;
  for (i = 0; i < n; i++)
    if (sel[i])
      {
        if (k == 0) a = i;
        else if (k == 1) b = i;
        k++;
      }
  if (k == 2)
    {
      if (focus == b)                  /* the one clicked last goes on the right */
        viewer_compare_pair (a, b);
      else if (focus == a)
        viewer_compare_pair (b, a);
      else
        viewer_compare_pair (a, b);
      return;
    }
  if (k > 2)
    {
      app_report ("Select two photos to compare them (or one, to compare it with the next).");
      return;
    }
  i = focus >= 0 ? focus : a;
  if (i < 0 || n < 2)
    {
      app_report ("Comparing needs two photos or more in the Browser.");
      return;
    }
  viewer_compare_pair (i, i + 1 < n ? i + 1 : i - 1);
}

/* --- versions and stacks */

/* For the tests: cell i's stack badge in screen coordinates, as last drawn. 1 if it has one. */
int browser_badge (int i, int r[4])
{
  int b[9];
  b[0] = win;
  if (i < 0 || i >= n || !badge || badge[i][0] >= badge[i][2] || _swix (Wimp_GetWindowState, _IN (1), b))
    return 0;
  r[0] = badge[i][0] + b[1] - b[5]; r[2] = badge[i][2] + b[1] - b[5];
  r[1] = badge[i][1] + b[4] - b[6]; r[3] = badge[i][3] + b[4] - b[6];
  return 1;
}

/* The selection's ids, or the focused photo's if none is selected. */
static int chosen (cat_id **ids)
{
  int k = browser_selection (ids);
  if (k == 0 && focus >= 0 && focus < n && (*ids = malloc (sizeof **ids)))
    {
      (*ids)[0] = items[focus].id;
      k = 1;
    }
  return k;
}

/* Selects the photo with this id (alone), if it is listed. Its index, or -1. */
static int select_id (cat_id id)
{
  int i;
  for (i = 0; i < n; i++)
    if (items[i].id == id)
      {
        browser_focus_on (i);
        return i;
      }
  return -1;
}

int browser_new_version (int keep_look)
{
  cat_id *ids, nv = 0;
  int k = chosen (&ids), j;
  cat *c = lib_cat (app_library ());
  for (j = 0; j < k; j++)
    if ((nv = cat_new_version (c, ids[j], keep_look)) < 0)
      {
        app_report (cat_error (c));
        break;
      }
  free (ids);
  browser_reload ();
  return nv > 0 ? select_id (nv) : -1;
}

void browser_stack (void)
{
  cat_id *ids;
  int k = browser_selection (&ids);
  cat *c = lib_cat (app_library ());
  if (k >= 2)
    {
      cat_id pick = focus >= 0 && focus < n && sel[focus] ? items[focus].id : ids[0];
      if (cat_stack (c, ids, k, pick) < 0)
        app_report (cat_error (c));
      else
        cat_stack_open (c, pick, 1);
      browser_reload ();
    }
  else if (k == 1)
    app_report ("Select two photos or more to stack them.");
  free (ids);
}

void browser_unstack (void)
{
  cat_id *ids;
  int k = chosen (&ids);
  cat *c = lib_cat (app_library ());
  /* Their stacks taken apart, every photo in them (in this view or not). */
  if (k && cat_unstack_all (c, ids, k))
    app_report (cat_error (c));
  free (ids);
  browser_reload ();
}

void browser_make_pick (void)
{
  if (focus >= 0 && focus < n && items[focus].stack)
    {
      cat_id id = items[focus].id;
      cat_stack_pick (lib_cat (app_library ()), id);
      browser_reload ();
      select_id (id);
    }
}

void browser_toggle_stack (void)
{
  if (focus >= 0 && focus < n && items[focus].stack)
    {
      cat_id id = items[focus].id, st = items[focus].stack;
      int i;
      cat_stack_open (lib_cat (app_library ()), id, !items[focus].stack_open);
      browser_reload ();
      if (select_id (id) < 0)
        for (i = 0; i < n; i++)
          if (items[i].stack == st)    /* closed: the pick stands for it */
            {
              browser_focus_on (i);
              break;
            }
    }
}

int browser_auto_stack (int seconds)
{
  cat_id *ids;
  int k = browser_selection (&ids), made = 0;
  if (k == 1)
    {
      free (ids);
      app_report ("Select two photos or more to auto-stack, or none for all those shown.");
      return 0;
    }
  if (k < 2)
    {
      /* Nothing selected: all those shown. */
      free (ids);
      ids = malloc (sizeof *ids * (size_t) (n ? n : 1));
      for (k = 0; ids && k < n; k++)
        ids[k] = items[k].id;
    }
  if (ids && k >= 2)
    made = cat_auto_stack (lib_cat (app_library ()), ids, k, seconds);
  free (ids);
  browser_reload ();
  return made;
}

/* After photos leave the library: thumbnails forgotten, the Viewer let
   go if its photo went, the list read again. */
void browser_removed (const cat_id *ids, int n)
{
  int k;
  tagbox_forget (ids, n);              /* its window let go of them */
  for (k = 0; k < n; k++)
    {
      thumbcache_drop (ids[k]);
      if (viewer_item () == ids[k])
        {
          mainwin_set_mode (LAYOUT_BROWSER);
          viewer_shutdown ();
        }
    }
  free (sel);
  sel = NULL;
  free (items);
  items = NULL;
  n_reset ();
  browser_reload ();
  mainwin_library_changed ();
  mainwin_info_changed ();
}

void browser_remove_selection (void)
{
  cat_id *ids;
  int k = browser_selection (&ids);
  char q[200];
  if (k)
    {
      if (k == 1)
        snprintf (q, sizeof q, "Remove this photo from the library? Its file isn't touched, and its "
                  "adjustments, rating and albums go with it.");
      else
        snprintf (q, sizeof q, "Remove these %d photos from the library? Their files aren't touched, and "
                  "their adjustments, ratings and albums go with them.", k);
      if (app_confirm (q, "Remove"))
        {
          if (lib_remove (app_library (), ids, k))
            app_report (cat_error (lib_cat (app_library ())));
          else
            browser_removed (ids, k);
        }
    }
  free (ids);
}

static void rate (int rating)
{
  cat_id *ids;
  int k = browser_selection (&ids);
  if (k)
    {
      cat_set_rating (lib_cat (app_library ()), ids, k, rating);
      browser_after_change ();
    }
  free (ids);
}

static void n_reset (void) { n = 0; focus = -1; }

int browser_key (int key)
{
  int step = cols;
  if (key >= '0' && key <= '5')
    rate (key - '0');
  else if (key == '9')
    rate (RATING_REJECT);
  else if (key == '/')
    {
      cat_id *ids;
      int k = browser_selection (&ids);
      if (k)
        {
          cat_set_flag (lib_cat (app_library ()), ids, k, -1);
          browser_after_change ();
        }
      free (ids);
    }
  else if (key == KEY_LEFT || key == KEY_RIGHT || key == KEY_UP || key == KEY_DOWN
           || key == KEY_HOME || key == KEY_END || key == KEY_PAGEUP || key == KEY_PAGEDOWN)
    {
      int f = focus < 0 ? 0 : focus;
      int page = cols * (vis_h / cell_h () > 1 ? vis_h / cell_h () : 1);
      if (n == 0)
        return 1;
      if (focus >= 0)
        switch (key)
          {
          case KEY_LEFT: f--; break;
          case KEY_RIGHT: f++; break;
          case KEY_UP: f -= step; break;
          case KEY_DOWN: f += step; break;
          case KEY_PAGEUP: f -= page; break;
          case KEY_PAGEDOWN: f += page; break;
          case KEY_HOME: f = 0; break;
          default: f = n - 1; break;
          }
      if (f < 0) f = 0;
      if (f >= n) f = n - 1;
      set_focus (f, 0);
    }
  else if (key == KEY_RETURN)
    {
      if (n)
        viewer_open (focus >= 0 ? focus : 0);
    }
  else if (key == KEY_CTRL_A)
    browser_select_all (1);
  else if (key == 0x05)                /* Ctrl-E: export */
    {
      int ptr[5];
      if (_swix (Wimp_GetPointerInfo, _IN (1), ptr) == NULL)
        exportbox_open (0, ptr[0], ptr[1]);
    }
  else if (key == 'o' || key == 'O')   /* compare */
    browser_compare ();
  else if (key == 0x04)                /* Ctrl-D: a duplicate version */
    browser_new_version (1);
  else if (key == 0x07)                /* Ctrl-G: stack the selection */
    browser_stack ();
  else if (key == 0x15)                /* Ctrl-U: unstack */
    browser_unstack ();
  else if (key == '\\')               /* \: the stack's pick */
    browser_make_pick ();
  else if (key == 's' || key == 'S')   /* open or close the stack */
    browser_toggle_stack ();
  else if (key == 0x0B)                /* Ctrl-K: caption and keywords */
    {
      int ptr[5];
      if (_swix (Wimp_GetPointerInfo, _IN (1), ptr) == NULL)
        tagbox_open (0, ptr[0], ptr[1]);
    }
  else if (key == 0x10)                /* Ctrl-P: print */
    {
      int ptr[5];
      if (_swix (Wimp_GetPointerInfo, _IN (1), ptr) == NULL)
        printbox_open (0, ptr[0], ptr[1]);
    }
  else if (key == 0x03)                /* Ctrl-C: copy the adjustments */
    looks_copy_selection ();
  else if (key == 0x16)                /* Ctrl-V: paste them */
    {
      if (looks_clip ())
        looks_apply_selection (looks_clip ());
    }
  else if (key == KEY_ESCAPE)
    browser_select_all (0);
  else if (key == 0x7F)                /* Delete: out of the library (after asking) */
    browser_remove_selection ();
  else
    return 0;
  return 1;
}

void browser_thumb_ready (cat_id v)
{
  int i;
  thumbcache_drop (v);
  for (i = 0; i < n; i++)
    if (items[i].id == v)
      {
        items[i].thumb_state = THUMB_READY;
        redraw_cell (i);
        break;
      }
}

void browser_thumb_loaded (cat_id v)
{
  int i;
  for (i = 0; i < n; i++)
    if (items[i].id == v)
      {
        redraw_cell (i);
        break;
      }
}
