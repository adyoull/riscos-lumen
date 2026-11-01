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

#define COL_SELECTED 0x2F2F2F00u       /* &BBGGRR00 */
#define COL_PLACEHOLDER 0xC4C4C400u
#define COL_FAILED 0xA8A8C800u
#define COL_TEXT 0x1A1A1A00u
#define COL_TEXT_SEL 0xFFFFFF00u
#define COL_BG 0xDDDDDD00u             /* close to Wimp colour 1 */
#define COL_ACCENT 0x166AC000u         /* #C06A16 */
#define COL_STAR_OFF 0x9A9A9A00u
#define COL_STAR_OFF_SEL 0x70707000u
#define COL_FLAG 0xB0E6F200u           /* #F2E6B0 */
#define COL_REJECT 0x2020B000u

static const int box_sizes[3] = { 160, 224, 320 };

static int win = -1;
static char title[200];
static cat_source src = { SRC_ALL, 0, 0 };
static cat_item *items;
static unsigned char *sel;
static int n, cols = 1, size = 1, focus = -1, is_open;
static int layout_w = 256;            /* a cell's width: the window's width shared out */
static int vis_w = 1200, vis_h = 800;
static int screen_w = 1920, screen_h = 1080;

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
  /* moveable; back, close, title, toggle size, vertical scroll, adjust size; new format */
  b[7] = (int) 0xBF000002;
  ((unsigned char *) b)[32] = 7;  ((unsigned char *) b)[33] = 2;   /* title fg, bg */
  ((unsigned char *) b)[34] = 7;  ((unsigned char *) b)[35] = 1;   /* work area fg, bg */
  ((unsigned char *) b)[36] = 3;  ((unsigned char *) b)[37] = 1;   /* scroll bar outer, inner */
  ((unsigned char *) b)[38] = 12;                                  /* title bg with input focus */
  b[10] = 0; b[11] = -screen_h; b[12] = screen_w; b[13] = 0;      /* work area extent */
  b[14] = 0x13D;              /* title: text, border, centred, filled, indirected */
  b[15] = 3 << 12;            /* work area: click */
  b[16] = 1;                  /* Wimp sprite area */
  b[17] = (320 << 16) | 400;  /* minimum height, width */
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
void browser_update_title (void)
{
  char name[80], status[100], t[200];
  lib_progress p;
  switch (src.kind)
    {
    case SRC_FLAGGED: snprintf (name, sizeof name, "Flagged"); break;
    case SRC_REJECTED: snprintf (name, sizeof name, "Rejected"); break;
    case SRC_LAST_IMPORT: snprintf (name, sizeof name, "Last import"); break;
    case SRC_PROJECT:
    case SRC_ALBUM: cat_group_name (lib_cat (app_library ()), src.kind, src.id, name, sizeof name); break;
    default: snprintf (name, sizeof name, "All photos"); break;
    }
  lib_get_progress (app_library (), &p);
  status[0] = '\0';
  if (p.importing)
    snprintf (status, sizeof status, "  \xB7  Importing: %d found", p.found);
  else if (p.thumbs_pending > 0)
    snprintf (status, sizeof status, "  \xB7  Making thumbnails: %d to go", p.thumbs_pending);
  snprintf (t, sizeof t, "%s - %s (%d)%s%s", APP_NAME, name, n,
            src.min_rating > 0 ? (src.min_rating == 5 ? "  \xB7  5 stars" : "  \xB7  rated") : "", status);
  if (strcmp (t, title) != 0)
    {
      snprintf (title, sizeof title, "%s", t);
      if (win != -1)
        _swix (Wimp_ForceRedraw, _INR (0, 2), win, TASK_WORD, 3);
    }
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
  browser_update_title ();
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
      _swix (Wimp_OpenWindow, _IN (1), b);
    }
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
      _swix (Wimp_OpenWindow, _IN (1), b);
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
}

/* --- opening */
void browser_open (int *block)
{
  int b[9];
  if (!block)
    {
      b[0] = win;
      if (is_open && _swix (Wimp_GetWindowState, _IN (1), b) == NULL)
        b[7] = -1;
      else
        {
          int w = screen_w - 192 < 2400 ? screen_w - 192 : 2400;
          int h = screen_h - 320;
          b[1] = (screen_w - w) / 2; b[2] = 160;
          b[3] = b[1] + w; b[4] = 160 + h;
          b[5] = 0; b[6] = 0; b[7] = -1;
        }
      block = b;
    }
  vis_w = block[3] - block[1];
  vis_h = block[4] - block[2];
  if (relayout (vis_w))
    {
      set_extent ();
      redraw_all ();
    }
  _swix (Wimp_OpenWindow, _IN (1), block);
  if (!is_open)
    {
      is_open = 1;
      /* Input focus, so the rating keys work at once. */
      _swix (Wimp_SetCaretPosition, _INR (0, 5), win, -1, 0, 0, 1 << 25, -1);
    }
}

void browser_close (void)
{
  int b[1];
  b[0] = win;
  _swix (Wimp_CloseWindow, _IN (1), b);
  is_open = 0;
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
      draw_text (pic[2] - tw - 16, pic[1] + 18, "Edited", COL_TEXT, 0xFFFFFF00u);
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
                 k < it->rating ? COL_ACCENT : (selected ? COL_STAR_OFF_SEL : COL_STAR_OFF));
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
      if (r0 < 0) r0 = 0;
      for (r = r0; r <= r1; r++)
        for (c = 0; c < cols; c++)
          {
            int i = r * cols + c;
            if (i >= 0 && i < n)
              draw_cell (i, ox, oy);
          }
      if (n == 0)
        draw_text (ox + MARGIN * 3, oy - MARGIN * 3 - 32,
                   src.kind == SRC_ALL ? "Drag photos or a folder of photos here to import them."
                                       : "Nothing here.",
                   COL_TEXT, COL_BG);
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

void browser_focus_on (int i)
{
  if (i >= 0 && i < n)
    set_focus (i, 0);
  if (is_open)
    _swix (Wimp_SetCaretPosition, _INR (0, 5), win, -1, 0, 0, 1 << 25, -1);
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
  /* Scroll it into view. */
  {
    int b[9], x0, y0, x1, y1;
    b[0] = win;
    if (_swix (Wimp_GetWindowState, _IN (1), b) == NULL)
      {
        int top = b[6], bottom = b[6] - (b[4] - b[2]);
        cell_rect (i, &x0, &y0, &x1, &y1);
        if (y1 > top || y0 < bottom)
          {
            b[6] = y1 > top ? y1 + MARGIN : y0 + (b[4] - b[2]) - MARGIN;
            _swix (Wimp_OpenWindow, _IN (1), b);
          }
      }
  }
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
  _swix (Wimp_SetCaretPosition, _INR (0, 5), win, -1, 0, 0, 1 << 25, -1);
  i = index_at (block[0] - (b[1] - b[5]), block[1] - (b[4] - b[6]));
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
    {
      int old = focus;
      sel[i] = !sel[i];
      focus = i;
      redraw_cell (old);               /* its focus frame goes (found in the ROL 5.31-5 test) */
      redraw_cell (i);
    }
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
  else if (key == 0x03)                /* Ctrl-C: copy the adjustments */
    looks_copy_selection ();
  else if (key == 0x16)                /* Ctrl-V: paste them */
    {
      if (looks_clip ())
        looks_apply_selection (looks_clip ());
    }
  else if (key == KEY_ESCAPE)
    browser_select_all (0);
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
