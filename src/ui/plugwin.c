/* Copyright (c) 2026 Andrew Youll
   Released under the MIT licence: see the LICENSE file.
   SPDX-License-Identifier: MIT */
/* plugwin.c: the Add plug-in window (Plug-ins > Add plug-in... on Lumen's
   menus). It is somewhere to drop a plug-in's directory (app.c's
   data_load does the adding), says where plug-ins come from, and has a
   button that opens PackMan.

   Icon flags as in exportbox.c (classic PRM): text, vcentred, indirected;
   bit 9 right-justified; colours in bits 24-31. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <kernel.h>
#include <swis.h>
#include "ui.h"

enum { I_DIR, I_LINE1, I_LINE2, I_LINE3, I_PACKMAN, I_COUNT };

#define W 900
#define H 300
#define T_TEXT    0x17000111u          /* text, vcentred, indirected; black on grey */
#define T_BUTTON  0x1700313Du          /* text, border, centred, vcentred, filled, indirected, click */

static int win = -1;

int plugwin_window (void) { return win; }

static void icon_at (int *ic, int x0, int y0, int x1, int y1, unsigned flags, const char *text, const char *valid)
{
  ic[0] = x0; ic[1] = y0; ic[2] = x1; ic[3] = y1;
  ic[4] = (int) flags;
  ic[5] = (int) text; ic[6] = valid ? (int) valid : -1; ic[7] = (int) strlen (text) + 1;
}

int plugwin_create (void)
{
  static const char v_button[] = "R5,3";
  int b[22 + 8 * I_COUNT];
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
  memcpy (&b[18], "Add plug-in", 12);
  b[21] = I_COUNT;
  /* The Wimp's directory sprite, as a picture of what to drop. */
  {
    int *d = ic + 8 * I_DIR;
    d[0] = 24; d[1] = -136; d[2] = 120; d[3] = -24;
    d[4] = 0x1A;                       /* sprite, centred both ways; not a button */
    memcpy (&d[5], "directory", 10);   /* the name in the icon's own 12 bytes */
  }
  icon_at (ic + 8 * I_LINE1, 140, -64, W - 24, -20, T_TEXT, "Drag a plug-in's directory into this window", NULL);
  icon_at (ic + 8 * I_LINE2, 140, -112, W - 24, -68, T_TEXT, "to add it to Lumen. Plug-ins come from PackMan:", NULL);
  icon_at (ic + 8 * I_LINE3, 140, -160, W - 24, -116, T_TEXT, "LumenRaw, for camera raw files, is one.", NULL);
  icon_at (ic + 8 * I_PACKMAN, W - 300, -272, W - 24, -208, T_BUTTON, "Open PackMan", v_button);
  return _swix (Wimp_CreateWindow, _IN (1) | _OUT (0), b, &win) ? -1 : 0;
}

/* In the middle of the screen, on top. */
void plugwin_open (void)
{
  int s[9], xe = 1, ye = 1, sw = 0, sh = 0;
  _swix (OS_ReadModeVariable, _INR (0, 1) | _OUT (2), -1, 4, &xe);
  _swix (OS_ReadModeVariable, _INR (0, 1) | _OUT (2), -1, 5, &ye);
  _swix (OS_ReadModeVariable, _INR (0, 1) | _OUT (2), -1, 11, &sw);
  _swix (OS_ReadModeVariable, _INR (0, 1) | _OUT (2), -1, 12, &sh);
  s[0] = win;
  if (_swix (Wimp_GetWindowState, _IN (1), s))
    return;
  if (!(s[8] & (1 << 16)))             /* not open yet: centre it */
    {
      int cx = ((sw + 1) << xe) / 2, cy = ((sh + 1) << ye) / 2;
      s[1] = cx - W / 2; s[2] = cy - H / 2; s[3] = cx + W / 2; s[4] = cy + H / 2;
      s[5] = 0; s[6] = 0;
    }
  s[7] = -1;
  _swix (Wimp_OpenWindow, _IN (1), s);
}

void plugwin_click (int *b)
{
  if (b[4] == I_PACKMAN && (b[2] == Button_Select || b[2] == Button_Adjust))
    {
      const char *v = getenv ("PackMan$Dir");
      char dir[600];
      snprintf (dir, sizeof dir, "%s", v ? v : "");
      if (!dir[0])
        {
          app_report ("PackMan hasn't been seen on this computer yet. Open the directory it is in "
                      "(or get it from the RISC OS Open website) and try again.");
          return;
        }
      {
        char cmd[700];
        snprintf (cmd, sizeof cmd, "Filer_Run %s", dir);
        if (_swix (Wimp_StartTask, _IN (0), cmd))
          app_report ("Lumen couldn't start PackMan.");
      }
    }
}
