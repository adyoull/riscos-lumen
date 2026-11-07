/* Copyright (c) 2026 Andrew Youll
   Released under the MIT licence: see the LICENSE file.
   SPDX-License-Identifier: MIT */
/* choicewin.c: the Choices window (icon bar menu > Choices...), in three
   tabs. What it sets is kept by app.c in <Choices$Write>.Lumen.Choices
   (prefs.h). Library: where the library is (from the next start), the
   thumbnail size Lumen starts with, whether originals are checked for
   when Lumen opens, and watched folders. Export: the Export box's
   starting format, size and quality. Previews and scratch: which photos
   get previews (library.h), and the scratch directory where plug-ins'
   full-size pictures are kept (rawjob's cache) and how big it may grow.

   A tab shows its own icons; the others' are moved below the work area
   (Wimp_ResizeIcon), out of sight.

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
  C_PREVIEWS_LABEL, C_PV_RAW, C_PV_ALL, C_PV_OFF, C_PV_DISCARD, C_TIFF,
  /* 0.0.38.6: tabs, and the scratch directory */
  C_TAB_LIB, C_TAB_EXPORT, C_TAB_CACHE,
  C_SCRATCH_LABEL, C_SCRATCH, C_SCRATCH_NOTE, C_SCRATCH_SIZE_LABEL, C_SCRATCH_MB, C_SCRATCH_MB_LABEL,
  C_SCRATCH_FITS, C_SCRATCH_DISCARD,
  C_COUNT
};
static const int size_choice[4] = { 0, 3840, 2048, 1024 };

/* The tabs: what each shows (the rest of the window's icons are moved
   out of sight, below its work area). */
enum { CT_LIBRARY, CT_EXPORT, CT_CACHE, TABS };
#define CT_ALWAYS (-1)

#define W 1120
#define H 670
#define T_LABEL   0x17000311u          /* text, vcentred, indirected, right-justified */
#define T_NOTE    0x17000111u          /* text, vcentred, indirected */
#define T_WRITE   0x0700F135u          /* writable, white */
#define T_RADIO   0x1700B113u          /* text+sprite, vcentred, indirected, radio */
#define T_BUTTON  0x1700313Du
#define T_TAB     0x00003000u          /* only somewhere to click: choicewin_redraw draws the tabs */
#define PAGE_TOP  (-76)                /* the tabs stand on the page, which runs down to here: */
#define PAGE_BOTTOM (-H + 104)
#define AWAY      4000                 /* how far below a hidden icon goes */

static int win = -1, tab = CT_LIBRARY;
static char lib_text[512], quality_text[4], minutes_text[6];
static char scratch_text[512], mb_text[7], fits_text[100];
static int home[C_COUNT][4], tab_of[C_COUNT];

int choicewin_window (void) { return win; }
int choicewin_tab (void) { return tab; }

static void icon_at (int *ic, int x0, int y0, int x1, int y1, unsigned flags, char *text, const char *valid, int size)
{
  ic[0] = x0; ic[1] = y0; ic[2] = x1; ic[3] = y1;
  ic[4] = (int) flags;
  ic[5] = (int) text; ic[6] = valid ? (int) valid : -1; ic[7] = size;
}

/* Icon i on tab t (CT_ALWAYS: whichever is showing), at its place. */
static int *icons;
static void place (int i, int t, int x0, int y0, int x1, int y1, unsigned flags, char *text, const char *valid, int size)
{
  home[i][0] = x0; home[i][1] = y0; home[i][2] = x1; home[i][3] = y1;
  tab_of[i] = t;
  if (t != CT_ALWAYS && t != tab)
    y0 -= AWAY, y1 -= AWAY;
  icon_at (icons + 8 * i, x0, y0, x1, y1, flags, text, valid, size);
}

#define LABEL(i, t, y, s) place ((i), (t), 16, (y) - 44, 220, (y), T_LABEL, (char *) (s), NULL, (int) sizeof (s))

int choicewin_create (void)
{
  static const char v_radio[] = "Sradiooff,radioon", v_option[] = "Soptoff,opton", v_write[] = "Pptr_write";
  static const char v_num[] = "A0-9;Pptr_write", v_save[] = "R6,3", v_cancel[] = "R5,3", v_tab[] = "R2";
  static const char v_discard[] = "R5,3";
  static char t_lib[] = "Library", t_note[] = "Used from the next start. Empty: the usual place.";
  static char t_export[] = "Export", t_jpeg[] = "JPEG", t_png[] = "PNG", t_tiff[] = "TIFF", t_quality[] = "Quality", t_size[] = "Size";
  static char t_full[] = "Full", t_3840[] = "3840", t_2048[] = "2048", t_1024[] = "1024", t_thumb[] = "Thumbnails";
  static char t_small[] = "Small", t_medium[] = "Medium", t_large[] = "Large";
  static char t_verify[] = "Check the photos' files are still there when Lumen opens";
  static char t_cancel[] = "Cancel", t_savebtn[] = "Save";
  static char t_folders[] = "Folders", t_watch[] = "Watch folders I import for new photos";
  static char t_albums[] = "Make albums from the folders inside them", t_every[] = "Look in watched folders every";
  static char t_minutes[] = "minutes";
  static char t_previews[] = "Previews", t_pv_raw[] = "Raw files", t_pv_all[] = "All photos", t_pv_off[] = "None";
  static char t_discard[] = "Discard", t_discard2[] = "Discard";
  static char t_tab_lib[] = "Library", t_tab_export[] = "Export", t_tab_cache[] = "Previews and scratch";
  static char t_scratch[] = "Scratch", t_scratch_note[] = "Raw photos are kept here at full size. Empty: none.";
  static char t_scratch_size[] = "Up to", t_mb[] = "MB";
  int b[22 + 8 * C_COUNT];
  icons = &b[22];
  memset (b, 0, sizeof b);
  b[0] = 0; b[1] = 0; b[2] = W; b[3] = H;
  b[6] = -1;
  b[7] = (int) 0x86000002;             /* moveable, close icon, title, new format; drawn by Lumen (the tabs) */
  ((unsigned char *) b)[32] = 7;  ((unsigned char *) b)[33] = 2;
  ((unsigned char *) b)[34] = 7;  ((unsigned char *) b)[35] = 1;
  ((unsigned char *) b)[36] = 3;  ((unsigned char *) b)[37] = 1;
  ((unsigned char *) b)[38] = 12;
  b[10] = 0; b[11] = -H; b[12] = W; b[13] = 0;
  b[14] = 0x3D;
  b[16] = 1;
  memcpy (&b[18], "Choices", 8);        /* the title: 12 bytes at most, not indirected */
  b[21] = C_COUNT;
  /* The tabs. */
  place (C_TAB_LIB, CT_ALWAYS, 24, PAGE_TOP, 224, -18, T_TAB, t_tab_lib, v_tab, (int) sizeof t_tab_lib);
  place (C_TAB_EXPORT, CT_ALWAYS, 224, PAGE_TOP, 424, -18, T_TAB, t_tab_export, v_tab, (int) sizeof t_tab_export);
  place (C_TAB_CACHE, CT_ALWAYS, 424, PAGE_TOP, 754, -18, T_TAB, t_tab_cache, v_tab, (int) sizeof t_tab_cache);
  /* Library: where it is, thumbnails, checking, folders. */
  LABEL (C_LIB_LABEL, CT_LIBRARY, -110, t_lib);
  place (C_LIB, CT_LIBRARY, 240, -162, W - 24, -110, T_WRITE, lib_text, v_write, (int) sizeof lib_text);
  place (C_LIB_NOTE, CT_LIBRARY, 240, -214, W - 24, -170, T_NOTE, t_note, NULL, (int) sizeof t_note);
  LABEL (C_THUMB_LABEL, CT_LIBRARY, -240, t_thumb);
  place (C_SMALL, CT_LIBRARY, 240, -284, 400, -240, T_RADIO | (3u << 16), t_small, v_radio, (int) sizeof t_small);
  place (C_MEDIUM, CT_LIBRARY, 420, -284, 600, -240, T_RADIO | (3u << 16), t_medium, v_radio, (int) sizeof t_medium);
  place (C_LARGE, CT_LIBRARY, 620, -284, 780, -240, T_RADIO | (3u << 16), t_large, v_radio, (int) sizeof t_large);
  place (C_VERIFY, CT_LIBRARY, 240, -348, W - 24, -304, T_RADIO, t_verify, v_option, (int) sizeof t_verify);
  LABEL (C_FOLDERS_LABEL, CT_LIBRARY, -378, t_folders);
  place (C_WATCH_NEW, CT_LIBRARY, 240, -422, W - 24, -378, T_RADIO, t_watch, v_option, (int) sizeof t_watch);
  place (C_ALBUMS_NEW, CT_LIBRARY, 240, -472, W - 24, -428, T_RADIO, t_albums, v_option, (int) sizeof t_albums);
  place (C_EVERY, CT_LIBRARY, 240, -526, 640, -482, T_NOTE, t_every, NULL, (int) sizeof t_every);
  place (C_MINUTES, CT_LIBRARY, 650, -530, 750, -478, T_WRITE, minutes_text, v_num, (int) sizeof minutes_text);
  place (C_MINUTES_LABEL, CT_LIBRARY, 760, -526, 960, -482, T_NOTE, t_minutes, NULL, (int) sizeof t_minutes);
  /* Export: what the Export box starts with. */
  LABEL (C_EXPORT_LABEL, CT_EXPORT, -110, t_export);
  place (C_JPEG, CT_EXPORT, 240, -154, 390, -110, T_RADIO | (1u << 16), t_jpeg, v_radio, (int) sizeof t_jpeg);
  place (C_PNG, CT_EXPORT, 400, -154, 540, -110, T_RADIO | (1u << 16), t_png, v_radio, (int) sizeof t_png);
  place (C_TIFF, CT_EXPORT, 550, -154, 690, -110, T_RADIO | (1u << 16), t_tiff, v_radio, (int) sizeof t_tiff);
  place (C_QUALITY_LABEL, CT_EXPORT, 710, -154, 860, -110, T_LABEL, t_quality, NULL, (int) sizeof t_quality);
  place (C_QUALITY, CT_EXPORT, 880, -158, 980, -106, T_WRITE, quality_text, v_num, (int) sizeof quality_text);
  LABEL (C_SIZE_LABEL, CT_EXPORT, -180, t_size);
  place (C_FULL, CT_EXPORT, 240, -224, 370, -180, T_RADIO | (2u << 16), t_full, v_radio, (int) sizeof t_full);
  place (C_3840, CT_EXPORT, 390, -224, 520, -180, T_RADIO | (2u << 16), t_3840, v_radio, (int) sizeof t_3840);
  place (C_2048, CT_EXPORT, 540, -224, 670, -180, T_RADIO | (2u << 16), t_2048, v_radio, (int) sizeof t_2048);
  place (C_1024, CT_EXPORT, 690, -224, 820, -180, T_RADIO | (2u << 16), t_1024, v_radio, (int) sizeof t_1024);
  /* Previews and scratch. */
  LABEL (C_PREVIEWS_LABEL, CT_CACHE, -110, t_previews);
  place (C_PV_RAW, CT_CACHE, 240, -154, 430, -110, T_RADIO | (4u << 16), t_pv_raw, v_radio, (int) sizeof t_pv_raw);
  place (C_PV_ALL, CT_CACHE, 450, -154, 650, -110, T_RADIO | (4u << 16), t_pv_all, v_radio, (int) sizeof t_pv_all);
  place (C_PV_OFF, CT_CACHE, 670, -154, 800, -110, T_RADIO | (4u << 16), t_pv_off, v_radio, (int) sizeof t_pv_off);
  place (C_PV_DISCARD, CT_CACHE, W - 200, -158, W - 24, -106, T_BUTTON, t_discard, v_discard, (int) sizeof t_discard);
  LABEL (C_SCRATCH_LABEL, CT_CACHE, -200, t_scratch);
  place (C_SCRATCH, CT_CACHE, 240, -252, W - 24, -200, T_WRITE, scratch_text, v_write, (int) sizeof scratch_text);
  place (C_SCRATCH_NOTE, CT_CACHE, 240, -304, W - 24, -260, T_NOTE, t_scratch_note, NULL, (int) sizeof t_scratch_note);
  LABEL (C_SCRATCH_SIZE_LABEL, CT_CACHE, -324, t_scratch_size);
  place (C_SCRATCH_MB, CT_CACHE, 240, -372, 380, -320, T_WRITE, mb_text, v_num, (int) sizeof mb_text);
  place (C_SCRATCH_MB_LABEL, CT_CACHE, 390, -368, 460, -324, T_NOTE, t_mb, NULL, (int) sizeof t_mb);
  place (C_SCRATCH_DISCARD, CT_CACHE, W - 200, -372, W - 24, -320, T_BUTTON, t_discard2, v_discard, (int) sizeof t_discard2);
  place (C_SCRATCH_FITS, CT_CACHE, 240, -426, W - 24, -382, T_NOTE, fits_text, NULL, (int) sizeof fits_text);
  place (C_CANCEL, CT_ALWAYS, W - 384, -H + 24, W - 220, -H + 80, T_BUTTON, t_cancel, v_cancel, (int) sizeof t_cancel);
  place (C_SAVE, CT_ALWAYS, W - 200, -H + 20, W - 24, -H + 84, T_BUTTON, t_savebtn, v_save, (int) sizeof t_savebtn);
  if (_swix (Wimp_CreateWindow, _IN (1) | _OUT (0), b, &win))
    return -1;
  return 0;
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

/* How many photos the scratch size holds (12 MP: 36 MB each, as
   Lumen keeps them), and what it holds now. */
static void show_fits (void)
{
  int mb = atoi (mb_text), photos = 0;
  long long used = app_scratch_used (&photos);
  if (mb < 16)
    mb = 16;
  if (!scratch_text[0])
    snprintf (fits_text, sizeof fits_text, "%d MB holds about %d photos of 12 MP.", mb, mb / 36);
  else
    snprintf (fits_text, sizeof fits_text, "About %d photos of 12 MP. Holding %d now (%d MB).", mb / 36, photos,
              (int) ((used + 512 * 1024) / (1024 * 1024)));
  set_on (C_SCRATCH_FITS, 0);
}

/* The window shows the Choices as saved. */
static void show (const prefs *p)
{
  int k;
  snprintf (lib_text, sizeof lib_text, "%s", p->library);
  snprintf (quality_text, sizeof quality_text, "%d", p->export_quality);
  set_on (C_JPEG, p->export_format == EXPORT_JPEG);
  set_on (C_PNG, p->export_format == EXPORT_PNG);
  set_on (C_TIFF, p->export_format == EXPORT_TIFF);
  for (k = 0; k < 4; k++)
    set_on (C_FULL + k, p->export_size == size_choice[k]);
  for (k = 0; k < 3; k++)
    set_on (C_SMALL + k, p->thumb_size == k);
  set_on (C_PV_RAW, p->previews == 1);
  set_on (C_PV_ALL, p->previews == 2);
  set_on (C_PV_OFF, p->previews == 0);
  set_on (C_VERIFY, p->verify);
  set_on (C_WATCH_NEW, p->watch_new);
  set_on (C_ALBUMS_NEW, p->albums_new);
  snprintf (minutes_text, sizeof minutes_text, "%d", p->watch_minutes);
  set_on (C_MINUTES, 0);
  snprintf (scratch_text, sizeof scratch_text, "%s", p->scratch_dir);
  snprintf (mb_text, sizeof mb_text, "%d", p->scratch_mb);
  show_fits ();
  set_on (C_LIB, 0);                   /* redraws the text fields */
  set_on (C_QUALITY, 0);
  set_on (C_SCRATCH, 0);
  set_on (C_SCRATCH_MB, 0);
}

/* The tabs, as a notebook's: the page outlined below them, the chosen
   tab part of the page (open at its foot, on the window's own grey), the
   others behind it, a little lower and darker. */
#define COL_LINE 0x80808000u
#define COL_BEHIND 0xC4C4C400u
#define COL_TAB_TEXT 0x1A1A1A00u
#define COL_PAGE 0xDDDDDD00u
static void draw_tabs (int ox, int oy)
{
  static const char *const names[TABS] = { "Library", "Export", "Previews and scratch" };
  int k;
  for (k = 0; k < TABS; k++)
    {
      const int *r = home[C_TAB_LIB + k];
      int x0 = ox + r[0], x1 = ox + r[2], y0 = oy + PAGE_TOP, y1 = oy + r[3];
      if (k == tab)
        continue;
      y1 -= 8;
      draw_rect (x0 + 2, y0, x1 - 2, y1, COL_BEHIND);
      draw_line (x0 + 2, y0, x0 + 2, y1, COL_LINE);
      draw_line (x1 - 2, y0, x1 - 2, y1, COL_LINE);
      draw_line (x0 + 2, y1, x1 - 2, y1, COL_LINE);
      draw_text (x0 + (r[2] - r[0] - draw_text_width (names[k])) / 2, y0 + 14, names[k], COL_TAB_TEXT, COL_BEHIND);
    }
  {
    const int *r = home[C_TAB_LIB + tab];
    int x0 = ox + r[0], x1 = ox + r[2], y0 = oy + PAGE_TOP, y1 = oy + r[3];
    /* the page: round its edge, but not across the chosen tab's foot */
    draw_line (ox + 12, y0, x0, y0, COL_LINE);
    draw_line (x1, y0, ox + W - 12, y0, COL_LINE);
    draw_line (ox + 12, y0, ox + 12, oy + PAGE_BOTTOM, COL_LINE);
    draw_line (ox + W - 12, y0, ox + W - 12, oy + PAGE_BOTTOM, COL_LINE);
    draw_line (ox + 12, oy + PAGE_BOTTOM, ox + W - 12, oy + PAGE_BOTTOM, COL_LINE);
    draw_line (x0, y0, x0, y1, COL_LINE);
    draw_line (x1, y0, x1, y1, COL_LINE);
    draw_line (x0, y1, x1, y1, COL_LINE);
    draw_text (x0 + (r[2] - r[0] - draw_text_width (names[tab])) / 2, y0 + 18, names[tab], COL_TAB_TEXT, COL_PAGE);
  }
}

void choicewin_redraw (int *b)
{
  int more = 0;
  _swix (Wimp_RedrawWindow, _IN (1) | _OUT (0), b, &more);
  while (more)
    {
      draw_tabs (b[1] - b[5], b[4] - b[6]);
      _swix (Wimp_GetRectangle, _IN (1) | _OUT (0), b, &more);
    }
}

/* The caret in the tab's first field. */
static void caret_for_tab (void)
{
  int i = tab == CT_LIBRARY ? C_LIB : tab == CT_EXPORT ? C_QUALITY : C_SCRATCH;
  const char *t = i == C_LIB ? lib_text : i == C_QUALITY ? quality_text : scratch_text;
  _swix (Wimp_SetCaretPosition, _INR (0, 5), win, i, 0, 0, -1, (int) strlen (t));
}

void choicewin_show_tab (int t)
{
  int i;
  if (t < 0 || t >= TABS)
    return;
  tab = t;
  for (i = 0; i < C_COUNT; i++)
    if (tab_of[i] != CT_ALWAYS)
      {
        int away = tab_of[i] == tab ? 0 : AWAY;
        _swix (Wimp_ResizeIcon, _INR (0, 5), win, i, home[i][0], home[i][1] - away, home[i][2], home[i][3] - away);
      }
  if (tab == CT_CACHE)
    show_fits ();
  _swix (Wimp_ForceRedraw, _INR (0, 4), win, 0, -H, W, 0);
  caret_for_tab ();
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
  caret_for_tab ();
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
  p.export_format = is_on (C_PNG) ? EXPORT_PNG : is_on (C_TIFF) ? EXPORT_TIFF : EXPORT_JPEG;
  p.export_quality = q < 1 ? 1 : q > 100 ? 100 : q;
  for (k = 0; k < 4; k++)
    if (is_on (C_FULL + k))
      p.export_size = size_choice[k];
  for (k = 0; k < 3; k++)
    if (is_on (C_SMALL + k))
      p.thumb_size = k;
  p.previews = is_on (C_PV_ALL) ? 2 : is_on (C_PV_OFF) ? 0 : 1;
  p.verify = is_on (C_VERIFY);
  p.watch_new = is_on (C_WATCH_NEW);
  p.albums_new = is_on (C_ALBUMS_NEW);
  {
    int mins = atoi (minutes_text);
    p.watch_minutes = mins < 0 ? 0 : mins > 1440 ? 1440 : mins;
  }
  {
    char *d = scratch_text;
    int mb = atoi (mb_text);
    while (*d == ' ')
      d++;
    l = strlen (d);
    while (l && (unsigned char) d[l - 1] <= ' ')
      d[--l] = '\0';
    snprintf (p.scratch_dir, sizeof p.scratch_dir, "%s", d);
    p.scratch_mb = !mb_text[0] ? 2048 : mb < 16 ? 16 : mb > 65536 ? 65536 : mb;
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
  else if (b[4] >= C_TAB_LIB && b[4] < C_TAB_LIB + TABS)
    choicewin_show_tab (b[4] - C_TAB_LIB);
  else if (b[4] == C_PV_DISCARD)
    app_discard_previews ();
  else if (b[4] == C_SCRATCH_DISCARD)
    {
      app_discard_scratch ();
      show_fits ();
    }
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
  if (key == KEY_TAB_KEY || key == KEY_SHIFT_TAB)
    {
      /* Tab: the next tab (Shift: the one before). */
      choicewin_show_tab ((tab + (key == KEY_TAB_KEY ? 1 : TABS - 1)) % TABS);
      return 1;
    }
  if (key == KEY_ESCAPE)
    {
      close_win ();
      return 1;
    }
  return 0;
}

/* A directory dropped on the window: the scratch directory on that tab,
   else the library to use. */
void choicewin_drop (const char *path)
{
  if (tab == CT_CACHE)
    {
      snprintf (scratch_text, sizeof scratch_text, "%s", path);
      set_on (C_SCRATCH, 0);
      show_fits ();
    }
  else
    {
      if (tab != CT_LIBRARY)
        choicewin_show_tab (CT_LIBRARY);
      snprintf (lib_text, sizeof lib_text, "%s", path);
      set_on (C_LIB, 0);
    }
  caret_for_tab ();
}

/* For the tests. */
const char *choicewin_library_text (void) { return lib_text; }
void choicewin_set_library_text (const char *s) { snprintf (lib_text, sizeof lib_text, "%s", s); }
void choicewin_set_scratch_text (const char *dir, const char *mb)
{
  snprintf (scratch_text, sizeof scratch_text, "%s", dir);
  snprintf (mb_text, sizeof mb_text, "%s", mb);
}
