/* tagbox.c: the Caption and keywords window.

   For one photo it shows its caption and keywords to change; for several
   (the selection), the caption they share, if they all have the same, and
   the keywords every one of them has. Saving gives the caption to all of
   them if it was changed, adds each keyword in the list to all of them, and
   takes away from all of them the shared keywords taken out of the list;
   keywords only some of them have stay as they are.

   The menu button beside the keywords lists the keywords already in the
   library; choosing one adds it to the list. It is a window of its own
   (not a dialogue box in a menu) so that menu can open over it. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <kernel.h>
#include <swis.h>
#include "ui.h"

enum { TG_CAP_LABEL, TG_CAP, TG_KW_LABEL, TG_KW, TG_KW_MENU, TG_NOTE, TG_CANCEL, TG_SAVE, TG_COUNT };

#define W 1000
#define H 300
#define T_LABEL   0x17000311u
#define T_NOTE    0x17000111u
#define T_WRITE   0x0700F135u
#define T_BUTTON  0x1700313Du
#define T_POPUP   0x1700310Bu          /* sprite, border, centred, indirected, click */

static int win = -1, is_open;
static char cap_text[256], kw_text[512], note[100];
static char orig_cap[256], orig_kw[512];
static int orig_same;
static cat_id *ids;
static int nids;

int tagbox_window (void) { return win; }
int tagbox_is_open (void) { return is_open; }
const char *tagbox_caption_text (void) { return cap_text; }
const char *tagbox_keywords_text (void) { return kw_text; }
const char *tagbox_note (void) { return note; }

void tagbox_set_texts (const char *caption, const char *keywords)
{
  snprintf (cap_text, sizeof cap_text, "%s", caption);
  snprintf (kw_text, sizeof kw_text, "%s", keywords);
}

static void icon_at (int *ic, int x0, int y0, int x1, int y1, unsigned flags, const char *text, const char *valid,
                     int size)
{
  ic[0] = x0; ic[1] = y0; ic[2] = x1; ic[3] = y1;
  ic[4] = (int) flags;
  ic[5] = (int) text; ic[6] = valid ? (int) valid : -1; ic[7] = size;
}

int tagbox_create (void)
{
  static const char v_write[] = "Kta;Pptr_write", v_popup[] = "R5;Sgright,pgright";
  static const char v_save[] = "R6,3", v_cancel[] = "R5,3";
  static char t_cap[] = "Caption", t_kw[] = "Keywords", t_cancel[] = "Cancel", t_save[] = "Save", t_none[] = "";
  int b[22 + 8 * TG_COUNT];
  int *ic = &b[22];
  memset (b, 0, sizeof b);
  b[0] = 0; b[1] = 0; b[2] = W; b[3] = H;
  b[6] = -1;
  b[7] = (int) 0x86000012;             /* moveable, close icon, title, new format */
  ((unsigned char *) b)[32] = 7;  ((unsigned char *) b)[33] = 2;
  ((unsigned char *) b)[34] = 7;  ((unsigned char *) b)[35] = 1;
  ((unsigned char *) b)[36] = 3;  ((unsigned char *) b)[37] = 1;
  ((unsigned char *) b)[38] = 12;
  b[10] = 0; b[11] = -H; b[12] = W; b[13] = 0;
  b[14] = 0x3D;
  b[16] = 1;
  memcpy (&b[18], "Keywords", 9);
  b[21] = TG_COUNT;
  icon_at (ic + 8 * TG_CAP_LABEL, 16, -76, 196, -32, T_LABEL, t_cap, NULL, (int) sizeof t_cap);
  icon_at (ic + 8 * TG_CAP, 212, -80, W - 24, -28, T_WRITE, cap_text, v_write, (int) sizeof cap_text);
  icon_at (ic + 8 * TG_KW_LABEL, 16, -144, 196, -100, T_LABEL, t_kw, NULL, (int) sizeof t_kw);
  icon_at (ic + 8 * TG_KW, 212, -148, W - 92, -96, T_WRITE, kw_text, v_write, (int) sizeof kw_text);
  icon_at (ic + 8 * TG_KW_MENU, W - 80, -148, W - 24, -96, T_POPUP, t_none, v_popup, 1);
  icon_at (ic + 8 * TG_NOTE, 212, -204, W - 24, -160, T_NOTE, note, NULL, (int) sizeof note);
  icon_at (ic + 8 * TG_CANCEL, W - 384, -280, W - 220, -224, T_BUTTON, t_cancel, v_cancel, (int) sizeof t_cancel);
  icon_at (ic + 8 * TG_SAVE, W - 200, -284, W - 24, -220, T_BUTTON, t_save, v_save, (int) sizeof t_save);
  return _swix (Wimp_CreateWindow, _IN (1) | _OUT (0), b, &win) ? -1 : 0;
}

static void redraw_icon (int i)
{
  int s[4];
  s[0] = win; s[1] = i; s[2] = 0; s[3] = 0;
  _swix (Wimp_SetIconState, _IN (1), s);
}

static void close_win (void)
{
  int b[1];
  b[0] = win;
  if (is_open)
    _swix (Wimp_CloseWindow, _IN (1), b);
  is_open = 0;
}

/* Opens the window for these photos (the list is copied), its top left
   near x, y. */
void tagbox_open_ids (const cat_id *v, int n, int x, int y)
{
  int s[9];
  cat *c = lib_cat (app_library ());
  cat_id *copy;
  if (n <= 0 || !(copy = malloc (sizeof *copy * (size_t) n)))
    return;
  memcpy (copy, v, sizeof *copy * (size_t) n);
  free (ids);
  ids = copy;
  nids = n;
  if (cat_common_tags (c, ids, n, orig_cap, sizeof orig_cap, &orig_same, orig_kw, sizeof orig_kw))
    {
      app_report (cat_error (c));
      return;
    }
  snprintf (cap_text, sizeof cap_text, "%s", orig_cap);
  snprintf (kw_text, sizeof kw_text, "%s", orig_kw);
  if (n == 1)
    snprintf (note, sizeof note, "Commas between keywords. Search finds both.");
  else
    snprintf (note, sizeof note, "%d photos: %s, and the keywords they all have.", n,
              orig_same ? "their caption" : "their captions differ");
  redraw_icon (TG_CAP);
  redraw_icon (TG_KW);
  redraw_icon (TG_NOTE);
  s[0] = win;
  if (_swix (Wimp_GetWindowState, _IN (1), s))
    return;
  if (!is_open)
    {
      s[1] = x - 64; s[3] = s[1] + W;
      s[4] = y; s[2] = y - H;
      if (s[2] < 96)                   /* above the icon bar */
        {
          s[2] = 96;
          s[4] = 96 + H;
        }
      s[5] = 0; s[6] = 0;
    }
  s[7] = -1;
  _swix (Wimp_OpenWindow, _IN (1), s);
  is_open = 1;
  _swix (Wimp_SetCaretPosition, _INR (0, 5), win, n == 1 || orig_same ? TG_CAP : TG_KW, 0, 0, -1,
         (int) strlen (n == 1 || orig_same ? cap_text : kw_text));
}

/* The selection (or the Viewer's photo). */
void tagbox_open (int from_viewer, int x, int y)
{
  cat_id *v = NULL, one;
  int n;
  if (from_viewer)
    {
      if (!viewer_item ())
        return;
      one = viewer_item ();
      tagbox_open_ids (&one, 1, x, y);
      return;
    }
  n = browser_selection (&v);
  if (n > 0)
    tagbox_open_ids (v, n, x, y);
  free (v);
}

/* A keyword chosen from the menu: added to the list, if not there yet. */
typedef struct { const char *kw; int found; } kw_find;

static void kw_match (void *ctx, const char *kw)
{
  kw_find *f = ctx;
  if (!strcasecmp (kw, f->kw))
    f->found = 1;
}

void tagbox_add_keyword (const char *kw)
{
  kw_find f = { kw, 0 };
  size_t len;
  cat_keyword_split (kw_text, kw_match, &f);
  if (f.found)
    return;
  len = strlen (kw_text);
  while (len && (kw_text[len - 1] == ' ' || kw_text[len - 1] == ','))
    kw_text[--len] = '\0';
  snprintf (kw_text + len, sizeof kw_text - len, "%s%s", len ? ", " : "", kw);
  redraw_icon (TG_KW);
  _swix (Wimp_SetCaretPosition, _INR (0, 5), win, TG_KW, 0, 0, -1, (int) strlen (kw_text));
}

/* The keywords in the original list that the new one no longer has. */
typedef struct { const char *list; char *out; size_t n; } kw_gone;

static void kw_check_gone (void *ctx, const char *kw)
{
  kw_gone *g = ctx;
  kw_find f = { kw, 0 };
  cat_keyword_split (g->list, kw_match, &f);
  if (!f.found)
    {
      size_t len = strlen (g->out);
      snprintf (g->out + len, g->n - len, "%s%s", len ? ", " : "", kw);
    }
}

static void trim (char *s)
{
  size_t l;
  char *p = s;
  while (*p == ' ')
    p++;
  memmove (s, p, strlen (p) + 1);
  l = strlen (s);
  while (l && (unsigned char) s[l - 1] <= ' ')
    s[--l] = '\0';
}

static void save (void)
{
  cat *c = lib_cat (app_library ());
  char gone[600] = "";
  kw_gone g = { kw_text, gone, sizeof gone };
  int rc = 0;
  trim (cap_text);
  cat_keyword_split (orig_kw, kw_check_gone, &g);
  cat_begin (c);
  if (nids == 1 || strcmp (cap_text, orig_cap))
    rc |= cat_set_caption (c, ids, nids, cap_text);
  if (gone[0])
    rc |= cat_remove_keywords (c, ids, nids, gone);
  rc |= cat_add_keywords (c, ids, nids, kw_text);
  cat_commit (c);
  if (rc)
    app_report (cat_error (c));
  close_win ();
  mainwin_info_changed ();
  browser_after_change ();             /* a search or smart album may show other photos now */
}

void tagbox_click (int *b)
{
  if (b[2] == Button_Menu)
    return;
  switch (b[4])
    {
    case TG_KW_MENU:
      {
        int st[9];
        st[0] = win;
        if (_swix (Wimp_GetWindowState, _IN (1), st) == NULL)
          {
            int ox = st[1] - st[5], oy = st[4] - st[6];
            menus_open_keyword_menu (ox + W - 24 + 8, oy - 96);
          }
        break;
      }
    case TG_CANCEL: close_win (); break;
    case TG_SAVE: save (); break;
    }
}

int tagbox_key (int key)
{
  if (key == KEY_RETURN)
    {
      save ();
      return 1;
    }
  if (key == KEY_ESCAPE)
    {
      close_win ();
      return 1;
    }
  return 0;
}

void tagbox_closed (void)
{
  is_open = 0;
}
