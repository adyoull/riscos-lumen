/* menus.c: Lumen's menus, built in code (Wimp_CreateMenu block: a 28-byte
   header, then 24-byte items: menu flags, submenu, icon flags, icon data).
   Every menu is rebuilt into the same static pool each time it opens, so
   an Adjust click can reopen the same tree at the same addresses with
   fresh ticks and shading. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <kernel.h>
#include <swis.h>
#include "ui.h"

#define MF_TICK 0x01
#define MF_DOTTED 0x02
#define MF_WRITABLE 0x04
#define MF_LAST 0x80
#define MF_TITLE_INDIRECTED 0x100
#define IF_SHADED 0x00400000
#define ITEM_ICON 0x07000121u          /* text, filled, indirected; black on white */
#define WRITABLE_ICON 0x0700F12Du      /* text, border, filled, indirected, writable */
#define MAX_LIST 48                    /* projects or albums shown in a submenu */

static char pool[24 * 1024];
static size_t used;
static int which;                      /* 1 window menu, 2 icon bar menu */
static int open_x, open_y;
static int info_window = -1;
static char album_name[40];

/* The lists the open menu was built from, to decode selections. */
static cat_group *projects, *albums;
static int nprojects, nalbums;

typedef struct { int flags, submenu, icon_flags, data[3]; } item;
typedef struct { char title[12]; unsigned char colours[4]; int width, height, gap; item it[1]; } menu;

static void *alloc (size_t n)
{
  void *p;
  n = (n + 3) & ~(size_t) 3;
  if (used + n > sizeof pool)
    return NULL;
  p = pool + used;
  used += n;
  return p;
}

static char *str (const char *s)
{
  char *p = alloc (strlen (s) + 1);
  if (p)
    strcpy (p, s);
  return p;
}

static menu *new_menu (const char *title, int nitems)
{
  menu *m = alloc (28 + 24 * (size_t) (nitems > 0 ? nitems : 1));
  if (!m)
    return NULL;
  memset (m, 0, 28);
  memcpy (m->title, title, strlen (title) < sizeof m->title ? strlen (title) + 1 : sizeof m->title);
  m->colours[0] = 7; m->colours[1] = 2; m->colours[2] = 7; m->colours[3] = 0;
  m->width = draw_text_width (title) + 64;
  m->height = 44;
  m->gap = 0;
  return m;
}

static void set_item (menu *m, int i, int last, const char *text, int flags, int submenu, int shaded)
{
  item *it = &m->it[i];
  char *t = str (text);
  int w = draw_text_width (text) + 64;
  it->flags = flags | (last ? MF_LAST : 0);
  it->submenu = submenu;
  it->icon_flags = (int) (ITEM_ICON | (shaded ? IF_SHADED : 0));
  it->data[0] = (int) t;
  it->data[1] = -1;
  it->data[2] = t ? (int) strlen (t) + 1 : 1;
  if (w > m->width)
    m->width = w;
}

static menu *group_menu (const char *title, const cat_group *g, int ng, int kind, const char *none)
{
  int k, count = ng > MAX_LIST ? MAX_LIST : ng;
  const cat_source *s = browser_source ();
  menu *m = new_menu (title, count ? count : 1);
  if (!m)
    return NULL;
  if (!count)
    set_item (m, 0, 1, none, 0, -1, 1);
  for (k = 0; k < count; k++)
    {
      char t[80];
      snprintf (t, sizeof t, "%s (%d)", g[k].name, g[k].count);
      set_item (m, k, k == count - 1, t, (s->kind == kind && s->id == g[k].id) ? MF_TICK : 0, -1, 0);
    }
  return m;
}

static void load_groups (void)
{
  free (projects);
  free (albums);
  projects = albums = NULL;
  nprojects = nalbums = 0;
  cat_groups (lib_cat (app_library ()), SRC_PROJECT, &projects, &nprojects);
  cat_groups (lib_cat (app_library ()), SRC_ALBUM, &albums, &nalbums);
}

/* Lumen > Show, Selection, Album, Size, Quit */
static menu *build_window_menu (void)
{
  const cat_source *s = browser_source ();
  cat_id *ids;
  int nsel = browser_selection (&ids), k;
  menu *m, *show, *selm, *rating, *album, *newalbum, *addto, *sizem, *filter, *proj, *alb;
  static const char *const rating_names[6] = { "No stars", "1 star", "2 stars", "3 stars", "4 stars", "5 stars" };
  static const char *const filter_names[6] = { "Any rating", "1 star and up", "2 stars and up", "3 stars and up", "4 stars and up", "5 stars" };
  free (ids);
  used = 0;
  load_groups ();

  proj = group_menu ("Projects", projects, nprojects, SRC_PROJECT, "No projects yet");
  alb = group_menu ("Albums", albums, nalbums, SRC_ALBUM, "No albums yet");
  filter = new_menu ("Rating", 6);
  for (k = 0; k < 6 && filter; k++)
    set_item (filter, k, k == 5, filter_names[k], s->min_rating == k ? MF_TICK : 0, -1, s->kind == SRC_REJECTED);
  show = new_menu ("Show", 7);
  if (show)
    {
      set_item (show, 0, 0, "All photos", s->kind == SRC_ALL ? MF_TICK : 0, -1, 0);
      set_item (show, 1, 0, "Flagged", s->kind == SRC_FLAGGED ? MF_TICK : 0, -1, 0);
      set_item (show, 2, 0, "Rejected", s->kind == SRC_REJECTED ? MF_TICK : 0, -1, 0);
      set_item (show, 3, 0, "Last import", (s->kind == SRC_LAST_IMPORT ? MF_TICK : 0) | MF_DOTTED, -1, 0);
      set_item (show, 4, 0, "Projects", s->kind == SRC_PROJECT ? MF_TICK : 0, (int) proj, 0);
      set_item (show, 5, 0, "Albums", (s->kind == SRC_ALBUM ? MF_TICK : 0) | MF_DOTTED, (int) alb, 0);
      set_item (show, 6, 1, "Rating filter", 0, (int) filter, 0);
    }

  rating = new_menu ("Rating", 6);
  for (k = 0; k < 6 && rating; k++)
    set_item (rating, k, k == 5, rating_names[k], 0, -1, nsel == 0);
  selm = new_menu ("Selection", 5);
  if (selm)
    {
      set_item (selm, 0, 0, "Rating", 0, (int) rating, nsel == 0);
      set_item (selm, 1, 0, "Flag / unflag      /", 0, -1, nsel == 0);
      set_item (selm, 2, 0, "Reject      9", MF_DOTTED, -1, nsel == 0);
      set_item (selm, 3, 0, "Select all      ^A", 0, -1, browser_count () == 0);
      set_item (selm, 4, 1, "Clear selection      Esc", 0, -1, nsel == 0);
    }

  newalbum = new_menu ("Name", 1);
  if (newalbum)
    {
      item *it = &newalbum->it[0];
      it->flags = MF_WRITABLE | MF_LAST;
      it->submenu = -1;
      it->icon_flags = (int) WRITABLE_ICON;
      it->data[0] = (int) album_name;
      it->data[1] = -1;
      it->data[2] = sizeof album_name;
      newalbum->width = 320;
    }
  addto = group_menu ("Add to", albums, nalbums, -1, "No albums yet");
  album = new_menu ("Album", 3);
  if (album)
    {
      set_item (album, 0, 0, "New album from selection", 0, (int) newalbum, nsel == 0);
      set_item (album, 1, 0, "Add selection to", 0, (int) addto, nsel == 0 || nalbums == 0);
      set_item (album, 2, 1, "Remove from this album", 0, -1, nsel == 0 || s->kind != SRC_ALBUM);
    }

  sizem = new_menu ("Size", 3);
  if (sizem)
    {
      set_item (sizem, 0, 0, "Small", browser_size () == 0 ? MF_TICK : 0, -1, 0);
      set_item (sizem, 1, 0, "Medium", browser_size () == 1 ? MF_TICK : 0, -1, 0);
      set_item (sizem, 2, 1, "Large", browser_size () == 2 ? MF_TICK : 0, -1, 0);
    }

  m = new_menu (APP_NAME, 5);
  if (m)
    {
      set_item (m, 0, 0, "Show", 0, (int) show, 0);
      set_item (m, 1, 0, "Selection", 0, (int) selm, 0);
      set_item (m, 2, 0, "Album", 0, (int) album, 0);
      set_item (m, 3, 0, "Thumbnail size", MF_DOTTED, (int) sizem, 0);
      set_item (m, 4, 1, "Quit", 0, -1, 0);
    }
  return m;
}

static menu *build_iconbar_menu (void)
{
  menu *m;
  used = 0;
  m = new_menu (APP_NAME, 3);
  if (m)
    {
      set_item (m, 0, 0, "Info", 0, info_window, info_window == -1);
      set_item (m, 1, 0, "Open library", MF_DOTTED, -1, 0);
      set_item (m, 2, 1, "Quit", 0, -1, 0);
    }
  return m;
}

/* The Info box: four labels and their values (indirected text icons). */
static int create_info_window (void)
{
  static const char *const labels[4] = { "Name", "Purpose", "Author", "Version" };
  static char values[4][48];
  int b[23 + 8 * 32 / 4], k, *icon;
  snprintf (values[0], sizeof values[0], "%s", APP_NAME);
  snprintf (values[1], sizeof values[1], "Photo library and editor");
  snprintf (values[2], sizeof values[2], "\xA9 Andrew Youll, 2026");
  snprintf (values[3], sizeof values[3], "%s (%s)", LUMEN_VERSION, LUMEN_DATE);
  memset (b, 0, sizeof b);
  b[0] = 0; b[1] = 0; b[2] = 600; b[3] = 220;
  b[6] = -1;
  b[7] = (int) 0x84000012;            /* moveable, title, new format */
  ((unsigned char *) b)[32] = 7;  ((unsigned char *) b)[33] = 2;
  ((unsigned char *) b)[34] = 7;  ((unsigned char *) b)[35] = 1;
  ((unsigned char *) b)[36] = 3;  ((unsigned char *) b)[37] = 1;
  ((unsigned char *) b)[38] = 12;
  b[10] = 0; b[11] = -220; b[12] = 600; b[13] = 0;
  b[14] = 0x3D;
  b[16] = 1;
  memcpy (&b[18], "About this", 11);
  b[21] = 8;
  for (k = 0; k < 4; k++)
    {
      int y1 = -16 - k * 50, y0 = y1 - 44;
      icon = &b[22 + 8 * (2 * k)];
      icon[0] = 16; icon[1] = y0; icon[2] = 160; icon[3] = y1;
      icon[4] = 0x07000311;            /* text, vertically centred, indirected, right justified */
      icon[5] = (int) labels[k]; icon[6] = -1; icon[7] = (int) strlen (labels[k]) + 1;
      icon = &b[22 + 8 * (2 * k + 1)];
      icon[0] = 176; icon[1] = y0; icon[2] = 584; icon[3] = y1;
      icon[4] = 0x0700013D;            /* text, border, centred, filled, indirected */
      icon[5] = (int) values[k]; icon[6] = -1; icon[7] = (int) sizeof values[k];
    }
  return _swix (Wimp_CreateWindow, _IN (1) | _OUT (0), b, &info_window) ? -1 : 0;
}

void menus_init (void)
{
  if (create_info_window ())
    info_window = -1;
}

void menus_open_window_menu (int x, int y)
{
  menu *m = build_window_menu ();
  which = 1;
  open_x = x - 64;
  open_y = y;
  if (m)
    _swix (Wimp_CreateMenu, _INR (1, 3), m, open_x, open_y);
}

void menus_open_iconbar_menu (int x)
{
  menu *m = build_iconbar_menu ();
  which = 2;
  open_x = x - 64;
  open_y = 96 + 3 * 44;
  if (m)
    _swix (Wimp_CreateMenu, _INR (1, 3), m, open_x, open_y);
}

static void with_selection (int action, int arg)
{
  cat_id *ids;
  int k = browser_selection (&ids);
  cat *c = lib_cat (app_library ());
  if (k)
    {
      switch (action)
        {
        case 0: cat_set_rating (c, ids, k, arg); break;
        case 1: cat_set_flag (c, ids, k, -1); break;
        case 2: cat_album_add (c, (cat_id) arg, ids, k); break;
        case 3: cat_album_remove (c, browser_source ()->id, ids, k); break;
        case 4:
          {
            cat_id a;
            if (!album_name[0])
              break;
            a = cat_album_create (c, album_name);
            if (a > 0)
              cat_album_add (c, a, ids, k);
            album_name[0] = '\0';
            break;
          }
        }
      browser_after_change ();
    }
  free (ids);
}

/* Returns 1 to quit. */
static int window_choice (const int *c)
{
  cat_source s = *browser_source ();
  switch (c[0])
    {
    case 0:                            /* Show */
      if (c[1] >= 0 && c[1] <= 3)
        {
          static const int kinds[4] = { SRC_ALL, SRC_FLAGGED, SRC_REJECTED, SRC_LAST_IMPORT };
          s.kind = kinds[c[1]];
          s.id = 0;
          browser_set_source (&s);
        }
      else if (c[1] == 4 && c[2] >= 0 && c[2] < nprojects)
        {
          s.kind = SRC_PROJECT;
          s.id = projects[c[2]].id;
          browser_set_source (&s);
        }
      else if (c[1] == 5 && c[2] >= 0 && c[2] < nalbums)
        {
          s.kind = SRC_ALBUM;
          s.id = albums[c[2]].id;
          browser_set_source (&s);
        }
      else if (c[1] == 6 && c[2] >= 0 && c[2] <= 5)
        {
          s.min_rating = c[2];
          browser_set_source (&s);
        }
      break;
    case 1:                            /* Selection */
      if (c[1] == 0 && c[2] >= 0 && c[2] <= 5)
        with_selection (0, c[2]);
      else if (c[1] == 1)
        with_selection (1, 0);
      else if (c[1] == 2)
        with_selection (0, RATING_REJECT);
      else if (c[1] == 3)
        browser_select_all (1);
      else if (c[1] == 4)
        browser_select_all (0);
      break;
    case 2:                            /* Album */
      if (c[1] == 0 && c[2] == 0)
        with_selection (4, 0);
      else if (c[1] == 1 && c[2] >= 0 && c[2] < nalbums)
        with_selection (2, (int) albums[c[2]].id);
      else if (c[1] == 2)
        with_selection (3, 0);
      break;
    case 3:                            /* Size */
      if (c[1] >= 0 && c[1] <= 2)
        browser_set_size (c[1]);
      break;
    case 4:
      return 1;
    }
  return 0;
}

/* Returns 1 to quit. */
int menus_selection (const int *choice)
{
  int ptr[5] = { 0 }, quit_requested = 0;
  if (which == 1)
    quit_requested = window_choice (choice);
  else if (which == 2)
    {
      if (choice[0] == 1)
        browser_open (NULL);
      else if (choice[0] == 2)
        quit_requested = 1;
    }
  if (quit_requested)
    return 1;
  /* Adjust keeps the menu open, rebuilt with fresh ticks. */
  if (_swix (Wimp_GetPointerInfo, _IN (1), ptr) == NULL && (ptr[2] & Button_Adjust))
    {
      menu *m = which == 1 ? build_window_menu () : build_iconbar_menu ();
      if (m)
        _swix (Wimp_CreateMenu, _INR (1, 3), m, open_x, open_y);
    }
  return 0;
}
