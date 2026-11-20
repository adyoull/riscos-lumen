/* Copyright (c) 2026 Andrew Youll
   Released under the MIT licence: see the LICENSE file.
   SPDX-License-Identifier: MIT */
/* stackbox.c: the Stack frames window (Selection > Stack frames...).

   The selected photos that are FITS frames are the lights. Dark frames
   (or a directory of them) are dragged into the window; they're
   optional. The stack is saved beside the first light under the name
   given, and comes into the library when it's done, shown as the Last
   import. It runs in the background (stackbox_idle), with its progress
   in the window, the status area and the Activity window; the window
   then says what happened and which frames were left out and why. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <kernel.h>
#include <swis.h>
#include "fsys.h"
#include "fits.h"
#include "stack.h"
#include "ui.h"

#define T_TEXT    0x17000111u          /* text, vcentred, indirected; black on grey */
#define T_WRITE   0x0700F135u          /* text, border, vcentred, filled, indirected, writable */
#define T_BUTTON  0x1700313Du          /* text, border, centred, vcentred, filled, indirected, click */
#define SHADED    (1u << 22)
#define LINE 120
#define NLINES 7
#define MAXF 256
/* The stack's name ends /fits on RISC OS (.fits on other computers'
   discs, as it travels), so other astronomy programs know it. */
#if defined(__riscos) && !defined(__FAKE_RISCOS)
#define FITS_EXT "/fits"
#else
#define FITS_EXT ".fits"
#endif

enum { S_L1, S_L2, S_L3, S_L4, S_L5, S_L6, S_L7, S_NAMEL, S_NAME, S_CLEAR, S_CANCEL, S_STACK, S_COUNT };
#define WW 1240
#define WH 560

static int win = -1;
static char lines[NLINES][LINE], name[64] = "Stacked", cancel_text[8] = "Cancel", stack_text[8] = "Stack";
static char (*lights)[1024], (*darks)[1024];
static int nlights, ndarks, ignored, finished;
static stack_job *job;
static char out_path[1100], what[200];

int stackbox_window (void) { return win; }

static void icon_at (int *ic, int x0, int y0, int x1, int y1, unsigned flags, const char *text, const char *valid,
                     int size)
{
  ic[0] = x0; ic[1] = y0; ic[2] = x1; ic[3] = y1;
  ic[4] = (int) flags;
  ic[5] = (int) text; ic[6] = valid ? (int) valid : -1; ic[7] = size;
}

int stackbox_create (void)
{
  static const char v_name[] = "Pptr_write;A~ .:*#$&@^%\\", v_ok[] = "R6,3", v_cancel[] = "R5,3";
  static const char title[] = "Stack frames";
  int b[22 + 8 * S_COUNT], *ic = &b[22], k, y;
  memset (b, 0, sizeof b);
  b[2] = WW; b[3] = WH;
  b[6] = -1;
  b[7] = (int) 0x86000012u;
  ((unsigned char *) b)[32] = 7;  ((unsigned char *) b)[33] = 2;
  ((unsigned char *) b)[34] = 7;  ((unsigned char *) b)[35] = 1;
  ((unsigned char *) b)[36] = 3;  ((unsigned char *) b)[37] = 1;
  ((unsigned char *) b)[38] = 12;
  b[10] = 0; b[11] = -WH; b[12] = WW; b[13] = 0;
  b[14] = 0x13D;
  b[16] = 1;
  b[18] = (int) title; b[19] = -1; b[20] = (int) sizeof title;
  b[21] = S_COUNT;
  for (k = 0, y = -20; k < NLINES; k++, y -= 48)
    icon_at (ic + 8 * (S_L1 + k), 24, y - 44, WW - 24, y, T_TEXT, lines[k], NULL, LINE);
  icon_at (ic + 8 * S_NAMEL, 24, -420, 260, -372, T_TEXT, "Save it as", NULL, 11);
  icon_at (ic + 8 * S_NAME, 270, -420, 760, -372, T_WRITE, name, v_name, (int) sizeof name);
  icon_at (ic + 8 * S_CLEAR, 24, -536, 300, -476, T_BUTTON, "Clear darks", v_cancel, 12);
  icon_at (ic + 8 * S_CANCEL, WW - 400, -536, WW - 216, -476, T_BUTTON, cancel_text, v_cancel, (int) sizeof cancel_text);
  icon_at (ic + 8 * S_STACK, WW - 200, -536, WW - 24, -476, T_BUTTON, stack_text, v_ok, (int) sizeof stack_text);
  return _swix (Wimp_CreateWindow, _IN (1) | _OUT (0), b, &win) ? -1 : 0;
}

static void redraw (int icon)
{
  int st[4];
  st[0] = win; st[1] = icon; st[2] = 0; st[3] = 0;
  _swix (Wimp_SetIconState, _IN (1), st);
}

static void set_shaded (int icon, int on)
{
  int st[4];
  st[0] = win; st[1] = icon;
  st[2] = on ? (int) SHADED : 0;
  st[3] = (int) SHADED;
  _swix (Wimp_SetIconState, _IN (1), st);
}

static void refresh (void)
{
  int k;
  for (k = 0; k < NLINES; k++)
    redraw (S_L1 + k);
  redraw (S_NAME);
  redraw (S_CANCEL);
  redraw (S_STACK);
  set_shaded (S_STACK, job != NULL || finished || nlights < 2);
  set_shaded (S_CLEAR, job != NULL || finished || ndarks == 0);
  set_shaded (S_NAME, job != NULL || finished);
}

static const char *leaf_of (const char *path)
{
  return fsys_leaf (path);
}

/* The lines before stacking: what will be stacked. */
static void describe (void)
{
  int k;
  for (k = 0; k < NLINES; k++)
    lines[k][0] = 0;
  if (nlights >= 2)
    snprintf (lines[0], LINE, "Lights: %d FITS frames, %s to %s", nlights, leaf_of (lights[0]), leaf_of (lights[nlights - 1]));
  else
    snprintf (lines[0], LINE, "Lights: select two or more FITS frames in the Browser first (%d selected).", nlights);
  if (ignored)
    snprintf (lines[1], LINE, "(%d other selected photo%s left out: only FITS frames can be stacked.)", ignored,
              ignored == 1 ? "" : "s");
  if (ndarks)
    snprintf (lines[2], LINE, "Darks: %d frame%s, %s%s", ndarks, ndarks == 1 ? "" : "s", leaf_of (darks[0]),
              ndarks > 1 ? " and the rest" : "");
  else
    snprintf (lines[2], LINE, "Darks: none. Drag dark frames, or their directory, into this window (optional).");
  snprintf (lines[3], LINE, "Darks are taken with the lens cap on, at the lights' exposure, ISO and temperature.");
  if (nlights >= 1)
    {
      char dir[1024];
      snprintf (dir, sizeof dir, "%s", lights[0]);
      if (fsys_leaf (dir) > dir)
        ((char *) fsys_leaf (dir))[-1] = 0;   /* the directory, without its separator */
      snprintf (lines[5], LINE, "Saved in %s, then added to the library.", dir);
    }
}

static int is_fits (const char *path)
{
  FILE *f = fopen (path, "rb");
  unsigned char head[32];
  size_t got;
  if (!f)
    return 0;
  got = fread (head, 1, sizeof head, f);
  fclose (f);
  return fits_is (head, got);
}

void stackbox_open (void)
{
  cat_id *ids;
  int n, k;
  if (job)
    {
      int s[1];
      s[0] = win;
      _swix (Wimp_GetWindowState, _IN (1), s);
      app_report ("Lumen is already stacking frames. Wait for it to finish (the Activity window shows how far it has got).");
      return;
    }
  free (lights);
  lights = NULL;
  nlights = ignored = 0;
  finished = 0;
  snprintf (cancel_text, sizeof cancel_text, "Cancel");
  n = browser_selection (&ids);
  if (n > 0)
    lights = malloc ((size_t) (n < MAXF ? n : MAXF) * sizeof *lights);
  for (k = 0; k < n && lights; k++)
    {
      cat_details d;
      if (cat_get_details (lib_cat (app_library ()), ids[k], &d) == 0 && !d.missing && is_fits (d.path) && nlights < MAXF)
        snprintf (lights[nlights++], sizeof lights[0], "%s", d.path);
      else
        ignored++;
    }
  free (ids);
  /* A name not used yet, beside the first light. */
  if (nlights)
    {
      char dir[1024], leaf[80], path[1200];
      fsys_entry e;
      int i;
      snprintf (dir, sizeof dir, "%s", lights[0]);
      if (fsys_leaf (dir) > dir)
        ((char *) fsys_leaf (dir))[-1] = 0;
      for (i = 1; i < 100; i++)
        {
          snprintf (name, sizeof name, i == 1 ? "Stacked" : "Stacked_%d", i);
          snprintf (leaf, sizeof leaf, "%s" FITS_EXT, name);
          fsys_join (path, sizeof path, dir, leaf);
          if (fsys_stat (path, &e) != 0)
            break;
        }
    }
  describe ();
  {
    int s[9], xe = 1, ye = 1, sw = 0, sh = 0;
    _swix (OS_ReadModeVariable, _INR (0, 1) | _OUT (2), -1, 4, &xe);
    _swix (OS_ReadModeVariable, _INR (0, 1) | _OUT (2), -1, 5, &ye);
    _swix (OS_ReadModeVariable, _INR (0, 1) | _OUT (2), -1, 11, &sw);
    _swix (OS_ReadModeVariable, _INR (0, 1) | _OUT (2), -1, 12, &sh);
    s[0] = win;
    _swix (Wimp_GetWindowState, _IN (1), s);
    if (!(s[8] & (1 << 16)))
      {
        int cx = ((sw + 1) << xe) / 2, cy = ((sh + 1) << ye) / 2;
        s[1] = cx - WW / 2; s[2] = cy - WH / 2; s[3] = cx + WW / 2; s[4] = cy + WH / 2;
        s[5] = 0; s[6] = 0;
      }
    s[7] = -1;
    _swix (Wimp_OpenWindow, _IN (1), s);
  }
  refresh ();
}

static void add_dark (const char *path)
{
  if (!darks)
    darks = malloc (MAXF * sizeof *darks);
  if (!darks || ndarks >= MAXF)
    return;
  snprintf (darks[ndarks++], sizeof darks[0], "%s", path);
}

/* A file or a directory dropped in the window: dark frames. */
void stackbox_drop (const char *path)
{
  fsys_entry e;
  int before = ndarks, other = 0;
  if (job || finished)
    return;
  if (fsys_stat (path, &e) != 0)
    return;
  if (e.is_dir)
    {
      fsys_dir *d = fsys_opendir (path);
      fsys_entry f;
      char full[1300];
      while (d && fsys_readdir (d, &f) == 1)
        if (!f.is_dir)
          {
            fsys_join (full, sizeof full, path, f.name);
            if (is_fits (full))
              add_dark (full);
            else
              other++;
          }
      if (d)
        fsys_closedir (d);
    }
  else if (is_fits (path))
    add_dark (path);
  else
    other++;
  if (ndarks == before)
    app_report (other ? "Darks must be FITS frames, like the lights: there were none there." : "There were no FITS frames there.");
  describe ();
  refresh ();
}

static void start (void)
{
  const char *lp[MAXF], *dp[MAXF];
  char dir[1024], leaf[80], err[600], creator[64];
  int k;
  if (job || nlights < 2)
    return;
  {
    int st[10];
    st[0] = win; st[1] = S_NAME;
    _swix (Wimp_GetIconState, _IN (1), st);
  }
  if (!name[0])
    snprintf (name, sizeof name, "Stacked");
  snprintf (dir, sizeof dir, "%s", lights[0]);
  if (fsys_leaf (dir) > dir)
    ((char *) fsys_leaf (dir))[-1] = 0;
  snprintf (leaf, sizeof leaf, "%s" FITS_EXT, name);
  fsys_join (out_path, sizeof out_path, dir, leaf);
  {
    fsys_entry e;
    if (fsys_stat (out_path, &e) == 0
        && !app_confirm ("There's already a file of that name beside the lights. Replace it with the stack?", "Replace"))
      return;
  }
  for (k = 0; k < nlights; k++)
    lp[k] = lights[k];
  for (k = 0; k < ndarks; k++)
    dp[k] = darks[k];
  snprintf (creator, sizeof creator, "Lumen %s", LUMEN_VERSION);
  job = stack_start (lp, nlights, dp, ndarks, out_path, creator, err, sizeof err);
  if (!job)
    {
      app_report (err);
      return;
    }
  snprintf (cancel_text, sizeof cancel_text, "Stop");
  for (k = 0; k < NLINES; k++)
    lines[k][0] = 0;
  snprintf (lines[0], LINE, "Stacking %d lights%s%s...", nlights, ndarks ? " with " : "", ndarks ? (ndarks == 1 ? "1 dark" : "darks") : "");
  activity_log (ACT_OTHER, "Stacking %d frames into %s", nlights, leaf_of (out_path));
  refresh ();
}

static void close_window (void)
{
  int b[1];
  b[0] = win;
  _swix (Wimp_CloseWindow, _IN (1), b);
}

static void stop (void)
{
  if (!job)
    return;
  stack_free (job);
  job = NULL;
  activity_log (ACT_OTHER, "Stacking stopped");
}

/* What happened: the summary, and the frames left out. */
static void report (void)
{
  const stack_frame *fr;
  int n = stack_frames (job, &fr), k, line = 2, out = 0;
  char sum[600];
  stack_summary (job, sum, sizeof sum);
  for (k = 0; k < NLINES; k++)
    lines[k][0] = 0;
  {
    /* The summary in two lines. */
    size_t len = strlen (sum), cut = len;
    if (len >= LINE - 1)
      {
        cut = LINE - 2;
        while (cut > 0 && sum[cut] != ' ')
          cut--;
      }
    snprintf (lines[0], LINE, "%.*s", (int) cut, sum);
    snprintf (lines[1], LINE, "%s", sum + cut + (sum[cut] == ' '));
  }
  for (k = 0; k < n; k++)
    if (!fr[k].used)
      {
        out++;
        if (line < NLINES - 1)
          snprintf (lines[line++], LINE, "Left out: %s: %s", leaf_of (fr[k].path), fr[k].why);
        activity_log (ACT_PROBLEM, "Stacking: %s left out: %s", leaf_of (fr[k].path), fr[k].why);
      }
  if (out > NLINES - 3)
    snprintf (lines[NLINES - 2], LINE, "...and %d more left out (the Activity window lists them all).", out - (NLINES - 4));
  snprintf (lines[NLINES - 1], LINE, "%s is in the library: Last Import.", leaf_of (out_path));
}

void stackbox_idle (unsigned deadline)
{
  int r;
  if (!job)
    return;
  r = stack_step (job, app_now, deadline);
  if (r == 0)
    {
      char w[200];
      stack_progress (job, w, sizeof w);
      if (strcmp (w, lines[1]))
        {
          snprintf (lines[1], LINE, "%s", w);
          redraw (S_L2);
        }
      return;
    }
  if (r < 0)
    {
      char err[600];
      snprintf (err, sizeof err, "Stacking failed: %s", stack_error (job));
      activity_log (ACT_PROBLEM, "%s", err);
      stack_free (job);
      job = NULL;
      for (r = 0; r < NLINES; r++)
        lines[r][0] = 0;
      snprintf (lines[0], LINE, "%.*s", LINE - 1, err);
      if (strlen (err) >= LINE - 1)
        snprintf (lines[1], LINE, "%s", err + LINE - 1);
      snprintf (cancel_text, sizeof cancel_text, "Close");
      finished = 1;
      refresh ();
      app_report (err);
      return;
    }
  /* Done: the report, and the stack into the library. */
  report ();
  {
    char sum[600];
    stack_summary (job, sum, sizeof sum);
    activity_log (ACT_EXPORT, "Stacked into %s: %s", leaf_of (out_path), sum);
  }
  stack_free (job);
  job = NULL;
  finished = 1;
  snprintf (cancel_text, sizeof cancel_text, "Close");
  refresh ();
  if (lib_import (app_library (), out_path) == 0)
    {
      cat_source s = { SRC_LAST_IMPORT, 0, 0, "" };
      browser_set_source (&s);
      mainwin_library_changed ();
    }
}

void stackbox_click (int *b)
{
  if (b[2] == Button_Menu || b[3] != win)
    return;
  switch (b[4])
    {
    case S_STACK:
      start ();
      break;
    case S_CLEAR:
      if (!job && !finished)
        {
          ndarks = 0;
          describe ();
          refresh ();
        }
      break;
    case S_CANCEL:
      if (job)
        {
          stop ();
          describe ();
          snprintf (cancel_text, sizeof cancel_text, "Cancel");
          refresh ();
        }
      else
        close_window ();
      break;
    }
}

int stackbox_key (int key)
{
  if (key == KEY_RETURN && !job && !finished)
    start ();
  else if (key == KEY_ESCAPE && !job)
    close_window ();
  else
    return 0;
  return 1;
}

void stackbox_close_request (void)
{
  /* Closing it doesn't stop the stacking: Stop does. */
  close_window ();
}

int stackbox_busy (void)
{
  return job != NULL;
}

int stackbox_percent (void)
{
  return job ? stack_progress (job, what, sizeof what) : 0;
}

void stackbox_status (char *out, size_t n)
{
  out[0] = 0;
  if (job)
    stack_progress (job, out, n);
}

void stackbox_quit (void)
{
  stop ();
  free (lights);
  free (darks);
  lights = NULL;
  darks = NULL;
  nlights = ndarks = 0;
}

/* For the tests. */
const char *stackbox_line (int k) { return k >= 0 && k < NLINES ? lines[k] : ""; }
void stackbox_set_name (const char *s) { snprintf (name, sizeof name, "%s", s); }
const char *stackbox_output (void) { return out_path; }
