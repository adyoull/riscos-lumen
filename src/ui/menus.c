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
#include "styles.h"

#define MF_TICK 0x01
#define MF_DOTTED 0x02
#define MF_WRITABLE 0x04
#define MF_WARN 0x08                   /* Message_MenuWarning before its submenu opens (classic PRM; the 2026 one omits the flags) */
#define MF_LAST 0x80
#define MF_TITLE_INDIRECTED 0x100
#define IF_SHADED 0x00400000
#define ITEM_ICON 0x07000121u          /* text, filled, indirected; black on white */
#define WRITABLE_ICON 0x0700F12Du      /* text, border, filled, indirected, writable */
#define MAX_LIST 48                    /* projects or albums shown in a submenu */

static char pool[32 * 1024];
static size_t used;
static int which;                      /* 1 window menu, 2 icon bar menu, 3 viewer menu, 4 presets, 5 New, 6 rating filter */
static int open_x, open_y;
static int info_window = -1;
static char album_name[40], preset_name[40], stack_secs[6] = "2";
static int export_after_menu, print_after_menu, compare_after_menu;
static int libexport_after_menu;       /* Export library box: 1 the whole library, 2 the selection, 3 the group menu's */
static int tags_after_menu;            /* 1: the selection's caption and keywords, 2: the Viewer's photo */
static int smart_after_menu;           /* open the Smart album window: 1 a new one, 2 the group menu's album */
static cat_keyword *keywords;          /* the keyword menu's list */
static int nkeywords;
static int remove_after_menu;          /* ask about removing once the menu has gone */
static int plugin_remove_after_menu;   /* 1 + the plug-in to ask about removing */
static int plugwin_after_menu;         /* open the Add plug-in window once the menu has gone */
static int group_kind;                 /* the Library tab's project or album the menu is for */
static cat_id group_id;
static char group_name[64];          /* 1 the selection, 2 the viewer's photo: open the box once the menu is done */

/* The presets the open menu was built from. */
static cat_preset *presets;
static int npresets;

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

static menu *writable_menu (const char *title, char *buf, int size)
{
  menu *m = new_menu (title, 1);
  if (m)
    {
      item *it = &m->it[0];
      it->flags = MF_WRITABLE | MF_LAST;
      it->submenu = -1;
      it->icon_flags = (int) WRITABLE_ICON;
      it->data[0] = (int) buf;
      it->data[1] = -1;
      it->data[2] = size;
      m->width = 320;
    }
  return m;
}

/* The presets' names; none: a shaded line. */
static menu *preset_list (const char *title, int shaded)
{
  int k, count = npresets > MAX_LIST ? MAX_LIST : npresets;
  menu *m = new_menu (title, count ? count : 1);
  if (!m)
    return NULL;
  if (!count)
    set_item (m, 0, 1, "No presets yet", 0, -1, 1);
  for (k = 0; k < count; k++)
    set_item (m, k, k == count - 1, presets[k].name, 0, -1, shaded);
  return m;
}

/* Lumen's built-in styles (styles.c), shaded if there is nothing to apply them to. */
static menu *styles_menu (int shaded)
{
  int k, n = styles_count ();
  menu *m = new_menu ("Styles", n);
  for (k = 0; m && k < n; k++)
    set_item (m, k, k == n - 1, styles_get (k)->name,
              k == 4 || k == 8 ? MF_DOTTED : 0, -1, shaded);   /* colour; strong colour; black and white */
  return m;
}

/* Presets: Styles >, the user's presets, then Save current as > and Delete >. */
static menu *build_presets (void)
{
  int k, count;
  menu *m, *save, *del, *sty;
  npresets = looks_presets (&presets);
  count = npresets > MAX_LIST ? MAX_LIST : npresets;
  sty = styles_menu (0);
  save = writable_menu ("Name", preset_name, sizeof preset_name);
  del = preset_list ("Delete", 0);
  m = new_menu ("Presets", 1 + (count ? count : 1) + 2);
  if (!m)
    return NULL;
  set_item (m, 0, 0, "Styles", MF_DOTTED | MF_WARN, (int) sty, 0);
  if (!count)
    set_item (m, 1, 0, "No presets yet", MF_DOTTED, -1, 1);
  for (k = 0; k < count; k++)
    set_item (m, 1 + k, 0, presets[k].name, k == count - 1 ? MF_DOTTED : 0, -1, 0);
  k = 1 + (count ? count : 1);
  set_item (m, k, 0, "Save current as", 0, (int) save, 0);
  set_item (m, k + 1, 1, "Delete", 0, (int) del, count == 0);
  return m;
}

/* Presets menu choice c (from its own level). */
static void presets_choice (const int *c)
{
  int slot = npresets > MAX_LIST ? MAX_LIST : npresets;
  int first = 1 + (slot ? slot : 1);
  if (c[0] == 0)
    {
      if (styles_get (c[1]))
        viewer_apply_look (styles_get (c[1])->recipe);
    }
  else if (slot && c[0] >= 1 && c[0] <= slot)
    viewer_apply_look (presets[c[0] - 1].recipe);
  else if (c[0] == first && c[1] == 0)
    {
      if (looks_save_preset (preset_name) == 0)
        preset_name[0] = '\0';
    }
  else if (c[0] == first + 1 && c[1] >= 0 && c[1] < slot)
    looks_delete_preset (presets[c[1]].id);
}

/* Selection > Apply preset: Styles >, then the user's presets. */
static menu *apply_menu (int shaded)
{
  int k, count = npresets > MAX_LIST ? MAX_LIST : npresets;
  menu *sty = styles_menu (shaded), *m = new_menu ("Apply preset", 1 + (count ? count : 1));
  if (!m)
    return NULL;
  set_item (m, 0, 0, "Styles", MF_DOTTED, (int) sty, shaded);
  if (!count)
    set_item (m, 1, 1, "No presets yet", 0, -1, 1);
  for (k = 0; k < count; k++)
    set_item (m, 1 + k, k == count - 1, presets[k].name, 0, -1, shaded);
  return m;
}

/* Interactive help for a Presets menu (or Apply preset), from its own level. */
static const char *presets_help (const int *c, int apply)
{
  if (c[0] == 0)
    {
      const style *st = styles_get (c[1]);
      if (st)
        {
          static char t[200];
          snprintf (t, sizeof t, "%s. Replaces the photo's look; its crop and straightening stay.", st->about);
          return t;
        }
      return "Lumen's own styles: film, toned prints and black and white looks, to start from.";
    }
  if (apply || c[0] <= (npresets > MAX_LIST ? MAX_LIST : npresets))
    return "A look you saved: it replaces the photo's look; its crop and straightening stay.";
  return "Save the photo's look as a preset, or delete one.";
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

/* Selection > ... (the order menus_selection decodes) */
enum { SEL_RATING, SEL_LABEL, SEL_FLAG, SEL_REJECT, SEL_TAGS, SEL_ALL, SEL_CLEAR, SEL_COPY, SEL_PASTE, SEL_PRESET,
       SEL_COMPARE, SEL_EXPORT, SEL_EXPORT_LIB, SEL_PRINT, SEL_REMOVE, SEL_COUNT };

/* None, Red ... Grey, each with its coloured dot (the lumlab sprites in
   !Sprites, beside the text as on a radio button); ticked: the label now
   (-1: none ticked). */
static menu *label_menu (const char *title, int now)
{
  int k;
  menu *m = new_menu (title, CAT_LABELS);
  for (k = 0; m && k < CAT_LABELS; k++)
    {
      char v[12];
      item *it;
      set_item (m, k, k == CAT_LABELS - 1, k ? cat_label_names[k] : "None",
                (k == now ? MF_TICK : 0) | (k == 0 ? MF_DOTTED : 0), -1, 0);
      it = &m->it[k];
      snprintf (v, sizeof v, "Slumlab%d", k);
      it->icon_flags |= 0x12;          /* a sprite as well as text, centred vertically */
      it->data[1] = (int) str (v);
    }
  if (m)
    m->width += 40;                    /* room for the dot */
  return m;
}

/* Lumen > Show, Selection, Album, Size, Quit */
/* Plug-ins: each one, ticked if it is on (choosing it switches it);
   Add plug-in...; Remove > each one. */
static menu *build_plugins_menu (void)
{
  const plugin_info *pl;
  int n = app_plugins (&pl), k, base;
  menu *m, *rem;
  char label[100];
  if (n > MAX_LIST)
    n = MAX_LIST;
  base = n ? n : 1;
  rem = new_menu ("Remove", base);
  if (rem && !n)
    set_item (rem, 0, 1, "No plug-ins", 0, -1, 1);
  for (k = 0; k < n && rem; k++)
    {
      snprintf (label, sizeof label, "%s...", pl[k].name);
      set_item (rem, k, k == n - 1, label, 0, -1, 0);
    }
  m = new_menu ("Plug-ins", base + 2);
  if (!m)
    return NULL;
  if (!n)
    set_item (m, 0, 0, "None installed", MF_DOTTED, -1, 1);
  for (k = 0; k < n; k++)
    {
      snprintf (label, sizeof label, "%s%s%s", pl[k].name, pl[k].version[0] ? " " : "", pl[k].version);
      set_item (m, k, 0, label, (pl[k].enabled ? MF_TICK : 0) | (k == n - 1 ? MF_DOTTED : 0), -1, !pl[k].program[0]);
    }
  set_item (m, base, 0, "Add plug-in...", 0, -1, 0);
  set_item (m, base + 1, 1, "Remove", 0, (int) rem, n == 0);
  return m;
}

static void plugins_choice (const int *c)
{
  const plugin_info *pl;
  int n = app_plugins (&pl), base;
  if (n > MAX_LIST)
    n = MAX_LIST;
  base = n ? n : 1;
  if (c[0] >= 0 && c[0] < n)
    app_plugin_toggle (c[0]);
  else if (c[0] == base)
    plugwin_after_menu = 1;
  else if (c[0] == base + 1 && c[1] >= 0 && c[1] < n)
    plugin_remove_after_menu = 1 + c[1];
}

static menu *build_window_menu (void)
{
  const cat_source *s = browser_source ();
  cat_id *ids;
  int nsel = browser_selection (&ids), k;
  menu *m, *show, *selm, *rating, *album, *newalbum, *addto, *sizem, *filter, *proj, *alb, *apply, *plugm, *vers, *autos;
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
  npresets = looks_presets (&presets);
  apply = apply_menu (nsel == 0);
  selm = new_menu ("Selection", SEL_COUNT);
  if (selm)
    {
      set_item (selm, SEL_RATING, 0, "Rating", 0, (int) rating, nsel == 0);
      set_item (selm, SEL_LABEL, 0, "Colour label", 0, (int) label_menu ("Colour label", -1), nsel == 0);
      set_item (selm, SEL_FLAG, 0, "Flag / unflag      /", 0, -1, nsel == 0);
      set_item (selm, SEL_REJECT, 0, "Reject      9", 0, -1, nsel == 0);
      set_item (selm, SEL_TAGS, 0, "Caption and keywords...      ^K", MF_DOTTED, -1, nsel == 0);
      set_item (selm, SEL_ALL, 0, "Select all      ^A", 0, -1, browser_count () == 0);
      set_item (selm, SEL_CLEAR, 0, "Clear selection      Esc", MF_DOTTED, -1, nsel == 0);
      set_item (selm, SEL_COPY, 0, "Copy adjustments      ^C", 0, -1, nsel == 0);
      set_item (selm, SEL_PASTE, 0, "Paste adjustments      ^V", 0, -1, nsel == 0 || !looks_clip ());
      set_item (selm, SEL_PRESET, 0, "Apply preset", MF_DOTTED, (int) apply, nsel == 0);
      set_item (selm, SEL_COMPARE, 0, "Compare      O", MF_DOTTED, -1, nsel != 2);
      set_item (selm, SEL_EXPORT, 0, "Export...      ^E", 0, -1, nsel == 0);
      set_item (selm, SEL_EXPORT_LIB, 0, "Export as library...", 0, -1, nsel == 0);
      set_item (selm, SEL_PRINT, 0, "Print...      ^P", MF_DOTTED, -1, nsel == 0);
      set_item (selm, SEL_REMOVE, 1, "Remove from library...      Delete", 0, -1, nsel == 0);
    }

  newalbum = writable_menu ("Name", album_name, sizeof album_name);
  addto = group_menu ("Add to", albums, nalbums, -1, "No albums yet");
  for (k = 0; addto && k < nalbums && k < MAX_LIST; k++)
    if (albums[k].smart)               /* a smart album gathers its own */
      addto->it[k].icon_flags |= (int) IF_SHADED;
  {
    int in_smart = 0;
    for (k = 0; k < nalbums; k++)
      if (s->kind == SRC_ALBUM && albums[k].id == s->id && albums[k].smart)
        in_smart = 1;
    album = new_menu ("Album", 4);
    if (album)
      {
        set_item (album, 0, 0, "New album from selection", 0, (int) newalbum, nsel == 0);
        set_item (album, 1, 0, "Add selection to", 0, (int) addto, nsel == 0 || nalbums == 0);
        set_item (album, 2, 0, "Remove from this album", MF_DOTTED, -1, nsel == 0 || s->kind != SRC_ALBUM || in_smart);
        set_item (album, 3, 1, "New smart album...", 0, -1, 0);
      }
  }

  sizem = new_menu ("Size", 3);
  if (sizem)
    {
      set_item (sizem, 0, 0, "Small", browser_size () == 0 ? MF_TICK : 0, -1, 0);
      set_item (sizem, 1, 0, "Medium", browser_size () == 1 ? MF_TICK : 0, -1, 0);
      set_item (sizem, 2, 1, "Large", browser_size () == 2 ? MF_TICK : 0, -1, 0);
    }

  /* Versions and stacks: on the selection, or the focused photo. */
  {
    int focused = browser_focus () >= 0, stacked = 0;
    cat_item fi;
    if (focused && browser_item (browser_focus (), &fi))
      stacked = fi.stack != 0;
    autos = writable_menu ("Seconds", stack_secs, sizeof stack_secs);
    vers = new_menu ("Versions", 7);
    if (vers)
      {
        set_item (vers, 0, 0, "Duplicate version      ^D", 0, -1, nsel == 0 && !focused);
        set_item (vers, 1, 0, "New version from original", MF_DOTTED, -1, nsel == 0 && !focused);
        set_item (vers, 2, 0, "Stack      ^G", 0, -1, nsel < 2);
        set_item (vers, 3, 0, "Unstack      ^U", 0, -1, !stacked && nsel == 0);
        set_item (vers, 4, 0, "Make the pick      \\", 0, -1, !stacked);
        set_item (vers, 5, 0, "Open or close stack      S", MF_DOTTED, -1, !stacked);
        set_item (vers, 6, 1, "Auto-stack, seconds apart", 0, (int) autos, browser_count () < 2);
      }
  }
  plugm = build_plugins_menu ();
  m = new_menu (APP_NAME, 7);
  if (m)
    {
      set_item (m, 0, 0, "Show", 0, (int) show, 0);
      set_item (m, 1, 0, "Selection", 0, (int) selm, 0);
      set_item (m, 2, 0, "Album", 0, (int) album, 0);
      set_item (m, 3, 0, "Versions and stacks", 0, (int) vers, 0);
      set_item (m, 4, 0, "Thumbnail size", MF_DOTTED, (int) sizem, 0);
      set_item (m, 5, 0, "Plug-ins", MF_DOTTED, (int) plugm, 0);
      set_item (m, 6, 1, "Quit", 0, -1, 0);
    }
  return m;
}

/* Photo > ... (the order menus_selection decodes) */
enum { VM_BEFORE, VM_ZOOM, VM_LOUPE, VM_UNDO, VM_REDO, VM_RESET, VM_AUTO, VM_PICK, VM_CROP, VM_SHAPE, VM_GRID,
       VM_PRESETS, VM_COPY, VM_PASTE, VM_LABEL, VM_TAGS, VM_COMPARE, VM_DUP, VM_NEW_ORIG, VM_EXPORT, VM_PRINT, VM_BACK, VM_REMOVE, VM_COUNT };

/* --- styles shown as the pointer goes down the list
   The Styles item asks for Message_MenuWarning (&400C0, PRM p1017): Lumen
   opens the list itself (Wimp_CreateSubMenu, PRM p721) and only from then
   on, while the photo is in the Viewer, does menus_idle look at the
   pointer, every 5 cs: Wimp_GetMenuState 1 says which item is under it,
   and that style is shown on the photo. Off the list, or when the menu goes
   (Message_MenusDeleted, &400C9), the photo's own look returns. In case
   that message is ever missed, Wimp_GetMenuState 0 saying no menu of ours
   is open ends it too. */
static int watching_styles, previewed = -1;

static int style_in_path (const int *c)
{
  int k = -1;
  if (which == 4 && c[0] == 0)
    k = c[1];
  else if (which == 3 && c[0] == VM_PRESETS && c[1] == 0)
    k = c[2];
  return k >= 0 && k < styles_count () ? k : -1;
}

static void watch_styles (void)
{
  watching_styles = viewer_has_photo () && !viewer_crop_mode ();
}

int menus_watching (void) { return watching_styles; }

void menus_closed (void);

/* Message_MenuWarning: open the submenu as the Wimp would; if it is a
   Styles list, start watching the pointer. */
void menus_warning (const int *b)
{
  const int *path = &b[8];
  if ((which == 4 && path[0] == 0) || (which == 3 && path[0] == VM_PRESETS && path[1] == 0))
    watch_styles ();
  _swix (Wimp_CreateSubMenu, _INR (1, 3), b[5], b[6], b[7]);
}

void menus_idle (void)
{
  int ptr[5], c[16], k = -1;
  if (!watching_styles)
    return;
  if (_swix (Wimp_GetMenuState, _INR (0, 1), 0, c) == NULL && c[0] == -1)
    {
      menus_closed ();                 /* gone (or not ours any more) without a word */
      return;
    }
  if (_swix (Wimp_GetPointerInfo, _IN (1), ptr) == NULL && ptr[3] >= 0
      && _swix (Wimp_GetMenuState, _INR (0, 3), 1, c, ptr[3], ptr[4]) == NULL && c[0] >= 0)
    k = style_in_path (c);
  if (k != previewed)
    {
      previewed = k;
      viewer_preview_look (k >= 0 ? styles_get (k)->recipe : NULL);
    }
}

void menus_closed (void)
{
  watching_styles = 0;
  if (previewed >= 0)
    {
      previewed = -1;
      viewer_preview_look (NULL);
    }
}

static menu *build_viewer_menu (void)
{
  menu *m, *shape, *grid, *pre, *loupe_m;
  int k, crop = viewer_crop_mode ();
  used = 0;
  shape = new_menu ("Crop shape", 6);
  for (k = 0; k < 6 && shape; k++)
    set_item (shape, k, k == 5, viewer_shape_name (k), viewer_crop_shape () == k ? MF_TICK : 0, -1, 0);
  grid = new_menu ("Grid", 4);
  for (k = 0; k < 4 && grid; k++)
    set_item (grid, k, k == 3, viewer_grid_name (k), viewer_grid () == k ? MF_TICK : 0, -1, 0);
  pre = build_presets ();
  loupe_m = new_menu ("Loupe", 3);
  for (k = 0; k < 3 && loupe_m; k++)
    {
      static const char *const mags[3] = { "100%", "200%", "400%" };
      set_item (loupe_m, k, k == 2, mags[k], viewer_loupe () == (1 << k) ? MF_TICK : 0, -1, 0);
    }
  m = new_menu ("Photo", VM_COUNT);
  if (m)
    {
      set_item (m, VM_BEFORE, 0, "Before and after      M", viewer_before () ? MF_TICK : 0, -1, crop);
      set_item (m, VM_ZOOM, 0, "Zoom to 100%      Z", viewer_zoomed () ? MF_TICK : 0, -1, crop);
      set_item (m, VM_LOUPE, 0, "Loupe      `", viewer_loupe () ? MF_TICK : 0, (int) loupe_m, crop);
      set_item (m, VM_UNDO, 0, "Undo      ^Z", 0, -1, !viewer_can_undo ());
      set_item (m, VM_REDO, 0, "Redo      ^Y", 0, -1, !viewer_can_redo ());
      set_item (m, VM_RESET, 0, "Reset all adjustments", MF_DOTTED, -1, 0);
      set_item (m, VM_AUTO, 0, "Auto adjust      A", 0, -1, crop);
      set_item (m, VM_PICK, 0, "Pick white balance", viewer_picking () ? MF_TICK : 0, -1, crop);
      set_item (m, VM_CROP, 0, "Crop      C", crop ? MF_TICK : 0, -1, 0);
      set_item (m, VM_SHAPE, 0, "Crop shape", 0, (int) shape, 0);
      set_item (m, VM_GRID, 0, "Grid      G", MF_DOTTED, (int) grid, 0);
      set_item (m, VM_PRESETS, 0, "Presets", 0, (int) pre, 0);
      set_item (m, VM_COPY, 0, "Copy adjustments      ^C", 0, -1, 0);
      set_item (m, VM_PASTE, 0, "Paste adjustments      ^V", MF_DOTTED, -1, looks_clip () == NULL);
      {
        cat_item vi;
        int now = viewer_index () >= 0 && browser_item (viewer_index (), &vi) ? vi.label : -1;
        set_item (m, VM_LABEL, 0, "Colour label", 0, (int) label_menu ("Colour label", now), 0);
      }
      set_item (m, VM_TAGS, 0, "Caption and keywords...      ^K", 0, -1, 0);
      set_item (m, VM_COMPARE, 0, viewer_comparing () ? "Stop comparing      O" : "Compare with the next      O",
                MF_DOTTED, -1, 0);
      set_item (m, VM_DUP, 0, "Duplicate version      ^D", 0, -1, 0);
      set_item (m, VM_NEW_ORIG, 0, "New version from original", MF_DOTTED, -1, 0);
      set_item (m, VM_EXPORT, 0, "Export...      ^E", 0, -1, 0);
      set_item (m, VM_PRINT, 0, "Print...      ^P", MF_DOTTED, -1, 0);
      set_item (m, VM_BACK, 0, "Back to the Browser      Esc", MF_DOTTED, -1, 0);
      set_item (m, VM_REMOVE, 1, "Remove from library...", 0, -1, 0);
    }
  return m;
}

void menus_open_presets_menu (int x, int y)
{
  menu *m;
  used = 0;
  m = build_presets ();
  which = 4;
  open_x = x - 64;
  open_y = y;
  if (m)
    _swix (Wimp_CreateMenu, _INR (1, 3), m, open_x, open_y);
}

void menus_open_viewer_menu (int x, int y)
{
  menu *m = build_viewer_menu ();
  which = 3;
  open_x = x - 64;
  open_y = y;
  if (m)
    _swix (Wimp_CreateMenu, _INR (1, 3), m, open_x, open_y);
}

static menu *build_iconbar_menu (void)
{
  menu *m, *plugm, *libm;
  used = 0;
  plugm = build_plugins_menu ();
  libm = new_menu ("Library", 3);
  if (libm)
    {
      set_item (libm, 0, 0, "Export library...", 0, -1, 0);
      set_item (libm, 1, 0, "Import library...", MF_DOTTED, -1, 0);
      set_item (libm, 2, 1, "Check library...", 0, -1, 0);
    }
  m = new_menu (APP_NAME, 8);
  if (m)
    {
      set_item (m, 0, 0, "Info", 0, info_window, info_window == -1);
      set_item (m, 1, 0, "Help...", 0, -1, 0);
      set_item (m, 2, 0, "User guide", MF_DOTTED, -1, 0);
      set_item (m, 3, 0, "Open library", 0, (int) libm, 0);
      set_item (m, 4, 0, "Activity...", MF_DOTTED, -1, 0);
      set_item (m, 5, 0, "Choices...", 0, -1, 0);
      set_item (m, 6, 0, "Plug-ins", MF_DOTTED, (int) plugm, 0);
      set_item (m, 7, 1, "Quit", 0, -1, 0);
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

/* The toolbar's New: an album, from the selection or empty. */
static menu *build_new_menu (void)
{
  cat_id *ids;
  int nsel = browser_selection (&ids);
  menu *m, *from, *empty;
  free (ids);
  used = 0;
  from = writable_menu ("Name", album_name, sizeof album_name);
  empty = writable_menu ("Name", album_name, sizeof album_name);
  m = new_menu ("New", 3);
  if (m)
    {
      set_item (m, 0, 0, "Album from selection", 0, (int) from, nsel == 0);
      set_item (m, 1, 0, "Empty album", 0, (int) empty, 0);
      set_item (m, 2, 1, "Smart album...", 0, -1, 0);
    }
  return m;
}

void menus_open_new_menu (int x, int y)
{
  menu *m = build_new_menu ();
  which = 5;
  open_x = x - 64;
  open_y = y;
  if (m)
    _swix (Wimp_CreateMenu, _INR (1, 3), m, open_x, open_y);
}

/* The bottom bar's rating filter. */
static menu *build_rating_menu (void)
{
  static const char *const names[6] = { "Any rating", "1 star and up", "2 stars and up", "3 stars and up",
                                        "4 stars and up", "5 stars" };
  const cat_source *s = browser_source ();
  menu *m;
  int k;
  used = 0;
  m = new_menu ("Show", 6);
  for (k = 0; k < 6 && m; k++)
    set_item (m, k, k == 5, names[k], s->min_rating == k ? MF_TICK : 0, -1, s->kind == SRC_REJECTED);
  return m;
}

void menus_open_rating_menu (int x, int y)
{
  menu *m = build_rating_menu ();
  which = 6;
  open_x = x;
  open_y = y + 6 * 44;                 /* opens upwards from the bar */
  if (m)
    _swix (Wimp_CreateMenu, _INR (1, 3), m, open_x, open_y);
}

/* Menu over a project or album in the Library tab. */
static menu *build_group_menu (void)
{
  menu *m, *ren;
  used = 0;
  ren = writable_menu ("Name", group_name, sizeof group_name);
  if (group_kind == SRC_ALBUM && cat_album_is_folder (lib_cat (app_library ()), group_id))
    {
      /* Made from a folder: it follows the folder, so nothing to change here. */
      m = new_menu ("Folder", 2);
      if (m)
        {
          set_item (m, 0, 0, "Follows its folder", MF_DOTTED, -1, 1);
          set_item (m, 1, 1, "Export as library...", 0, -1, 0);
        }
      return m;
    }
  if (group_kind == SRC_ALBUM)
    {
      cat_rule r;
      int smart = cat_smart_get (lib_cat (app_library ()), group_id, &r) == 1;
      m = new_menu (smart ? "Smart album" : "Album", smart ? 4 : 3);
      if (m)
        {
          set_item (m, 0, 0, "Rename", 0, (int) ren, 0);
          set_item (m, 1, 0, "Delete album...", smart ? 0 : MF_DOTTED, -1, 0);
          if (smart)
            set_item (m, 2, 0, "Edit smart album...", MF_DOTTED, -1, 0);
          set_item (m, smart ? 3 : 2, 1, "Export as library...", 0, -1, 0);
        }
      return m;
    }
  {
    int w = 0, al = 0;
    char root[1024];
    int has_root = cat_project_root (lib_cat (app_library ()), group_id, root, sizeof root) == 0;
    cat_project_watching (lib_cat (app_library ()), group_id, &w, &al);
    m = new_menu ("Project", 5);
    if (m)
      {
        set_item (m, 0, 0, "Rename", MF_DOTTED, (int) ren, 0);
        set_item (m, 1, 0, "Watch for new photos", w ? MF_TICK : 0, -1, !has_root);
        set_item (m, 2, 0, "Albums from its folders", (al ? MF_TICK : 0) | MF_DOTTED, -1, !has_root);
        set_item (m, 3, 0, "Export as library...", MF_DOTTED, -1, 0);
        set_item (m, 4, 1, "Remove project from library...", 0, -1, 0);
      }
  }
  return m;
}

void menus_open_group_menu (int x, int y, int kind, cat_id id)
{
  menu *m;
  group_kind = kind;
  group_id = id;
  cat_group_name (lib_cat (app_library ()), kind, id, group_name, sizeof group_name);
  m = build_group_menu ();
  which = 7;
  open_x = x - 64;
  open_y = y;
  if (m)
    _swix (Wimp_CreateMenu, _INR (1, 3), m, open_x, open_y);
}

/* The keywords already in the library, to add one to the Caption and
   keywords window's list. */
void menus_open_keyword_menu (int x, int y)
{
  menu *m;
  int k, count;
  free (keywords);
  keywords = NULL;
  nkeywords = 0;
  cat_keywords (lib_cat (app_library ()), &keywords, &nkeywords);
  count = nkeywords > 64 ? 64 : nkeywords;
  nkeywords = count;
  used = 0;
  m = new_menu ("Keywords", count ? count : 1);
  if (!m)
    return;
  if (!count)
    set_item (m, 0, 1, "None yet", 0, -1, 1);
  for (k = 0; k < count; k++)
    set_item (m, k, k == count - 1, keywords[k].name, 0, -1, 0);
  which = 8;
  open_x = x;
  open_y = y;
  _swix (Wimp_CreateMenu, _INR (1, 3), m, open_x, open_y);
}

/* Where Export as library... is on the group menu. */
static int group_export_item (void)
{
  cat *db = lib_cat (app_library ());
  cat_rule r;
  if (group_kind == SRC_PROJECT)
    return 3;
  if (cat_album_is_folder (db, group_id))
    return 1;
  return cat_smart_get (db, group_id, &r) == 1 ? 3 : 2;
}

static void group_choice (const int *c)
{
  cat *db = lib_cat (app_library ());
  char q[240], name[64];
  cat_group_name (db, group_kind, group_id, name, sizeof name);
  if (c[0] == group_export_item ())
    libexport_after_menu = 3;
  else if (c[0] == 0 && c[1] == 0)
    {
      if (cat_group_rename (db, group_kind, group_id, group_name))
        app_report (cat_error (db));
      browser_update_title ();
      mainwin_library_changed ();
    }
  else if (group_kind == SRC_PROJECT && (c[0] == 1 || c[0] == 2))
    {
      int w = 0, al = 0;
      cat_project_watching (db, group_id, &w, &al);
      if (c[0] == 1)
        w = !w;
      else
        al = !al;
      if (cat_project_set_watching (db, group_id, w, al))
        app_report (cat_error (db));
      if (al)
        cat_sync_folder_albums (db, group_id);
      app_watch_now ();                /* look now (and count what is watched) */
      mainwin_library_changed ();
    }
  else if (c[0] == 2 && group_kind == SRC_ALBUM)
    smart_after_menu = 2;
  else if (c[0] == 1 && group_kind == SRC_ALBUM)
    {
      snprintf (q, sizeof q, "Delete the album \"%s\"? Its photos stay in the library.", name);
      if (app_confirm (q, "Delete"))
        {
          cat_source all = { SRC_ALL, 0, 0, "" };
          cat_album_delete (db, group_id);
          if (browser_source ()->kind == SRC_ALBUM && browser_source ()->id == group_id)
            {
              all.min_rating = browser_source ()->min_rating;
              snprintf (all.search, sizeof all.search, "%s", browser_source ()->search);
              browser_set_source (&all);
            }
          mainwin_library_changed ();
        }
    }
  else if (c[0] == 4 && group_kind == SRC_PROJECT)
    {
      cat_id *ids;
      int n;
      if (cat_group_versions (db, SRC_PROJECT, group_id, &ids, &n))
        return;
      snprintf (q, sizeof q, "Remove the project \"%s\" and its %d photo%s from the library? The files "
                "aren't touched.", name, n, n == 1 ? "" : "s");
      if (app_confirm (q, "Remove"))
        {
          if (lib_remove (app_library (), ids, n))
            app_report (cat_error (db));
          else
            {
              if (browser_source ()->kind == SRC_PROJECT && browser_source ()->id == group_id)
                {
                  cat_source all = { SRC_ALL, 0, 0, "" };
                  browser_set_source (&all);
                }
              browser_removed (ids, n);
            }
        }
      free (ids);
    }
}

void menus_open_iconbar_menu (int x)
{
  menu *m = build_iconbar_menu ();
  which = 2;
  open_x = x - 64;
  open_y = 96 + 7 * 44;
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
        case 5: cat_set_label (c, ids, k, arg); break;
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
      switch (c[1])
        {
        case SEL_RATING: if (c[2] >= 0 && c[2] <= 5) with_selection (0, c[2]); break;
        case SEL_LABEL: if (c[2] >= 0 && c[2] < CAT_LABELS) with_selection (5, c[2]); break;
        case SEL_FLAG: with_selection (1, 0); break;
        case SEL_REJECT: with_selection (0, RATING_REJECT); break;
        case SEL_TAGS: tags_after_menu = 1; break;
        case SEL_ALL: browser_select_all (1); break;
        case SEL_CLEAR: browser_select_all (0); break;
        case SEL_COPY: looks_copy_selection (); break;
        case SEL_PASTE: if (looks_clip ()) looks_apply_selection (looks_clip ()); break;
        case SEL_PRESET:
          if (c[2] == 0 && styles_get (c[3]))
            looks_apply_selection (styles_get (c[3])->recipe);
          else if (c[2] >= 1 && c[2] <= npresets && c[2] <= MAX_LIST)
            looks_apply_selection (presets[c[2] - 1].recipe);
          break;
        case SEL_COMPARE: compare_after_menu = 1; break;
        case SEL_EXPORT: export_after_menu = 1; break;
        case SEL_EXPORT_LIB: libexport_after_menu = 2; break;
        case SEL_PRINT: print_after_menu = 1; break;
        case SEL_REMOVE: remove_after_menu = 1; break;
        }
      break;
    case 2:                            /* Album */
      if (c[1] == 0 && c[2] == 0)
        with_selection (4, 0);
      else if (c[1] == 1 && c[2] >= 0 && c[2] < nalbums && !albums[c[2]].smart)
        with_selection (2, (int) albums[c[2]].id);
      else if (c[1] == 2)
        with_selection (3, 0);
      else if (c[1] == 3)
        smart_after_menu = 1;
      break;
    case 3:                            /* Versions and stacks */
      switch (c[1])
        {
        case 0: browser_new_version (1); break;
        case 1: browser_new_version (0); break;
        case 2: browser_stack (); break;
        case 3: browser_unstack (); break;
        case 4: browser_make_pick (); break;
        case 5: browser_toggle_stack (); break;
        case 6:
          if (c[2] == 0)
            {
              int secs = atoi (stack_secs), made;
              if (secs < 0)
                secs = 0;
              made = browser_auto_stack (secs);
              activity_log (ACT_OTHER, "Auto-stack: %d stack%s of photos taken within %d second%s",
                            made, made == 1 ? "" : "s", secs, secs == 1 ? "" : "s");
            }
          break;
        }
      break;
    case 4:                            /* Size */
      if (c[1] >= 0 && c[1] <= 2)
        browser_set_size (c[1]);
      break;
    case 5:                            /* Plug-ins */
      plugins_choice (c + 1);
      break;
    case 6:
      return 1;
    }
  return 0;
}

/* Returns 1 to quit. */
int menus_selection (const int *choice)
{
  int ptr[5] = { 0 }, quit_requested = 0, was_style = watching_styles && style_in_path (choice) >= 0;
  if (watching_styles)
    {
      /* A style chosen replaces the preview (viewer_apply_look starts from
         the photo's own recipe); anything else puts the photo back first. */
      if (previewed >= 0 && style_in_path (choice) < 0)
        viewer_preview_look (NULL);
      watching_styles = 0;
      previewed = -1;
    }
  if (which == 1)
    quit_requested = window_choice (choice);
  else if (which == 3)
    switch (choice[0])
      {
      case VM_BEFORE: viewer_toggle_before (); break;
      case VM_ZOOM: viewer_toggle_zoom (); break;
      case VM_LOUPE:
        if (choice[1] >= 0)
          {
            if (!viewer_loupe ())
              viewer_toggle_loupe ();
            viewer_set_loupe_mag (1 << choice[1]);
          }
        else
          viewer_toggle_loupe ();
        break;
      case VM_UNDO: viewer_undo (); break;
      case VM_REDO: viewer_redo (); break;
      case VM_RESET: viewer_reset_all (); break;
      case VM_AUTO: viewer_auto (); break;
      case VM_PICK: viewer_pick_white (); break;
      case VM_CROP: viewer_toggle_crop (); break;
      case VM_SHAPE: if (choice[1] >= 0) viewer_set_crop_shape (choice[1]); break;
      case VM_GRID: if (choice[1] >= 0) viewer_set_grid (choice[1]); break;
      case VM_PRESETS: if (choice[1] >= 0) presets_choice (choice + 1); break;
      case VM_COPY: looks_copy_viewer (); break;
      case VM_PASTE: if (looks_clip ()) viewer_apply_look (looks_clip ()); break;
      case VM_TAGS: tags_after_menu = 2; break;
      case VM_LABEL:
        if (choice[1] >= 0 && choice[1] < CAT_LABELS && viewer_item ())
          {
            cat_id v = viewer_item ();
            cat_set_label (lib_cat (app_library ()), &v, 1, choice[1]);
            browser_after_change ();
            mainwin_info_changed ();
          }
        break;
      case VM_COMPARE: if (viewer_comparing ()) viewer_key ('O'); else viewer_compare (); break;
      case VM_DUP: viewer_new_version (1); break;
      case VM_NEW_ORIG: viewer_new_version (0); break;
      case VM_EXPORT: export_after_menu = 2; break;
      case VM_PRINT: print_after_menu = 2; break;
      case VM_BACK: viewer_close (); return 0;
      case VM_REMOVE:
        {
          /* This photo alone: the Browser's focus is the Viewer's. */
          int i = viewer_index ();
          if (i >= 0)
            browser_focus_on (i);
          remove_after_menu = 1;
        }
        break;
      }
  else if (which == 4)
    presets_choice (choice);
  else if (which == 7)
    group_choice (choice);
  else if (which == 8)
    {
      if (choice[0] >= 0 && choice[0] < nkeywords)
        tagbox_add_keyword (keywords[choice[0]].name);
    }
  else if (which == 6)
    {
      if (choice[0] >= 0 && choice[0] <= 5)
        {
          cat_source s = *browser_source ();
          s.min_rating = choice[0];
          browser_set_source (&s);
          mainwin_status_changed ();
        }
    }
  else if (which == 5)
    {
      if (choice[0] == 0 && choice[1] == 0)
        with_selection (4, 0);
      else if (choice[0] == 1 && choice[1] == 0 && album_name[0])
        {
          cat_album_create (lib_cat (app_library ()), album_name);
          album_name[0] = '\0';
          browser_after_change ();
        }
      else if (choice[0] == 2)
        smart_after_menu = 1;
    }
  else if (which == 2)
    {
      if (choice[0] == 1)
        app_open_doc ("!Help");
      else if (choice[0] == 2)
        app_open_doc ("Guide.index/html");
      else if (choice[0] == 3 && choice[1] == 0)
        libexport_after_menu = 1;
      else if (choice[0] == 3 && choice[1] == 1)
        libbox_import_open ();
      else if (choice[0] == 3 && choice[1] == 2)
        libbox_check_open ();
      else if (choice[0] == 3)
        mainwin_open (NULL);
      else if (choice[0] == 4)
        activitywin_open ();
      else if (choice[0] == 5)
        choicewin_open ();
      else if (choice[0] == 6)
        plugins_choice (choice + 1);
      else if (choice[0] == 7)
        quit_requested = 1;
    }
  if (quit_requested)
    return 1;
  if (plugin_remove_after_menu)
    {
      int k = plugin_remove_after_menu - 1;
      plugin_remove_after_menu = 0;
      app_plugin_remove (k);
      return 0;
    }
  if (plugwin_after_menu)
    {
      plugwin_after_menu = 0;
      plugwin_open ();
      return 0;
    }
  if (remove_after_menu)
    {
      /* The question replaces the menu. */
      remove_after_menu = 0;
      browser_remove_selection ();
      return 0;
    }
  if (compare_after_menu)
    {
      compare_after_menu = 0;
      browser_compare ();
      return 0;
    }
  if (tags_after_menu)
    {
      /* The window opens once the menu has gone, at the pointer. */
      int from_viewer = tags_after_menu == 2;
      tags_after_menu = 0;
      if (_swix (Wimp_GetPointerInfo, _IN (1), ptr) == NULL)
        tagbox_open (from_viewer, ptr[0], ptr[1]);
      return 0;
    }
  if (smart_after_menu)
    {
      cat_id album = smart_after_menu == 2 ? group_id : 0;
      smart_after_menu = 0;
      if (_swix (Wimp_GetPointerInfo, _IN (1), ptr) == NULL)
        smartbox_open (album, ptr[0], ptr[1]);
      return 0;
    }
  if (print_after_menu)
    {
      /* The Print box replaces the menu, at the pointer. */
      int from_viewer = print_after_menu == 2;
      print_after_menu = 0;
      if (_swix (Wimp_GetPointerInfo, _IN (1), ptr) == NULL)
        printbox_open (from_viewer, ptr[0], ptr[1]);
      return 0;
    }
  if (libexport_after_menu)
    {
      /* The Export library box replaces the menu, at the pointer. */
      int kind = libexport_after_menu;
      libexport_after_menu = 0;
      if (_swix (Wimp_GetPointerInfo, _IN (1), ptr))
        return 0;
      if (kind == 1)
        libbox_export_open (NULL, -1, "The whole library", "Library", ptr[0], ptr[1]);
      else if (kind == 2)
        {
          cat_id *ids;
          int n = browser_selection (&ids);
          libbox_export_open (ids, n, "The selection", "Photos", ptr[0], ptr[1]);
          free (ids);
        }
      else
        {
          cat_id *ids;
          int n;
          char what[100], nm[64];
          cat_group_name (lib_cat (app_library ()), group_kind, group_id, nm, sizeof nm);
          if (cat_group_versions (lib_cat (app_library ()), group_kind, group_id, &ids, &n) == 0)
            {
              snprintf (what, sizeof what, "%s %s", group_kind == SRC_PROJECT ? "The project" : "The album", nm);
              libbox_export_open (ids, n, what, nm, ptr[0], ptr[1]);
              free (ids);
            }
        }
      return 0;
    }
  if (export_after_menu)
    {
      /* The Export box replaces the menu, at the pointer (as a menu would open). */
      int from_viewer = export_after_menu == 2;
      export_after_menu = 0;
      if (_swix (Wimp_GetPointerInfo, _IN (1), ptr) == NULL)
        exportbox_open (from_viewer, ptr[0], ptr[1]);
      return 0;
    }
  /* Adjust keeps the menu open, rebuilt with fresh ticks. */
  if (_swix (Wimp_GetPointerInfo, _IN (1), ptr) == NULL && (ptr[2] & Button_Adjust))
    {
      menu *m;
      if (which == 8)
        {
          menus_open_keyword_menu (open_x, open_y);   /* Adjust: add another */
          return 0;
        }
      if (which == 4)
        {
          used = 0;
          m = build_presets ();
        }
      else
        m = which == 1 ? build_window_menu () : which == 3 ? build_viewer_menu ()
          : which == 5 ? build_new_menu () : which == 6 ? build_rating_menu ()
          : which == 7 ? build_group_menu () : build_iconbar_menu ();
      if (m)
        _swix (Wimp_CreateMenu, _INR (1, 3), m, open_x, open_y);
      if (was_style)
        watch_styles ();               /* Adjust on a style: the list is open again */
    }
  return 0;
}

/* --- help for menu items (Wimp_GetMenuState 1: the item
   under the pointer as a list of selections, -1 terminated; -1 first if
   the pointer isn't over Lumen's menu) */
const char *menus_help (int window, int icon)
{
  int c[16];
  const plugin_info *pl;
  int n = app_plugins (&pl), base;
  if (window < 0 || _swix (Wimp_GetMenuState, _INR (0, 3), 1, c, window, icon) || c[0] < 0)
    return NULL;
  if (n > MAX_LIST)
    n = MAX_LIST;
  base = n ? n : 1;
  /* The Plug-ins submenu, from the icon bar menu (2) or the window menu (4). */
  if ((which == 2 && c[0] == 6) || (which == 1 && c[0] == 5))
    {
      if (c[1] < 0)
        return "Lumen's plug-ins: switch them on or off, add or remove them.";
      if (c[1] < n)
        return pl[c[1]].enabled ? "Ticked: this plug-in is on. Choose it to switch it off (it stays installed)."
                                : "Not ticked: this plug-in is off. Choose it to switch it on.";
      if (c[1] == base)
        return "Opens a window to drag a plug-in's directory into. Plug-ins come from PackMan.";
      if (c[1] == base + 1)
        return c[2] >= 0 ? "Deletes this plug-in from Lumen, after asking." : "Deletes a plug-in from Lumen.";
      return "";
    }
  if (which == 2)
    switch (c[0])
      {
      case 0: return "Who made Lumen, and its version.";
      case 1: return "Shows Lumen's short help file.";
      case 2: return "Opens the Lumen User Guide in your web browser.";
      case 3:
        switch (c[1])
          {
          case 0: return "Copies the whole library to a new one: every photo, adjustment, album and keyword, and the "
                         "originals if you choose. For a backup, or to move it to another disc or computer.";
          case 1: return "Brings another Lumen library's photos into this one: drag its directory into the window. "
                         "Photos already here aren't added twice.";
          case 2: return "Checks the catalogue and the thumbnails for anything wrong, and puts it right if you ask.";
          }
        return "Opens Lumen's window. Its menu: export, import or check the library.";
      case 4: return "What Lumen is doing now and what it has done: imports, thumbnails, exports, watched folders.";
      case 5: return "Where the library is, how exports and thumbnails start, and checking for missing files.";
      case 7: return "Leaves Lumen. Everything is saved as you go.";
      }
  if (which == 1)
    switch (c[0])
      {
      case 0: return "Which photos the Browser shows: all, flagged, rejected, the last import, a project or album, by rating.";
      case 1:
        if (c[1] == SEL_REMOVE)
          return "Takes the selected photos out of the library, after asking. Their files are not deleted.";
        if (c[1] == SEL_PRINT)
          return "Prints the selected photos, one to a page, on the printer chosen in !Printers.";
        if (c[1] == SEL_EXPORT)
          return "Exports the selected photos as new JPEG or PNG files.";
        if (c[1] == SEL_EXPORT_LIB)
          return "Makes a new Lumen library of the selected photos, with all their adjustments, keywords and albums "
                 "(and their originals, if you choose): to take them to another computer.";
        if (c[1] == SEL_PRESET && c[2] >= 0)
          return presets_help (c + 2, 1);
        if (c[1] == SEL_TAGS)
          return "Gives the selected photos a caption and keywords, which search and smart albums find.";
        if (c[1] == SEL_LABEL)
          return "Gives the selected photos a colour label (or None). Search and smart albums find labels by name.";
        if (c[1] == SEL_COMPARE)
          return "Shows the two selected photos side by side in the Viewer, to choose between them.";
        return "Rate, flag, reject, caption, select, copy and paste adjustments, export or remove the selected photos.";
      case 2:
        if (c[1] == 3)
          return "Makes an album that gathers the photos matching what you choose: words, rating, flag, dates.";
        if (c[1] == 1 && c[2] >= 0 && c[2] < nalbums && albums[c[2]].smart)
          return "A smart album gathers its own photos: change what it looks for instead (its menu in the Library tab).";
        return "Make albums from the selection, add the selection to one, or take it out of this one.";
      case 3:
        switch (c[1])
          {
          case 0: return "Makes another version of each selected photo, with its adjustments, to try another look. "
                         "The original file is shared; the versions are stacked.";
          case 1: return "Makes another version of each selected photo as it came from the camera, without adjustments.";
          case 2: return "Stacks the selected photos under one (the one with the focus): a burst, a bracket, versions.";
          case 3: return "Takes the selected photos' stacks apart.";
          case 4: return "Makes this photo its stack's pick: the one shown while the stack is closed.";
          case 5: return "Opens the stack to show all its photos, or closes it to show only its pick. "
                         "Clicking the number on the pick does it too.";
          case 6: return "Stacks photos taken within this many seconds of each other (bursts and brackets): "
                         "the selected ones, or all those shown.";
          }
        return "Versions of a photo, and stacks of photos that belong together.";
      case 4: return "Thumbnail size: Small, Medium or Large.";
      case 6: return "Leaves Lumen. Everything is saved as you go.";
      }
  if (which == 4)
    return presets_help (c, 0);
  if (which == 3)
    return c[0] == VM_PRESETS && c[1] >= 0 ? presets_help (c + 1, 0)
                                           : "What can be done to this photo. Many have keys, shown beside them.";
  if (which == 8)
    return "Adds this keyword to the list.";
  if (which == 7 && c[0] == group_export_item ())
    return group_kind == SRC_PROJECT
           ? "Makes a new Lumen library of this project's photos, with all their adjustments (and their originals, if you choose)."
           : "Makes a new Lumen library of this album's photos, with all their adjustments (and their originals, if you choose).";
  if (which == 7)
    switch (c[0])
      {
      case 0: return group_kind == SRC_ALBUM && cat_album_is_folder (lib_cat (app_library ()), group_id)
                     ? "This album is made from a folder: it holds the photos in that folder and the folders inside it, "
                       "and follows them as they change."
                     : "Gives it a new name.";
      case 1: return group_kind == SRC_PROJECT ? "Ticked: Lumen looks in this project's folder for new photos at start, "
                                                 "when its window opens and every few minutes (Choices)."
                                               : "Deletes the album. Its photos stay in the library.";
      case 2: return group_kind == SRC_ALBUM ? "Changes what this smart album looks for."
                                             : "Ticked: each folder inside this project's folder is an album, under the project in the Library tab.";
      case 4: return "Takes the project and its photos out of the library, after asking. The files aren't touched.";
      }
  return "";
}
