/* Copyright (c) 2026 Andrew Youll
   Released under the MIT licence: see the LICENSE file.
   SPDX-License-Identifier: MIT */
/* libbox.c: moving photos between libraries, and checking a library.

   Export library: a save box like the Export box (exportbox.c), with a
   directory to drag to a directory display (or a full pathname typed and
   Save clicked). It is opened from the icon bar menu (the whole library),
   from Selection on the window menu, or from a project's or album's menu
   in the Library tab. The drag follows the data transfer protocol (PRM
   p1005-1008): Message_DataSave with filetype &1000 to the window under
   the pointer, whose Message_DataSaveAck gives the full pathname for the
   new library. A program other than the Filer (scrap transfer) can't take
   a directory, so that is refused.

   Import library: a window to drag a library's directory into (or one
   dropped anywhere on Lumen). It shows what would come across before
   anything is changed, and whether to copy the photos in.

   Check library: a window showing the check as it runs, what it found,
   and a Repair button.

   One export or import runs at a time, in the background (libbox_idle),
   with its progress in the status area and the Activity window. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <kernel.h>
#include <swis.h>
#include "fsys.h"
#include "libxfer.h"
#include "libcheck.h"
#include "ui.h"

#define Message_DataSave 1

#define T_TEXT    0x17000111u          /* text, vcentred, indirected; black on grey */
#define T_WRITE   0x0700F135u          /* text, border, vcentred, filled, indirected, writable */
#define T_OPTION  0x1700B113u          /* text+sprite, vcentred, indirected; Select toggles it */
#define T_BUTTON  0x1700313Du          /* text, border, centred, vcentred, filled, indirected, click */
#define SHADED    (1u << 22)
#define SELECTED  (1u << 21)
#define LINE 100

/* --- Export library (the save box) */
enum { E_FILE, E_NAME, E_WHAT, E_ORIG, E_CANCEL, E_SAVE, E_COUNT };
#define EW 800
#define EH 404
static int ewin = -1, eopen, edragging, esave_ref;
static char ename[256] = "Library", ewhat[LINE], eorig[LINE], esprite[12] = "directory";
static cat_id *eids;                   /* the versions; eall: the whole library */
static int en, eall;

/* --- Import library */
enum { I_DIR, I_L1, I_L2, I_L3, I_COPY, I_CANCEL, I_IMPORT, I_COUNT };
#define IW 1000
#define IH 352
static int iwin = -1;
static char il[3][LINE], icopy[LINE];
static libxfer *plan;                  /* planned, waiting for Import */

/* --- Check library */
enum { C_L1, C_L2, C_L3, C_L4, C_L5, C_L6, C_CLOSE, C_REPAIR, C_COUNT };
#define CLINES 6
#define CW 1100
#define CH 420
static int cwin = -1;
static char cl[CLINES][LINE];
static libcheck *check;
static int check_done, repaired;

/* --- the job running */
static libxfer *xfer;

int libbox_export_window (void) { return ewin; }
int libbox_import_window (void) { return iwin; }
int libbox_check_window (void) { return cwin; }
int libbox_dragging (void) { return edragging; }

static void icon_at (int *ic, int x0, int y0, int x1, int y1, unsigned flags, const char *text,
                     const char *valid, int size)
{
  ic[0] = x0; ic[1] = y0; ic[2] = x1; ic[3] = y1;
  ic[4] = (int) flags;
  ic[5] = (int) text; ic[6] = valid ? (int) valid : -1; ic[7] = size;
}

static void window_block (int *b, int w, int h, const char *title, unsigned flags, int nicons)
{
  memset (b, 0, sizeof (int) * 22);
  b[0] = 0; b[1] = 0; b[2] = w; b[3] = h;
  b[6] = -1;
  b[7] = (int) flags;
  ((unsigned char *) b)[32] = 7;  ((unsigned char *) b)[33] = 2;
  ((unsigned char *) b)[34] = 7;  ((unsigned char *) b)[35] = 1;
  ((unsigned char *) b)[36] = 3;  ((unsigned char *) b)[37] = 1;
  ((unsigned char *) b)[38] = 12;
  b[10] = 0; b[11] = -h; b[12] = w; b[13] = 0;
  b[14] = 0x13D;                       /* title: text, border, centred, filled, indirected */
  b[16] = 1;
  b[18] = (int) title; b[19] = -1; b[20] = (int) strlen (title) + 1;
  b[21] = nicons;
}

int libbox_create (void)
{
  static const char v_name[] = "Pptr_write", v_option[] = "Soptoff,opton", v_save[] = "R6,3", v_cancel[] = "R5,3";
  int b[22 + 8 * 8], *ic = &b[22], k, y;

  /* Export library */
  memset (b, 0, sizeof b);
  window_block (b, EW, EH, "Export library", 0x84000012u, E_COUNT);
  icon_at (ic + 8 * E_FILE, EW / 2 - 60, -128, EW / 2 + 60, -20, 0x0000611Au, esprite, (const char *) 1, (int) sizeof esprite);
  icon_at (ic + 8 * E_NAME, 24, -192, EW - 24, -140, T_WRITE, ename, v_name, (int) sizeof ename);
  icon_at (ic + 8 * E_WHAT, 24, -252, EW - 24, -208, T_TEXT, ewhat, NULL, LINE);
  icon_at (ic + 8 * E_ORIG, 24, -312, EW - 24, -268, T_OPTION | SELECTED, eorig, v_option, LINE);
  icon_at (ic + 8 * E_CANCEL, EW - 400, -388, EW - 216, -328, T_BUTTON, "Cancel", v_cancel, 7);
  icon_at (ic + 8 * E_SAVE, EW - 200, -388, EW - 24, -328, T_BUTTON, "Save", v_save, 5);
  if (_swix (Wimp_CreateWindow, _IN (1) | _OUT (0), b, &ewin))
    return -1;

  /* Import library */
  memset (b, 0, sizeof b);
  window_block (b, IW, IH, "Import library", 0x86000012u, I_COUNT);
  {
    int *d = ic + 8 * I_DIR;
    d[0] = 24; d[1] = -136; d[2] = 120; d[3] = -24;
    d[4] = 0x1A;                       /* sprite, centred both ways */
    memcpy (&d[5], "directory", 10);
  }
  for (k = 0, y = -20; k < 3; k++, y -= 48)
    icon_at (ic + 8 * (I_L1 + k), 140, y - 44, IW - 24, y, T_TEXT, il[k], NULL, LINE);
  icon_at (ic + 8 * I_COPY, 140, -232, IW - 24, -188, T_OPTION | SELECTED, icopy, v_option, LINE);
  icon_at (ic + 8 * I_CANCEL, IW - 400, -328, IW - 216, -268, T_BUTTON, "Cancel", v_cancel, 7);
  icon_at (ic + 8 * I_IMPORT, IW - 200, -328, IW - 24, -268, T_BUTTON | SHADED, "Import", v_save, 7);
  if (_swix (Wimp_CreateWindow, _IN (1) | _OUT (0), b, &iwin))
    return -1;

  /* Check library */
  memset (b, 0, sizeof b);
  window_block (b, CW, CH, "Check library", 0x86000012u, C_COUNT);
  for (k = 0, y = -20; k < CLINES; k++, y -= 48)
    icon_at (ic + 8 * (C_L1 + k), 24, y - 44, CW - 24, y, T_TEXT, cl[k], NULL, LINE);
  icon_at (ic + 8 * C_CLOSE, CW - 400, -396, CW - 216, -336, T_BUTTON, "Close", v_cancel, 6);
  icon_at (ic + 8 * C_REPAIR, CW - 200, -396, CW - 24, -336, T_BUTTON | SHADED, "Repair", v_save, 7);
  return _swix (Wimp_CreateWindow, _IN (1) | _OUT (0), b, &cwin) ? -1 : 0;
}

/* --- small helpers */
static void redraw (int win, int icon)
{
  int st[4];
  st[0] = win; st[1] = icon; st[2] = 0; st[3] = 0;
  _swix (Wimp_SetIconState, _IN (1), st);
}

static void set_bit (int win, int icon, unsigned bit, int on)
{
  int st[4];
  st[0] = win; st[1] = icon;
  st[2] = on ? (int) bit : 0;
  st[3] = (int) bit;
  _swix (Wimp_SetIconState, _IN (1), st);
}

static int is_selected (int win, int icon)
{
  int st[10];
  st[0] = win; st[1] = icon;
  if (_swix (Wimp_GetIconState, _IN (1), st))
    return 0;
  return (st[6] & (int) SELECTED) != 0;
}

static void size_text (long long bytes, char *out, size_t n)
{
  if (bytes >= 1024LL * 1024 * 1024)
    snprintf (out, n, "%.1f GB", (double) bytes / (1024.0 * 1024 * 1024));
  else if (bytes >= 1024 * 1024)
    snprintf (out, n, "%.1f MB", (double) bytes / (1024.0 * 1024));
  else
    snprintf (out, n, "%lld KB", (bytes + 1023) / 1024);
}

/* Centred on the screen (if not open already), on top. */
static void open_centred (int win, int w, int h)
{
  int s[9], xe = 1, ye = 1, sw = 0, sh = 0;
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
      s[1] = cx - w / 2; s[2] = cy - h / 2; s[3] = cx + w / 2; s[4] = cy + h / 2;
      s[5] = 0; s[6] = 0;
    }
  s[7] = -1;
  _swix (Wimp_OpenWindow, _IN (1), s);
}

static void close_window (int win)
{
  int b[1];
  b[0] = win;
  _swix (Wimp_CloseWindow, _IN (1), b);
}

static int busy_already (void)
{
  if (xfer || (check && !check_done))
    {
      app_report ("Lumen is already exporting, importing or checking a library. Wait for it to finish "
                  "(the Activity window shows how far it has got).");
      return 1;
    }
  return 0;
}

/* Words into lines of at most `width` characters. */
static void wrap (const char *text, char (*lines)[LINE], int nlines, int width)
{
  int k;
  for (k = 0; k < nlines; k++)
    lines[k][0] = 0;
  for (k = 0; k < nlines && *text; k++)
    {
      int len = (int) strlen (text), cut = len;
      if (len > width)
        {
          cut = width;
          while (cut > 0 && text[cut] != ' ')
            cut--;
          if (cut == 0)
            cut = width;
        }
      if (k == nlines - 1 && len > cut)
        {
          snprintf (lines[k], LINE, "%.*s...", width - 3 < cut ? width - 3 : cut, text);
          break;
        }
      snprintf (lines[k], LINE, "%.*s", cut, text);
      text += cut;
      while (*text == ' ')
        text++;
    }
}

/* --- Export library */
void libbox_export_open (const cat_id *ids, int n, const char *what, const char *suggested, int x, int y)
{
  int photos = 0;
  long long bytes = 0;
  char sz[24];
  if (busy_already ())
    return;
  free (eids);
  eids = NULL;
  en = 0;
  eall = n < 0;
  if (!eall)
    {
      if (n == 0)
        return;
      if (!(eids = malloc (sizeof *eids * (size_t) n)))
        return;
      memcpy (eids, ids, sizeof *eids * (size_t) n);
      en = n;
    }
  libx_measure (app_library (), eids, eall ? -1 : en, &photos, &bytes);
  snprintf (ewhat, sizeof ewhat, "%s: %d photo%s", what, photos, photos == 1 ? "" : "s");
  size_text (bytes, sz, sizeof sz);
  snprintf (eorig, sizeof eorig, "Include the original photos (%s)", sz);
  {
    /* The name: letters, digits, - and _ (a leaf on any filing system). */
    size_t i = 0;
    const char *s = suggested && *suggested ? suggested : "Library";
    for (; *s && i + 1 < sizeof ename && i < 40; s++)
      ename[i++] = ((*s >= 'a' && *s <= 'z') || (*s >= 'A' && *s <= 'Z') || (*s >= '0' && *s <= '9')
                    || *s == '-' || *s == '_') ? *s : '_';
    ename[i] = 0;
  }
  eopen = 1;
  _swix (Wimp_CreateMenu, _INR (1, 3), ewin, x - 64, y);
  _swix (Wimp_SetCaretPosition, _INR (0, 5), ewin, E_NAME, 0, 0, -1, (int) strlen (ename));
}

static void export_close (void)
{
  if (eopen)
    _swix (Wimp_CreateMenu, _IN (1), -1);
  eopen = 0;
}

int libbox_export_to (const char *path)
{
  char err[400];
  libxfer *x;
  if (busy_already ())
    return -1;
  if (libx_export (&x, app_library (), path, eids, eall ? -1 : en, is_selected (ewin, E_ORIG), err, sizeof err))
    {
      app_report (err);
      return -1;
    }
  xfer = x;
  export_close ();
  return 0;
}

static int full_path (const char *s)
{
#ifdef __riscos__
  return strchr (s, '.') != NULL || strchr (s, ':') != NULL;
#else
  return strchr (s, '/') != NULL;
#endif
}

static void export_save_clicked (void)
{
  if (!full_path (ename))
    app_report ("To save, drag the directory to a directory display.");
  else
    libbox_export_to (ename);
}

/* The drag ended: offer the directory to whatever is under the pointer. */
void libbox_drag_end (void)
{
  int ptr[5], m[64];
  const char *leaf = fsys_leaf (ename);
  size_t len = strlen (leaf);
  edragging = 0;
  if (_swix (Wimp_GetPointerInfo, _IN (1), ptr) || ptr[3] == ewin)
    return;
  if (len > 200)
    len = 200;
  memset (m, 0, sizeof m);
  m[0] = (int) ((44 + len + 1 + 3) & ~3u);
  m[4] = Message_DataSave;
  m[5] = ptr[3]; m[6] = ptr[4]; m[7] = ptr[0]; m[8] = ptr[1];
  m[9] = 0;
  m[10] = FT_DIR;
  memcpy ((char *) &m[11], leaf, len);
  ((char *) &m[11])[len] = '\0';
  if (_swix (Wimp_SendMessage, _INR (0, 3), User_Message, m, ptr[3], ptr[4]) == NULL)
    esave_ref = m[2];
}

/* Message_DataSaveAck: 1 if it was ours. */
int libbox_save_ack (int *m)
{
  if (!esave_ref || m[3] != esave_ref)
    return 0;
  esave_ref = 0;
  if (m[9] == -1)
    {
      app_report ("A library is a directory: drag it to a directory display.");
      return 1;
    }
  libbox_export_to ((const char *) &m[11]);
  return 1;
}

static void export_click (int *b)
{
  switch (b[4])
    {
    case E_CANCEL: export_close (); break;
    case E_SAVE: export_save_clicked (); break;
    case E_FILE:
      if (b[2] == 64 || b[2] == 16)    /* a drag with Select or Adjust */
        {
          int st[9], db[10];
          st[0] = ewin;
          if (_swix (Wimp_GetWindowState, _IN (1), st))
            return;
          db[0] = ewin;
          db[1] = 5;                   /* Wimp_DragBox 5: a fixed size box, anywhere */
          db[2] = st[1] + EW / 2 - 60; db[3] = st[4] - 128;
          db[4] = st[1] + EW / 2 + 60; db[5] = st[4] - 20;
          db[6] = -0x10000000; db[7] = -0x10000000; db[8] = 0x10000000; db[9] = 0x10000000;
          if (_swix (Wimp_DragBox, _IN (1), db) == NULL)
            edragging = 1;
        }
      break;
    }
}

/* --- Import library */
static void import_lines_empty (void)
{
  snprintf (il[0], LINE, "Drag a Lumen library's directory into this window");
  snprintf (il[1], LINE, "to bring its photos into this library. Nothing changes");
  snprintf (il[2], LINE, "until you click Import.");
  snprintf (icopy, LINE, "Copy its photos into this library");
}

static void import_refresh (int ready)
{
  int k;
  for (k = 0; k < 3; k++)
    redraw (iwin, I_L1 + k);
  redraw (iwin, I_COPY);
  set_bit (iwin, I_IMPORT, SHADED, !ready);
}

void libbox_import_open (void)
{
  if (!plan)
    import_lines_empty ();
  open_centred (iwin, IW, IH);
  import_refresh (plan != NULL);
}

static void import_forget (void)
{
  libx_free (plan);
  plan = NULL;
}

void libbox_import_drop (const char *path)
{
  libx_survey s;
  char err[400], sz[24];
  libxfer *x;
  if (busy_already ())
    return;
  if (libx_import_plan (&x, app_library (), path, &s, err, sizeof err))
    {
      app_report (err);
      return;
    }
  import_forget ();
  plan = x;
  if (s.known == 0)
    snprintf (il[0], LINE, "From %s: %d photo%s, none of them here yet.", s.name, s.photos, s.photos == 1 ? "" : "s");
  else if (s.known == s.photos)
    snprintf (il[0], LINE, "From %s: %d photo%s, all already here.", s.name, s.photos, s.photos == 1 ? "" : "s");
  else
    snprintf (il[0], LINE, "From %s: %d photo%s, %d of them already here.", s.name, s.photos, s.photos == 1 ? "" : "s", s.known);
  {
    int len = snprintf (il[1], LINE, "%d version%s (%d new), %d project%s (%d new)",
                        s.versions, s.versions == 1 ? "" : "s", s.new_versions, s.projects, s.projects == 1 ? "" : "s",
                        s.new_projects);
    if (s.albums && len > 0 && len < LINE)
      len += snprintf (il[1] + len, (size_t) (LINE - len), ", %d album%s (%d new)", s.albums, s.albums == 1 ? "" : "s",
                       s.new_albums);
    if (len > 0 && len < LINE)
      snprintf (il[1] + len, (size_t) (LINE - len), ".");
  }
  if (s.missing)
    snprintf (il[2], LINE, "%d of its photos can't be found: they come in marked missing.", s.missing);
  else
    snprintf (il[2], LINE, "Folder albums and watching stay with that library.");
  size_text (s.bytes, sz, sizeof sz);
  snprintf (icopy, LINE, "Copy its photos into this library (%s)", sz);
  open_centred (iwin, IW, IH);
  import_refresh (s.new_versions > 0 || s.photos > s.known);
}

static void import_click (int *b)
{
  switch (b[4])
    {
    case I_CANCEL:
      import_forget ();
      close_window (iwin);
      break;
    case I_IMPORT:
      if (!plan || busy_already ())
        return;
      libx_import_go (plan, is_selected (iwin, I_COPY));
      xfer = plan;
      plan = NULL;
      close_window (iwin);
      break;
    }
}

/* --- Check library */
static void check_refresh (void)
{
  int k;
  for (k = 0; k < CLINES; k++)
    redraw (cwin, C_L1 + k);
  set_bit (cwin, C_REPAIR, SHADED, !check_done || repaired || !check || !libcheck_problems (libcheck_result (check)));
}

void libbox_check_open (void)
{
  int k;
  if (busy_already ())
    return;
  libcheck_free (check);
  check = libcheck_start (app_library ());
  check_done = repaired = 0;
  for (k = 0; k < CLINES; k++)
    cl[k][0] = 0;
  snprintf (cl[0], LINE, "Checking the library: its catalogue, its thumbnails...");
  open_centred (cwin, CW, CH);
  check_refresh ();
}

static void check_finished (void)
{
  char text[1200];
  const libcheck_report *r = libcheck_result (check);
  check_done = 1;
  libcheck_describe (r, text, sizeof text);
  wrap (text, cl, CLINES - 1, 70);
  snprintf (cl[CLINES - 1], LINE, "%s", libcheck_problems (r) ? "Repair keeps a copy of the catalogue first." : "");
  check_refresh ();
  activity_log (r->damaged || libcheck_problems (r) ? ACT_PROBLEM : ACT_CHECK, "Checked the library: %s",
                libcheck_problems (r) ? "problems found" : "all is well");
}

static void check_click (int *b)
{
  switch (b[4])
    {
    case C_CLOSE:
      if (check && !check_done)
        {
          libcheck_free (check);       /* stopped */
          check = NULL;
        }
      close_window (cwin);
      break;
    case C_REPAIR:
      if (check && check_done && !repaired)
        {
          char fixed[400], err[400];
          if (libcheck_repair (check, fixed, sizeof fixed, err, sizeof err))
            {
              wrap (err, cl, CLINES, 70);
              activity_log (ACT_PROBLEM, "Library repair: %s", err);
            }
          else
            {
              wrap (fixed, cl, CLINES, 70);
              activity_log (ACT_CHECK, "%s", fixed);
            }
          repaired = 1;
          check_refresh ();
          browser_reload ();
          mainwin_library_changed ();
        }
      break;
    }
}

/* --- events */
void libbox_click (int *b)
{
  if (b[2] == Button_Menu)
    return;
  if (b[3] == ewin)
    export_click (b);
  else if (b[3] == iwin)
    import_click (b);
  else if (b[3] == cwin)
    check_click (b);
}

int libbox_key (int win, int key)
{
  if (win == ewin)
    {
      if (key == KEY_RETURN)
        export_save_clicked ();
      else if (key == KEY_ESCAPE)
        export_close ();
      else
        return 0;
      return 1;
    }
  return 0;
}

void libbox_close_request (int win)
{
  if (win == iwin)
    import_forget ();
  if (win == cwin && check && !check_done)
    {
      libcheck_free (check);
      check = NULL;
    }
  close_window (win);
}

/* --- the job */
int libbox_busy (void)
{
  return xfer != NULL || (check && !check_done);
}

int libbox_percent (void)
{
  if (xfer)
    {
      int done, total;
      char what[200];
      libx_progress (xfer, &done, &total, what, sizeof what);
      return total > 0 ? done * 100 / total : 0;
    }
  if (check && !check_done)
    return libcheck_percent (check);
  return 0;
}

void libbox_status (char *out, size_t n)
{
  out[0] = 0;
  if (xfer)
    {
      int done, total;
      libx_progress (xfer, &done, &total, out, n);
    }
  else if (check && !check_done)
    snprintf (out, n, "Checking the library");
}

void libbox_idle (unsigned deadline)
{
  if (check && !check_done)
    {
      if (libcheck_step (check, app_now, deadline))
        check_finished ();
      return;
    }
  if (!xfer)
    return;
  {
    int f = libx_work (xfer, app_now, deadline);
    if (f & LIB_LIST_CHANGED)
      {
        browser_reload ();
        mainwin_library_changed ();
      }
    else if (f & LIB_THUMBS_CHANGED)
      browser_reload ();
  }
  if (libx_finished (xfer))
    {
      char msg[400];
      int failed, photos, versions, export = libx_is_export (xfer);
      libx_result (xfer, msg, sizeof msg, &failed);
      libx_counts (xfer, &photos, &versions);
      libx_free (xfer);
      xfer = NULL;
      activity_log (failed ? ACT_PROBLEM : export ? ACT_EXPORT : ACT_IMPORT, "%s", msg);
      if (failed)
        app_report (msg);
      else if (!export && photos > 0)
        {
          /* Show what arrived. */
          cat_source s = { SRC_LAST_IMPORT, 0, 0, "" };
          browser_set_source (&s);
          mainwin_library_changed ();
        }
      else if (!export)
        browser_reload ();
    }
}

void libbox_quit (void)
{
  import_forget ();
  libcheck_free (check);
  check = NULL;
  libx_free (xfer);                    /* an export's files so far stay; an import not merged changes nothing */
  xfer = NULL;
}

/* For the tests. */
void libbox_set_name (const char *s) { snprintf (ename, sizeof ename, "%s", s); }
const char *libbox_line (int win, int k)
{
  if (win == iwin && k >= 0 && k < 3)
    return il[k];
  if (win == cwin && k >= 0 && k < CLINES)
    return cl[k];
  if (win == ewin)
    return k == 0 ? ewhat : k == 1 ? eorig : ename;
  return "";
}
