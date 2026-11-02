/* smartbox.c: the Smart album window, to make a smart album or change one.

   A smart album holds the photos that match its rule now (catalog.h):
   words they must all match (the name, camera, date, project, caption or a
   keyword), a lowest rating, flagged only, adjusted only, and the dates
   they were taken between. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <kernel.h>
#include <swis.h>
#include "ui.h"

enum
{
  SB_NAME_LABEL, SB_NAME, SB_WORDS_LABEL, SB_WORDS, SB_WORDS_NOTE, SB_RATING_LABEL, SB_R0, SB_R1, SB_R2, SB_R3, SB_R4,
  SB_R5, SB_FLAGGED, SB_EDITED, SB_DATES_LABEL, SB_FROM, SB_TO_LABEL, SB_TO, SB_DATES_NOTE, SB_NOTE, SB_CANCEL, SB_OK,
  SB_COUNT
};

#define W 1080
#define H 600
#define T_LABEL   0x17000311u
#define T_NOTE    0x17000111u
#define T_WRITE   0x0700F135u
#define T_RADIO   0x1700B113u
#define T_BUTTON  0x1700313Du

static int win = -1, is_open;
static cat_id editing;                 /* the album being changed, 0 for a new one */
static char name_text[40], words_text[64], from_text[12], to_text[12], note[100], ok_text[8] = "Create";

int smartbox_window (void) { return win; }
int smartbox_is_open (void) { return is_open; }
const char *smartbox_note (void) { return note; }

static void icon_at (int *ic, int x0, int y0, int x1, int y1, unsigned flags, const char *text, const char *valid,
                     int size)
{
  ic[0] = x0; ic[1] = y0; ic[2] = x1; ic[3] = y1;
  ic[4] = (int) flags;
  ic[5] = (int) text; ic[6] = valid ? (int) valid : -1; ic[7] = size;
}

int smartbox_create (void)
{
  static const char v_write[] = "Kta;Pptr_write", v_date[] = "A0-9/:.\\-;Kta;Pptr_write";
  static const char v_radio[] = "Sradiooff,radioon", v_option[] = "Soptoff,opton";
  static const char v_ok[] = "R6,3", v_cancel[] = "R5,3";
  static char t_name[] = "Name", t_words[] = "Containing", t_wnote[] = "In the name, caption, keywords, camera or date";
  static char t_rating[] = "Rating", t_any[] = "Any", t_1[] = "1+", t_2[] = "2+", t_3[] = "3+", t_4[] = "4+", t_5[] = "5";
  static char t_flagged[] = "Flagged only", t_edited[] = "Adjusted only", t_dates[] = "Taken from", t_to[] = "to";
  static char t_dnote[] = "Such as 2026, 2026-06 or 16/6/2026", t_cancel[] = "Cancel";
  static char *const t_r[6] = { t_any, t_1, t_2, t_3, t_4, t_5 };
  int b[22 + 8 * SB_COUNT], k;
  int *ic = &b[22];
  memset (b, 0, sizeof b);
  b[0] = 0; b[1] = 0; b[2] = W; b[3] = H;
  b[6] = -1;
  b[7] = (int) 0x86000012;
  ((unsigned char *) b)[32] = 7;  ((unsigned char *) b)[33] = 2;
  ((unsigned char *) b)[34] = 7;  ((unsigned char *) b)[35] = 1;
  ((unsigned char *) b)[36] = 3;  ((unsigned char *) b)[37] = 1;
  ((unsigned char *) b)[38] = 12;
  b[10] = 0; b[11] = -H; b[12] = W; b[13] = 0;
  b[14] = 0x3D;
  b[16] = 1;
  memcpy (&b[18], "Smart album", 12);
  b[21] = SB_COUNT;
  icon_at (ic + 8 * SB_NAME_LABEL, 16, -76, 216, -32, T_LABEL, t_name, NULL, (int) sizeof t_name);
  icon_at (ic + 8 * SB_NAME, 232, -80, 732, -28, T_WRITE, name_text, v_write, (int) sizeof name_text);
  icon_at (ic + 8 * SB_WORDS_LABEL, 16, -144, 216, -100, T_LABEL, t_words, NULL, (int) sizeof t_words);
  icon_at (ic + 8 * SB_WORDS, 232, -148, W - 24, -96, T_WRITE, words_text, v_write, (int) sizeof words_text);
  icon_at (ic + 8 * SB_WORDS_NOTE, 232, -196, W - 24, -152, T_NOTE, t_wnote, NULL, (int) sizeof t_wnote);
  icon_at (ic + 8 * SB_RATING_LABEL, 16, -256, 216, -212, T_LABEL, t_rating, NULL, (int) sizeof t_rating);
  for (k = 0; k < 6; k++)
    icon_at (ic + 8 * (SB_R0 + k), 232 + k * 130, -256, 232 + k * 130 + 124, -212, T_RADIO | (1u << 16), t_r[k],
             v_radio, (int) strlen (t_r[k]) + 1);
  icon_at (ic + 8 * SB_FLAGGED, 232, -316, 560, -272, T_RADIO, t_flagged, v_option, (int) sizeof t_flagged);
  icon_at (ic + 8 * SB_EDITED, 580, -316, 900, -272, T_RADIO, t_edited, v_option, (int) sizeof t_edited);
  icon_at (ic + 8 * SB_DATES_LABEL, 16, -380, 216, -336, T_LABEL, t_dates, NULL, (int) sizeof t_dates);
  icon_at (ic + 8 * SB_FROM, 232, -384, 472, -332, T_WRITE, from_text, v_date, (int) sizeof from_text);
  icon_at (ic + 8 * SB_TO_LABEL, 480, -380, 540, -336, 0x17000119u, t_to, NULL, (int) sizeof t_to);
  icon_at (ic + 8 * SB_TO, 548, -384, 788, -332, T_WRITE, to_text, v_date, (int) sizeof to_text);
  icon_at (ic + 8 * SB_DATES_NOTE, 232, -432, W - 24, -388, T_NOTE, t_dnote, NULL, (int) sizeof t_dnote);
  icon_at (ic + 8 * SB_NOTE, 24, -500, W - 24, -456, T_NOTE, note, NULL, (int) sizeof note);
  icon_at (ic + 8 * SB_CANCEL, W - 384, -580, W - 220, -524, T_BUTTON, t_cancel, v_cancel, (int) sizeof t_cancel);
  icon_at (ic + 8 * SB_OK, W - 200, -584, W - 24, -520, T_BUTTON, ok_text, v_ok, (int) sizeof ok_text);
  return _swix (Wimp_CreateWindow, _IN (1) | _OUT (0), b, &win) ? -1 : 0;
}

static void set_on (int i, int on)
{
  int s[4];
  s[0] = win; s[1] = i;
  s[2] = on ? (1 << 21) : 0;
  s[3] = 1 << 21;
  _swix (Wimp_SetIconState, _IN (1), s);
}

static int is_on (int i)
{
  int s[10];
  s[0] = win; s[1] = i;
  if (_swix (Wimp_GetIconState, _IN (1), s))
    return 0;
  return (s[6] & (1 << 21)) != 0;
}

static void redraw_icon (int i)
{
  set_on (i, is_on (i));
}

static void close_win (void)
{
  int b[1];
  b[0] = win;
  if (is_open)
    _swix (Wimp_CloseWindow, _IN (1), b);
  is_open = 0;
}

/* Opens the window: album 0 for a new smart album (from the Browser's
   search, if there is one), or an album to change. */
void smartbox_open (cat_id album, int x, int y)
{
  cat_rule r;
  int s[9], k;
  memset (&r, 0, sizeof r);
  editing = 0;
  name_text[0] = '\0';
  if (album && cat_smart_get (lib_cat (app_library ()), album, &r) == 1)
    {
      editing = album;
      cat_group_name (lib_cat (app_library ()), SRC_ALBUM, album, name_text, sizeof name_text);
    }
  else
    snprintf (r.words, sizeof r.words, "%s", browser_source ()->search);
  snprintf (words_text, sizeof words_text, "%s", r.words);
  snprintf (from_text, sizeof from_text, "%s", r.from);
  snprintf (to_text, sizeof to_text, "%s", r.to);
  snprintf (ok_text, sizeof ok_text, "%s", editing ? "Save" : "Create");
  note[0] = '\0';
  for (k = 0; k < 6; k++)
    set_on (SB_R0 + k, r.min_rating == k);
  set_on (SB_FLAGGED, r.flagged);
  set_on (SB_EDITED, r.edited);
  redraw_icon (SB_NAME);
  redraw_icon (SB_WORDS);
  redraw_icon (SB_FROM);
  redraw_icon (SB_TO);
  redraw_icon (SB_NOTE);
  redraw_icon (SB_OK);
  s[0] = win;
  if (_swix (Wimp_GetWindowState, _IN (1), s))
    return;
  if (!is_open)
    {
      s[1] = x - 64; s[3] = s[1] + W;
      s[4] = y; s[2] = y - H;
      if (s[2] < 96)
        {
          s[2] = 96;
          s[4] = 96 + H;
        }
      s[5] = 0; s[6] = 0;
    }
  s[7] = -1;
  _swix (Wimp_OpenWindow, _IN (1), s);
  is_open = 1;
  _swix (Wimp_SetCaretPosition, _INR (0, 5), win, SB_NAME, 0, 0, -1, (int) strlen (name_text));
}

static void say (const char *msg, int icon)
{
  snprintf (note, sizeof note, "%s", msg);
  redraw_icon (SB_NOTE);
  if (icon >= 0)
    _swix (Wimp_SetCaretPosition, _INR (0, 5), win, icon, 0, 0, -1, (int) strlen (icon == SB_NAME ? name_text
                                                                                   : icon == SB_FROM ? from_text
                                                                                   : to_text));
}

static void ok (void)
{
  cat *c = lib_cat (app_library ());
  cat_rule r;
  char name[40], *p = name_text;
  size_t l;
  int k;
  memset (&r, 0, sizeof r);
  while (*p == ' ')
    p++;
  snprintf (name, sizeof name, "%s", p);
  l = strlen (name);
  while (l && (unsigned char) name[l - 1] <= ' ')
    name[--l] = '\0';
  if (!name[0])
    {
      say ("Give the album a name.", SB_NAME);
      return;
    }
  if (cat_date_normal (from_text, 0, r.from))
    {
      say ("Lumen can't read that date: try 2026-06-16, 2026-06 or 16/6/2026.", SB_FROM);
      return;
    }
  if (cat_date_normal (to_text, 1, r.to))
    {
      say ("Lumen can't read that date: try 2026-06-16, 2026-06 or 16/6/2026.", SB_TO);
      return;
    }
  snprintf (r.words, sizeof r.words, "%s", words_text);
  for (k = 0; k < 6; k++)
    if (is_on (SB_R0 + k))
      r.min_rating = k;
  r.flagged = is_on (SB_FLAGGED);
  r.edited = is_on (SB_EDITED);
  if (editing)
    {
      char old[64];
      cat_group_name (c, SRC_ALBUM, editing, old, sizeof old);
      if ((strcmp (old, name) && cat_group_rename (c, SRC_ALBUM, editing, name)) || cat_smart_set (c, editing, &r))
        {
          app_report (cat_error (c));
          return;
        }
    }
  else
    {
      cat_id a = cat_smart_create (c, name, &r);
      cat_source s = *browser_source ();
      if (a <= 0)
        {
          app_report (cat_error (c));
          return;
        }
      /* Show it, without the search it was made from (it is in the rule now). */
      s.kind = SRC_ALBUM;
      s.id = a;
      s.search[0] = '\0';
      mainwin_set_search ("");
      browser_set_source (&s);
      editing = a;
      if (mainwin_mode () != LAYOUT_BROWSER)
        {
          /* The Viewer moves to the album's first photo; none: back to the Browser. */
          if (browser_count () > 0)
            browser_focus_on (0);
          else
            mainwin_set_mode (LAYOUT_BROWSER);
        }
    }
  close_win ();
  mainwin_library_changed ();
  if (browser_source ()->kind == SRC_ALBUM && browser_source ()->id == editing)
    browser_after_change ();
}

void smartbox_click (int *b)
{
  if (b[2] == Button_Menu)
    return;
  switch (b[4])
    {
    case SB_CANCEL: close_win (); break;
    case SB_OK: ok (); break;
    }
}

int smartbox_key (int key)
{
  if (key == KEY_RETURN)
    {
      ok ();
      return 1;
    }
  if (key == KEY_ESCAPE)
    {
      close_win ();
      return 1;
    }
  return 0;
}

void smartbox_closed (void)
{
  is_open = 0;
}

/* For the tests: fills in the window as if typed. */
void smartbox_set_texts (const char *name, const char *words, const char *from, const char *to)
{
  snprintf (name_text, sizeof name_text, "%s", name);
  snprintf (words_text, sizeof words_text, "%s", words);
  snprintf (from_text, sizeof from_text, "%s", from);
  snprintf (to_text, sizeof to_text, "%s", to);
}
