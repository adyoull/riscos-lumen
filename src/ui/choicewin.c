/* choicewin.c: the Choices window (icon bar menu > Choices...). What it
   sets is kept by app.c in <Choices$Write>.Lumen.Choices (prefs.h):
   where the library is (from the next start), the Export box's starting
   format, size and quality, the thumbnail size Lumen starts with, and
   whether originals are checked for when Lumen opens.

   Icon flags as in exportbox.c (classic PRM): radio buttons are type 11
   in an exclusive selection group (bits 16-20); an option button is a
   radio button in no group, which the Wimp turns on and off. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <kernel.h>
#include <swis.h>
#include "export.h"
#include "ui.h"

enum
{
  C_LIB_LABEL, C_LIB, C_LIB_NOTE, C_EXPORT_LABEL, C_JPEG, C_PNG, C_QUALITY_LABEL, C_QUALITY, C_SIZE_LABEL,
  C_FULL, C_3840, C_2048, C_1024, C_THUMB_LABEL, C_SMALL, C_MEDIUM, C_LARGE, C_VERIFY, C_CANCEL, C_SAVE,
  C_FOLDERS_LABEL, C_WATCH_NEW, C_ALBUMS_NEW, C_EVERY, C_MINUTES, C_MINUTES_LABEL,
  C_COUNT
};
static const int size_choice[4] = { 0, 3840, 2048, 1024 };

#define W 1120
#define H 690
#define T_LABEL   0x17000311u          /* text, vcentred, indirected, right-justified */
#define T_NOTE    0x17000111u          /* text, vcentred, indirected */
#define T_WRITE   0x0700F135u          /* writable, white */
#define T_RADIO   0x1700B113u          /* text+sprite, vcentred, indirected, radio */
#define T_BUTTON  0x1700313Du

static int win = -1;
static char lib_text[512], quality_text[4], minutes_text[6];

int choicewin_window (void) { return win; }

static void icon_at (int *ic, int x0, int y0, int x1, int y1, unsigned flags, char *text, const char *valid, int size)
{
  ic[0] = x0; ic[1] = y0; ic[2] = x1; ic[3] = y1;
  ic[4] = (int) flags;
  ic[5] = (int) text; ic[6] = valid ? (int) valid : -1; ic[7] = size;
}

#define LABEL(i, y, t) icon_at (ic + 8 * (i), 16, (y) - 44, 220, (y), T_LABEL, (char *) (t), NULL, (int) sizeof (t))

int choicewin_create (void)
{
  static const char v_radio[] = "Sradiooff,radioon", v_option[] = "Soptoff,opton", v_write[] = "Pptr_write";
  static const char v_num[] = "A0-9;Pptr_write", v_save[] = "R6,3", v_cancel[] = "R5,3";
  static char t_lib[] = "Library", t_note[] = "Used from the next start. Empty: the usual place.";
  static char t_export[] = "Export", t_jpeg[] = "JPEG", t_png[] = "PNG", t_quality[] = "Quality", t_size[] = "Size";
  static char t_full[] = "Full", t_3840[] = "3840", t_2048[] = "2048", t_1024[] = "1024", t_thumb[] = "Thumbnails";
  static char t_small[] = "Small", t_medium[] = "Medium", t_large[] = "Large";
  static char t_verify[] = "Check the photos' files are still there when Lumen opens";
  static char t_cancel[] = "Cancel", t_savebtn[] = "Save";
  int b[22 + 8 * C_COUNT];
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
  memcpy (&b[18], "Choices", 8);        /* the title: 12 bytes at most, not indirected */
  b[21] = C_COUNT;
  LABEL (C_LIB_LABEL, -20, t_lib);
  icon_at (ic + 8 * C_LIB, 240, -72, W - 24, -20, T_WRITE, lib_text, v_write, (int) sizeof lib_text);
  icon_at (ic + 8 * C_LIB_NOTE, 240, -124, W - 24, -80, T_NOTE, t_note, NULL, (int) sizeof t_note);
  LABEL (C_EXPORT_LABEL, -150, t_export);
  icon_at (ic + 8 * C_JPEG, 240, -194, 420, -150, T_RADIO | (1u << 16), t_jpeg, v_radio, (int) sizeof t_jpeg);
  icon_at (ic + 8 * C_PNG, 440, -194, 620, -150, T_RADIO | (1u << 16), t_png, v_radio, (int) sizeof t_png);
  icon_at (ic + 8 * C_QUALITY_LABEL, 640, -194, 800, -150, T_LABEL, t_quality, NULL, (int) sizeof t_quality);
  icon_at (ic + 8 * C_QUALITY, 820, -198, 920, -146, T_WRITE, quality_text, v_num, (int) sizeof quality_text);
  LABEL (C_SIZE_LABEL, -214, t_size);
  icon_at (ic + 8 * C_FULL, 240, -258, 370, -214, T_RADIO | (2u << 16), t_full, v_radio, (int) sizeof t_full);
  icon_at (ic + 8 * C_3840, 390, -258, 520, -214, T_RADIO | (2u << 16), t_3840, v_radio, (int) sizeof t_3840);
  icon_at (ic + 8 * C_2048, 540, -258, 670, -214, T_RADIO | (2u << 16), t_2048, v_radio, (int) sizeof t_2048);
  icon_at (ic + 8 * C_1024, 690, -258, 820, -214, T_RADIO | (2u << 16), t_1024, v_radio, (int) sizeof t_1024);
  LABEL (C_THUMB_LABEL, -290, t_thumb);
  icon_at (ic + 8 * C_SMALL, 240, -334, 400, -290, T_RADIO | (3u << 16), t_small, v_radio, (int) sizeof t_small);
  icon_at (ic + 8 * C_MEDIUM, 420, -334, 600, -290, T_RADIO | (3u << 16), t_medium, v_radio, (int) sizeof t_medium);
  icon_at (ic + 8 * C_LARGE, 620, -334, 780, -290, T_RADIO | (3u << 16), t_large, v_radio, (int) sizeof t_large);
  icon_at (ic + 8 * C_VERIFY, 240, -410, W - 24, -366, T_RADIO, t_verify, v_option, (int) sizeof t_verify);
  {
    static char t_folders[] = "Folders", t_watch[] = "Watch folders I import for new photos";
    static char t_albums[] = "Make albums from the folders inside them", t_every[] = "Look in watched folders every";
    static char t_minutes[] = "minutes";
    LABEL (C_FOLDERS_LABEL, -440, t_folders);
    icon_at (ic + 8 * C_WATCH_NEW, 240, -484, W - 24, -440, T_RADIO, t_watch, v_option, (int) sizeof t_watch);
    icon_at (ic + 8 * C_ALBUMS_NEW, 240, -534, W - 24, -490, T_RADIO, t_albums, v_option, (int) sizeof t_albums);
    icon_at (ic + 8 * C_EVERY, 240, -588, 640, -544, T_NOTE, t_every, NULL, (int) sizeof t_every);
    icon_at (ic + 8 * C_MINUTES, 650, -592, 750, -540, T_WRITE, minutes_text, v_num, (int) sizeof minutes_text);
    icon_at (ic + 8 * C_MINUTES_LABEL, 760, -588, 960, -544, T_NOTE, t_minutes, NULL, (int) sizeof t_minutes);
  }
  icon_at (ic + 8 * C_CANCEL, W - 384, -666, W - 220, -610, T_BUTTON, t_cancel, v_cancel, (int) sizeof t_cancel);
  icon_at (ic + 8 * C_SAVE, W - 200, -670, W - 24, -606, T_BUTTON, t_savebtn, v_save, (int) sizeof t_savebtn);
  return _swix (Wimp_CreateWindow, _IN (1) | _OUT (0), b, &win) ? -1 : 0;
}

static void set_on (int i, int on)
{
  int s[4];
  s[0] = win; s[1] = i;
  s[2] = on ? (1 << 21) : 0;           /* EOR word: set bit 21 (selected) to on */
  s[3] = 1 << 21;                      /* clear word: bit 21 */
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

/* The window shows the Choices as saved. */
static void show (const prefs *p)
{
  int k;
  snprintf (lib_text, sizeof lib_text, "%s", p->library);
  snprintf (quality_text, sizeof quality_text, "%d", p->export_quality);
  set_on (C_JPEG, p->export_format == EXPORT_JPEG);
  set_on (C_PNG, p->export_format == EXPORT_PNG);
  for (k = 0; k < 4; k++)
    set_on (C_FULL + k, p->export_size == size_choice[k]);
  for (k = 0; k < 3; k++)
    set_on (C_SMALL + k, p->thumb_size == k);
  set_on (C_VERIFY, p->verify);
  set_on (C_WATCH_NEW, p->watch_new);
  set_on (C_ALBUMS_NEW, p->albums_new);
  snprintf (minutes_text, sizeof minutes_text, "%d", p->watch_minutes);
  set_on (C_MINUTES, 0);
  set_on (C_LIB, 0);                   /* redraws the text fields */
  set_on (C_QUALITY, 0);
}

void choicewin_open (void)
{
  int s[9], xe = 1, ye = 1, sw = 0, sh = 0;
  show (app_prefs ());
  _swix (OS_ReadModeVariable, _INR (0, 1) | _OUT (2), -1, 4, &xe);
  _swix (OS_ReadModeVariable, _INR (0, 1) | _OUT (2), -1, 5, &ye);
  _swix (OS_ReadModeVariable, _INR (0, 1) | _OUT (2), -1, 11, &sw);
  _swix (OS_ReadModeVariable, _INR (0, 1) | _OUT (2), -1, 12, &sh);
  s[0] = win;
  if (_swix (Wimp_GetWindowState, _IN (1), s))
    return;
  if (!(s[8] & (1 << 16)))
    {
      int cx = ((sw + 1) << xe) / 2, cy = ((sh + 1) << ye) / 2;
      s[1] = cx - W / 2; s[2] = cy - H / 2; s[3] = cx + W / 2; s[4] = cy + H / 2;
      s[5] = 0; s[6] = 0;
    }
  s[7] = -1;
  _swix (Wimp_OpenWindow, _IN (1), s);
  _swix (Wimp_SetCaretPosition, _INR (0, 5), win, C_LIB, 0, 0, -1, (int) strlen (lib_text));
}

static void close_win (void)
{
  int b[1];
  b[0] = win;
  _swix (Wimp_CloseWindow, _IN (1), b);
}

static void save (void)
{
  prefs p = *app_prefs ();
  int k, q = atoi (quality_text);
  char *s = lib_text;
  size_t l;
  while (*s == ' ')
    s++;
  l = strlen (s);
  while (l && (unsigned char) s[l - 1] <= ' ')
    s[--l] = '\0';
  snprintf (p.library, sizeof p.library, "%s", s);
  p.export_format = is_on (C_PNG) ? EXPORT_PNG : EXPORT_JPEG;
  p.export_quality = q < 1 ? 1 : q > 100 ? 100 : q;
  for (k = 0; k < 4; k++)
    if (is_on (C_FULL + k))
      p.export_size = size_choice[k];
  for (k = 0; k < 3; k++)
    if (is_on (C_SMALL + k))
      p.thumb_size = k;
  p.verify = is_on (C_VERIFY);
  p.watch_new = is_on (C_WATCH_NEW);
  p.albums_new = is_on (C_ALBUMS_NEW);
  {
    int mins = atoi (minutes_text);
    p.watch_minutes = mins < 0 ? 0 : mins > 1440 ? 1440 : mins;
  }
  if (app_set_prefs (&p) == 0)
    close_win ();
}

void choicewin_click (int *b)
{
  if (b[2] == Button_Menu)
    return;
  if (b[4] == C_SAVE)
    save ();
  else if (b[4] == C_CANCEL)
    {
      if (b[2] == Button_Adjust)
        show (app_prefs ());           /* Adjust: back to as saved, still open */
      else
        close_win ();
    }
}

int choicewin_key (int key)
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

/* A directory dropped on the window: the library to use. */
void choicewin_drop (const char *path)
{
  snprintf (lib_text, sizeof lib_text, "%s", path);
  set_on (C_LIB, 0);
  _swix (Wimp_SetCaretPosition, _INR (0, 5), win, C_LIB, 0, 0, -1, (int) strlen (lib_text));
}

/* For the tests. */
const char *choicewin_library_text (void) { return lib_text; }
void choicewin_set_library_text (const char *s) { snprintf (lib_text, sizeof lib_text, "%s", s); }
