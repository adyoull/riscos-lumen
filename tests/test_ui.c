/* Copyright (c) 2026 Andrew Youll
   Released under the MIT licence: see the LICENSE file.
   SPDX-License-Identifier: MIT */
#define _GNU_SOURCE
#include <math.h>
#include <stdint.h>
#include "thumbgen.h"
/* Lumen's desktop front end against the Fake RISC OS. Every SWI it uses is
   faked here; the test plays what a user does: drops a folder from the
   Filer, waits for thumbnails, rates and flags with the keyboard, uses the
   menus, and quits. The core runs for real (SQLite, libjpeg) on Linux.
   Usage: test_ui WORKDIR */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <jpeglib.h>
#include <sys/stat.h>
#include "fake.h"
#include "swis.h"
#include "fsys.h"
#include "rawjob.h"
#include "styles.h"
#include "exif.h"
#include "colour.h"
#include "export.h"
#include "presets.h"
#include "ui.h"

#define BROWSER 0x2a000
#define INFO 0x2b000
#define VIEWER 0x2c000
#define EXPORTW 0x2d000
#define MAINW 0x2e000
#define INSP 0x2f000
#define PLUGW 0x30000
#define CHOICEW 0x31000
#define ACTW 0x32000
#define PRINTW 0x33000
#define TAGW 0x34000
#define SMARTW 0x35000
#define LIBEW 0x36000
#define LIBIW 0x37000
#define LIBCW 0x38000
#define FILER 0x9000
#define TASK 0x1234

static char work[512];
static int nwindows, browser_block[23], viewer_block[23];
static int vopened[8], vopen_calls, vclosed, drag_block[10], drag_calls, ptr_x, ptr_y, ptr_buttons, ptr_window = -1, ptr_icon = -1;
static char *vtitle;
static int last_plot_w, last_plot_h;
static char *title_buf;
static int iconbar[9], win_icon[9];
static int opened[8], open_calls, mopened[9], iopened[8], nested_opens;
static unsigned main_flags = 0xAF000002u;
static int extent[4];
static int caret_window, caret_icon = -1;
static unsigned print_turn = 1u << 21, print_sharpen = 1u << 21, print_names_opt;
static int force_redraws, title_redraws;
static int sprite_plots, smooth_plots, masked_plots, last_scale[4];
static int stars;
static char texts[256][80];
static int ntexts;
static int sent_reason, sent_block[64], sent_to;
static const int *menu_open;
static char reported[256], asked[256], asked_button[32];
static int answer = 2;            /* Cancel */
static int plugw_opens;
static char started[300];               /* the last Wimp_StartTask command */
static int other_lumen;
static int menu_state[6] = { -1, -1, -1, -1, -1, -1 };   /* what Wimp_GetMenuState 1 reports */
static int getmenustate0, submenu_opened;
static unsigned smart_icon_flags[32];   /* the Smart album window's */
static int tagw_open, smartw_open;
static unsigned lib_icon_flags[3][16];   /* the Export, Import and Check library windows' icons */
static unsigned choice_icon_flags[32];  /* the Choices window's icons' state (bit 21: selected) */                /* a task handle the fake Task Manager lists as another Lumen */

/* TaskManager_EnumerateTasks: ourselves, a LumenRaw task window, a module
   task called Lumen, and maybe another Lumen; two records a call. */
static const _kernel_oserror *tasks (int n, int r[10])
{
  static const char *const names[4] = { "Lumen\r", "LumenRaw", "Lumen", "Lumen\0" };
  int handles[4], flags[4] = { 0, 0, 1, 0 }, *buf = (int *) r[1], k, at = r[0], count = other_lumen ? 4 : 3;
  (void) n;
  handles[0] = TASK + 0x20000;          /* ourselves, with other top bits (as in ROL) */
  handles[1] = 0x2000; handles[2] = 0x3000; handles[3] = other_lumen;
  for (k = 0; k < 2 && at < count; k++, at++)
    {
      buf[4 * k] = handles[at];
      buf[4 * k + 1] = (int) names[at];
      buf[4 * k + 2] = 640 * 1024;
      buf[4 * k + 3] = flags[at];
    }
  r[0] = at < count ? at : -1;
  r[1] = (int) (buf + 4 * k);
  return NULL;
}
static int scroll_y;
static int closedown;
static int processed_key;

static const int *state_of (int w)
{
  return w == VIEWER ? vopened : w == MAINW ? mopened : w == INSP ? iopened : opened;
}

static const _kernel_oserror *wimp (int swi, int r[10])
{
  int *b = (int *) r[1];
  switch (swi)
    {
    case Wimp_Initialise:
      CHECK (strcmp ((const char *) r[2], "Lumen") == 0, "task name");
      r[0] = 380; r[1] = TASK;
      return NULL;
    case Wimp_CreateIcon:
      if (b[0] < 0)                    /* the icon bar */
        {
          memcpy (iconbar, b, sizeof iconbar);
          r[0] = 1;
        }
      else
        {
          memcpy (win_icon, b, sizeof win_icon);   /* the search field */
          r[0] = 0;
        }
      return NULL;
    case Wimp_ResizeIcon:
      CHECK (r[0] == MAINW && r[1] == 0, "the search field moves with the bottom bar");
      memcpy (&win_icon[1], &r[2], 4 * sizeof (int));
      return NULL;
    case Wimp_CreateWindow:
      if (nwindows++ == 0)
        {
          memcpy (browser_block, b, sizeof browser_block);
          title_buf = (char *) b[18];
          r[0] = BROWSER;
        }
      else if (nwindows == 2)
        {
          memcpy (viewer_block, b, sizeof viewer_block);
          vtitle = (char *) b[18];
          r[0] = VIEWER;
        }
      else if (nwindows == 3)
        r[0] = EXPORTW;
      else if (nwindows == 4)
        {
          title_buf = vtitle = (char *) b[18];   /* one title, the main window's */
          r[0] = MAINW;
        }
      else if (nwindows == 5)
        r[0] = INSP;
      else if (nwindows == 6)
        r[0] = PLUGW;
      else if (nwindows == 7)
        r[0] = CHOICEW;
      else if (nwindows == 8)
        r[0] = ACTW;
      else if (nwindows == 9)
        r[0] = PRINTW;
      else if (nwindows == 10)
        r[0] = TAGW;
      else if (nwindows == 11)
        r[0] = SMARTW;
      else if (nwindows >= 12 && nwindows <= 14)
        {
          int k2;
          for (k2 = 0; k2 < 16 && k2 < b[21]; k2++)
            lib_icon_flags[nwindows - 12][k2] = (unsigned) b[22 + 8 * k2 + 4];
          r[0] = nwindows == 12 ? LIBEW : nwindows == 13 ? LIBIW : LIBCW;
        }
      else if (nwindows == 12)
        r[0] = LIBEW;
      else if (nwindows == 13)
        r[0] = LIBIW;
      else if (nwindows == 14)
        r[0] = LIBCW;
      else
        r[0] = INFO;
      return NULL;
    case Wimp_OpenWindow:
      if (b[0] == BROWSER || b[0] == VIEWER || b[0] == INSP)
        {
          CHECK (r[2] == 0x4B534154 && r[3] == MAINW && r[4] == 0, "a child: nested in the main window");
          nested_opens++;
        }
      if (b[0] == MAINW)
        {
          memcpy (mopened, b, sizeof mopened);
          if (r[2] == 0x4B534154)        /* extended: the frame changed */
            {
              CHECK (r[3] == -1 && r[4] == 1, "the main window stays top level; new flags given");
              main_flags = (unsigned) b[8];
            }
        }
      else if (b[0] == INSP)
        memcpy (iopened, b, sizeof iopened);
      else if (b[0] == PLUGW)
        plugw_opens++;
      else if (b[0] == TAGW)
        tagw_open = 1;
      else if (b[0] == SMARTW)
        smartw_open = 1;
      else if (b[0] == BROWSER)
        {
          memcpy (opened, b, sizeof opened);
          scroll_y = b[6];
          open_calls++;
        }
      else if (b[0] == VIEWER)
        {
          memcpy (vopened, b, sizeof vopened);
          vopen_calls++;
        }
      return NULL;
    case Wimp_GetWindowState:
      CHECK (b[0] == BROWSER || b[0] == VIEWER || b[0] == EXPORTW || b[0] == MAINW || b[0] == INSP
             || b[0] == PLUGW || b[0] == CHOICEW || b[0] == ACTW || b[0] == PRINTW || b[0] == TAGW || b[0] == SMARTW
             || b[0] == LIBEW || b[0] == LIBIW || b[0] == LIBCW,
             "window state: one of ours");
      if (b[0] == EXPORTW)
        {
          b[1] = 600; b[2] = 400; b[3] = 1320; b[4] = 860; b[5] = 0; b[6] = 0;
        }
      else
        memcpy (&b[1], &state_of (b[0])[1], 6 * sizeof (int));
      b[7] = -1; b[8] = 0;
      return NULL;
    case Wimp_SetIconState:
      if (b[0] == CHOICEW && b[1] >= 0 && b[1] < 32)
        choice_icon_flags[b[1]] = (choice_icon_flags[b[1]] & ~(unsigned) b[3]) ^ (unsigned) b[2];
      if (b[0] == SMARTW && b[1] >= 0 && b[1] < 32)
        smart_icon_flags[b[1]] = (smart_icon_flags[b[1]] & ~(unsigned) b[3]) ^ (unsigned) b[2];
      if ((b[0] == LIBEW || b[0] == LIBIW || b[0] == LIBCW) && b[1] >= 0 && b[1] < 16)
        {
          unsigned *f = &lib_icon_flags[(b[0] - LIBEW) / 0x1000][b[1]];
          *f = (*f & ~(unsigned) b[3]) ^ (unsigned) b[2];
        }
      return NULL;
    case Wimp_GetIconState:
      if (b[0] == PRINTW)
        {
          b[6] = (int) (b[1] == 12 ? print_turn : b[1] == 13 ? print_sharpen : b[1] == 27 ? print_names_opt : 0);
          return NULL;
        }
      if (b[0] == SMARTW)
        {
          b[6] = b[1] >= 0 && b[1] < 32 ? (int) smart_icon_flags[b[1]] : 0;
          return NULL;
        }
      if (b[0] == LIBEW || b[0] == LIBIW || b[0] == LIBCW)
        {
          b[6] = b[1] >= 0 && b[1] < 16 ? (int) lib_icon_flags[(b[0] - LIBEW) / 0x1000][b[1]] : 0;
          return NULL;
        }
      CHECK (b[0] == CHOICEW, "icon state: the Choices window's");
      if (b[1] >= 0 && b[1] < 32)
        b[6] = (int) choice_icon_flags[b[1]];
      return NULL;
    case Wimp_StartTask:
      snprintf (started, sizeof started, "%s", (const char *) r[0]);
      return NULL;
    case Wimp_GetMenuState:
      if (r[0] == 0)
        {
          /* Is a menu of ours open? -1 when closed. */
          b[0] = (intptr_t) menu_open == -1 || !menu_open ? -1 : 0;
          b[1] = -1;
          getmenustate0++;
          return NULL;
        }
      CHECK (r[0] == 1, "Wimp_GetMenuState 1: for a window and icon");
      memcpy (b, menu_state, sizeof menu_state);
      return NULL;
    case Wimp_CreateSubMenu:
      submenu_opened = r[1];
      return NULL;
    case Wimp_DragBox:
      memcpy (drag_block, b, sizeof drag_block);
      drag_calls++;
      return NULL;
    case Wimp_SetExtent:
      if (r[0] == BROWSER)
        memcpy (extent, b, sizeof extent);
      return NULL;
    case Wimp_ForceRedraw:
      if (r[1] == 0x4B534154)
        title_redraws++;
      else
        force_redraws++;
      return NULL;
    case Wimp_SetCaretPosition:
      caret_window = r[0];
      caret_icon = r[1];
      return NULL;
    case Wimp_GetCaretPosition:
      b[0] = caret_window;
      b[1] = caret_icon;
      b[2] = b[3] = 0;
      b[4] = -1;
      b[5] = -1;
      return NULL;
    case Wimp_RedrawWindow:
      /* The whole visible area, one rectangle. */
      {
        const int *o = state_of (b[0]);
        memcpy (&b[1], &o[1], 6 * sizeof (int));
        b[7] = o[1]; b[8] = o[2]; b[9] = o[3]; b[10] = o[4];
      }
      r[0] = 1;
      return NULL;
    case Wimp_GetRectangle:
      r[0] = 0;
      return NULL;
    case Wimp_TextOp:
      if ((r[0] & 0xFF) == 1)
        r[0] = (int) strlen ((const char *) r[1]) * 16;
      else if ((r[0] & 0xFF) == 2 && ntexts < 256)
        snprintf (texts[ntexts++], sizeof texts[0], "%s", (const char *) r[1]);
      return NULL;
    case Wimp_SendMessage:
      sent_reason = r[0];
      b[2] = 0x5151;                   /* the Wimp fills in my_ref */
      memcpy (sent_block, b, sizeof sent_block);
      sent_to = r[2];
      return NULL;
    case Wimp_CreateMenu:
      menu_open = b;
      return NULL;
    case Wimp_SpriteOp:                /* 40, read sprite information: the icon is there */
      CHECK (r[0] == 40 && (strcmp ((const char *) r[2], "!lumen") == 0 || strncmp ((const char *) r[2], "file_", 5) == 0),
             "Wimp_SpriteOp 40 on !lumen, or a file type's icon");
      return NULL;
    case Wimp_GetPointerInfo:
      b[0] = ptr_x; b[1] = ptr_y; b[2] = ptr_buttons; b[3] = ptr_window; b[4] = ptr_icon;
      return NULL;
    case Wimp_ProcessKey:
      processed_key = r[0];
      return NULL;
    case Wimp_ReportError:
      snprintf (reported, sizeof reported, "%s", ((const _kernel_oserror *) r[0])->errmess);
      if (r[1] & 0x100)                /* a question: the answer the test chose */
        {
          snprintf (asked, sizeof asked, "%s", reported);
          snprintf (asked_button, sizeof asked_button, "%s", (const char *) r[5]);
          reported[0] = '\0';
          r[1] = answer;
        }
      return NULL;
    case Wimp_CloseWindow:
      if (b[0] == VIEWER)
        vclosed++;
      return NULL;
    case Wimp_CloseDown:
      CHECK (r[0] == TASK && r[1] == 0x4B534154, "Wimp_CloseDown: task handle, TASK");
      closedown++;
      return NULL;
    }
  return NULL;
}

static unsigned clock_cs;

/* A printer: PDriver_* as the classic PRM describes them, an A4 page with
   5 mm margins, 300 dpi; what the job does is recorded. */
static int print_jobs, print_rects, print_ended, print_aborted, print_matrix[4], print_pos[2], print_rect[4];
static int print_plot_w, print_plot_h, print_plots, print_handle_open;
/* The rectangles given for the page being made (several photos to a page,
   and names under them), handed back one by one by DrawPage/GetRectangle. */
static int pg_n, pg_i, pg_ids[128], pg_rects[128][4], pg_mats[128][4], pg_pos[128][2], print_pages, print_names;
static int have_printer = 1, printer_colour = 1;
static const _kernel_oserror *printer (int swi, int r[10])
{
  static _kernel_oserror none = { 0, "Printer driver not loaded" };
  static int pending;
  switch (swi)
    {
    case PDriver_Info:
      if (!have_printer)
        return &none;
      r[0] = 400; r[1] = 300; r[2] = 300; r[3] = printer_colour; r[4] = (int) "Fake printer"; r[7] = 0;
      return NULL;
    case PDriver_PageSize:
      r[1] = 595276; r[2] = 841890;
      r[3] = 14173; r[4] = 14173; r[5] = 595276 - 14173; r[6] = 841890 - 14173;
      return NULL;
    case PDriver_SelectJob:
      if (r[0])
        print_jobs++;
      r[0] = 0;
      return NULL;
    case PDriver_GiveRectangle:
      memcpy (print_rect, (const int *) r[1], sizeof print_rect);
      memcpy (print_matrix, (const int *) r[2], sizeof print_matrix);
      memcpy (print_pos, (const int *) r[3], sizeof print_pos);
      if (pg_n < 128)
        {
          pg_ids[pg_n] = r[0];
          memcpy (pg_rects[pg_n], print_rect, sizeof print_rect);
          memcpy (pg_mats[pg_n], print_matrix, sizeof print_matrix);
          memcpy (pg_pos[pg_n], print_pos, sizeof print_pos);
          pg_n++;
        }
      print_rects++;
      return NULL;
    case PDriver_DrawPage:
      pending = 1;
      pg_i = 0;
      print_pages++;
      r[0] = pg_n > 0; r[2] = pg_n > 0 ? pg_ids[0] : 0;
      return NULL;
    case PDriver_GetRectangle:
      if (++pg_i < pg_n)
        {
          r[0] = 1; r[2] = pg_ids[pg_i];
          return NULL;
        }
      r[0] = 0;
      pending = 0;
      pg_n = 0;
      return NULL;
    case Font_FindFont:
      r[0] = 5;
      return NULL;
    case Font_LoseFont: case ColourTrans_SetFontColours:
      return NULL;
    case Font_ScanString:                /* 6 points (6000 millipoints) a character */
      r[3] = (int) strlen ((const char *) r[1]) * 6000; r[4] = 0;
      return NULL;
    case Font_Paint:
      CHECK (print_handle_open && r[0] == 5 && (r[2] & 0x110) == 0x110, "a name painted, OS units, with the font, during the job");
      print_names++;
      return NULL;
    case PDriver_EndJob:
      print_ended++;
      return NULL;
    case PDriver_AbortJob:
      print_aborted++;
      return NULL;
    case OS_Find:
      if (r[0] == 0x8F)
        {
          CHECK (!strcmp ((const char *) r[1], "printer:"), "the job goes to printer:");
          r[0] = 77;
          print_handle_open = 1;
        }
      else if (r[0] == 0 && r[1] == 77)
        print_handle_open = 0;
      return NULL;
    case Hourglass_On: case Hourglass_Off:
      return NULL;
    case 0x49980:                      /* JPEG_Info: the size from the JPEG's frame header */
      {
        img_info ii;
        memset (&ii, 0, sizeof ii);
        if (img_read_info ((const unsigned char *) r[1], (size_t) r[2], &ii))
          return &none;
        r[2] = ii.width; r[3] = ii.height; r[0] = 0;
      }
      return NULL;
    case 0x49982:                      /* JPEG_PlotScaled */
      CHECK (print_handle_open && r[3] == 0, "JPEG_PlotScaled at 1:1, during the job");
      {
        img_info ii;
        memset (&ii, 0, sizeof ii);
        img_read_info ((const unsigned char *) r[0], (size_t) r[4], &ii);
        print_plot_w = ii.width;
        print_plot_h = ii.height;
      }
      print_plots++;
      return NULL;
    }
  (void) pending;
  return NULL;
}

static const _kernel_oserror *graphics (int swi, int r[10])
{
  switch (swi)
    {
    case OS_ReadMonotonicTime:
      r[0] = (int) (clock_cs += 1);    /* time moves while Lumen works */
      return NULL;
    case OS_ReadModeVariable:
      r[2] = r[1] == 4 || r[1] == 5 ? 1 : r[1] == 11 ? 1919 : 1079;
      return NULL;
    case ColourTrans_SetGCOL:
      return NULL;
    case ColourTrans_SelectTable:
      r[4] = 0;                        /* 32bpp to a 32bpp screen: no table */
      return NULL;
    case OS_Plot:
      if (r[0] == 85)
        stars++;
      return NULL;
    case OS_SpriteOp:
      CHECK (r[0] == 52 + 512, "OS_SpriteOp 52, user area, sprite pointer");
      CHECK (r[7] == 0, "no translation table needed");
      {
        const unsigned *area = (const unsigned *) r[1], *sp = (const unsigned *) r[2];
        CHECK (sp == area + 4 && area[1] == 1u && area[2] == 16u, "a valid one-sprite area");
        CHECK (sp[10] == 0x301680B5u && sp[8] == 44u && sp[6] == 0u && sp[7] == 31u, "a 32bpp new-format sprite header");
      }
      if (r[6])
        memcpy (last_scale, (const int *) r[6], sizeof last_scale);
      else                             /* unscaled: a smoothed copy at the size shown */
        {
          const unsigned *sp = (const unsigned *) r[2];
          last_scale[0] = last_scale[1] = last_scale[2] = last_scale[3] = 1;
          smooth_plots++;
          (void) sp;
        }
      {
        const unsigned *sp = (const unsigned *) r[2];
        last_plot_w = (int) sp[4] + 1;
        last_plot_h = (int) sp[5] + 1;
      }
      sprite_plots++;
      if (r[5] & 8)
        masked_plots++;
      return NULL;
    }
  return NULL;
}

static const char *getvar (const char *name)
{
  static char v[700];
  int len = 0;
  if (_swix (OS_ReadVarVal, _INR (0, 4) | _OUT (2), name, v, (int) sizeof v - 1, 0, 0, &len) || len < 0)
    return NULL;
  v[len] = '\0';
  return v;
}

static const _kernel_oserror *setvarval (int swi, int r[10])
{
  char v[700];
  (void) swi;
  if (r[2] < 0)
    fake_setvar ((const char *) r[0], NULL);
  else
    {
      snprintf (v, sizeof v, "%.*s", r[2], (const char *) r[1]);
      fake_setvar ((const char *) r[0], v);
    }
  return NULL;
}

static void fakes (void)
{
  static const struct { int swi; const char *name; int wimp; } list[] = {
    { Wimp_Initialise, "Wimp_Initialise", 1 }, { Wimp_CreateIcon, "Wimp_CreateIcon", 1 },
    { Wimp_CreateWindow, "Wimp_CreateWindow", 1 }, { Wimp_OpenWindow, "Wimp_OpenWindow", 1 },
    { Wimp_GetWindowState, "Wimp_GetWindowState", 1 }, { Wimp_SetExtent, "Wimp_SetExtent", 1 },
    { Wimp_ForceRedraw, "Wimp_ForceRedraw", 1 }, { Wimp_SetCaretPosition, "Wimp_SetCaretPosition", 1 },
    { Wimp_GetCaretPosition, "Wimp_GetCaretPosition", 1 },
    { Wimp_CreateSubMenu, "Wimp_CreateSubMenu", 1 },
    { Wimp_RedrawWindow, "Wimp_RedrawWindow", 1 }, { Wimp_GetRectangle, "Wimp_GetRectangle", 1 },
    { Wimp_TextOp, "Wimp_TextOp", 1 }, { Wimp_SendMessage, "Wimp_SendMessage", 1 },
    { Wimp_CreateMenu, "Wimp_CreateMenu", 1 }, { Wimp_GetPointerInfo, "Wimp_GetPointerInfo", 1 },
    { Wimp_ProcessKey, "Wimp_ProcessKey", 1 }, { Wimp_ReportError, "Wimp_ReportError", 1 },
    { Wimp_CloseWindow, "Wimp_CloseWindow", 1 }, { Wimp_CloseDown, "Wimp_CloseDown", 1 },
    { OS_ReadModeVariable, "OS_ReadModeVariable", 0 }, { ColourTrans_SetGCOL, "ColourTrans_SetGCOL", 0 },
    { ColourTrans_SelectTable, "ColourTrans_SelectTable", 0 }, { OS_Plot, "OS_Plot", 0 },
    { OS_SpriteOp, "OS_SpriteOp", 0 }, { OS_ReadMonotonicTime, "OS_ReadMonotonicTime", 0 },
    { Wimp_DragBox, "Wimp_DragBox", 1 }, { Wimp_SetIconState, "Wimp_SetIconState", 1 },
    { Wimp_SpriteOp, "Wimp_SpriteOp", 1 }, { Wimp_ResizeIcon, "Wimp_ResizeIcon", 1 },
    { Wimp_GetMenuState, "Wimp_GetMenuState", 1 }, { Wimp_GetIconState, "Wimp_GetIconState", 1 },
    { Wimp_StartTask, "Wimp_StartTask", 1 } };
  size_t i;
  fake_reset ();
  for (i = 0; i < sizeof list / sizeof list[0]; i++)
    fake_swi (list[i].swi, list[i].name, list[i].wimp ? wimp : graphics);
  fake_swi (TaskManager_EnumerateTasks, "TaskManager_EnumerateTasks", tasks);
  fake_swi (OS_SetVarVal, "OS_SetVarVal", setvarval);
  {
    static const struct { int swi; const char *name; } pl[] = {
      { PDriver_Info, "PDriver_Info" }, { PDriver_PageSize, "PDriver_PageSize" },
      { PDriver_SelectJob, "PDriver_SelectJob" }, { PDriver_GiveRectangle, "PDriver_GiveRectangle" },
      { PDriver_DrawPage, "PDriver_DrawPage" }, { PDriver_GetRectangle, "PDriver_GetRectangle" },
      { PDriver_EndJob, "PDriver_EndJob" }, { PDriver_AbortJob, "PDriver_AbortJob" }, { OS_Find, "OS_Find" },
      { Hourglass_On, "Hourglass_On" }, { Hourglass_Off, "Hourglass_Off" },
      { 0x49980, "JPEG_Info" }, { 0x49982, "JPEG_PlotScaled" }, { Font_FindFont, "Font_FindFont" },
      { Font_LoseFont, "Font_LoseFont" }, { Font_ScanString, "Font_ScanString" }, { Font_Paint, "Font_Paint" },
      { ColourTrans_SetFontColours, "ColourTrans_SetFontColours" } };
    for (i = 0; i < sizeof pl / sizeof pl[0]; i++)
      fake_swi (pl[i].swi, pl[i].name, printer);
  }
}

static void make_jpeg (const char *path, int w, int h)
{
  struct jpeg_compress_struct c;
  struct jpeg_error_mgr e;
  FILE *f = fopen (path, "wb");
  unsigned char *row = calloc ((size_t) w * 3, 1);
  c.err = jpeg_std_error (&e);
  jpeg_create_compress (&c);
  jpeg_stdio_dest (&c, f);
  c.image_width = (JDIMENSION) w; c.image_height = (JDIMENSION) h;
  c.input_components = 3; c.in_color_space = JCS_RGB;
  jpeg_set_defaults (&c);
  jpeg_start_compress (&c, TRUE);
  while (c.next_scanline < c.image_height)
    {
      JSAMPROW r = row;
      memset (row, (int) (c.next_scanline * 255 / (unsigned) h), (size_t) w * 3);
      jpeg_write_scanlines (&c, &r, 1);
    }
  jpeg_finish_compress (&c);
  jpeg_destroy_compress (&c);
  fclose (f);
  free (row);
}

static int nulls_until_idle (void)
{
  int k = 0, b[64];
  while (app_busy () && k < 20000)
    {
      app_event (Null_Reason, b, 0);
      k++;
    }
  return k;
}

static void redraw (void)
{
  int b[64] = { 0 };
  b[0] = BROWSER;
  sprite_plots = 0;
  ntexts = 0;
  stars = 0;
  app_event (Redraw_Window_Request, b, 0);
}

static int text_drawn (const char *s)
{
  int i;
  for (i = 0; i < ntexts; i++)
    if (strcmp (texts[i], s) == 0)
      return 1;
  return 0;
}

static void click (int wx, int wy, int buttons)
{
  int b[64] = { 0 };
  /* Work area to screen: origin is visible top left minus scroll. */
  b[0] = wx + opened[1] - opened[5];
  b[1] = wy + opened[4] - opened[6];
  b[2] = buttons;
  b[3] = BROWSER;
  app_event (Mouse_Click, b, 0);
}

static void key (int k)
{
  int b[64] = { 0 };
  b[0] = caret_window;                 /* the Wimp sends keys to the caret's window */
  b[1] = caret_icon;                   /* and icon (-1: none) */
  b[6] = k;
  app_event (Key_Pressed, b, 0);
}

/* A key typed into one of our windows (not the caret's). */
static void key_in (int w, int k)
{
  int b[64] = { 0 };
  b[0] = w;
  b[1] = -1;
  b[6] = k;
  app_event (Key_Pressed, b, 0);
}

/* Menu block helpers: a 28-byte header, then 24-byte items. */
static const int *item (const int *menu, int i) { return menu + 7 + 6 * i; }
static const char *item_text (const int *menu, int i) { return (const char *) item (menu, i)[3]; }
static const int *submenu (const int *menu, int i) { return (const int *) item (menu, i)[1]; }

static int history_has (const char *text)
{
  int k;
  for (k = 0; k < activity_history_count (); k++)
    if (strstr (activity_history (k, NULL, NULL), text))
      return 1;
  return 0;
}

static int choose4 (int a, int b, int c, int d)
{
  int blk[64];
  blk[0] = a; blk[1] = b; blk[2] = c; blk[3] = d; blk[4] = -1;
  return app_event (Menu_Selection, blk, 0);
}

/* A click on toolbar button k (as mainwin.c numbers them). */
static void toolbar (int k)
{
  int b[64], tr[4];
  memset (b, 0, sizeof b);
  mainwin_toolbar_button (k, tr);
  b[0] = mopened[1] + (tr[0] + tr[2]) / 2;
  b[1] = mopened[4] - 60;
  b[2] = Button_Select; b[3] = MAINW; b[4] = -1;
  app_event (Mouse_Click, b, 0);
}

static int choose (int a, int b, int c)
{
  int blk[64];
  blk[0] = a; blk[1] = b; blk[2] = c; blk[3] = -1;
  return app_event (Menu_Selection, blk, 0);
}

int main (int argc, char **argv)
{
  char path[700], lib[700];
  int b[64], i;
  cat_item *items;
  int n;
  if (argc < 2)
    return 2;
  snprintf (work, sizeof work, "%s", argv[1]);
  mkdir (work, 0777);
  snprintf (path, sizeof path, "%s/Holiday", work);
  mkdir (path, 0777);
  for (i = 0; i < 6; i++)
    {
      char f[760];
      snprintf (f, sizeof f, "%s/Holiday/IMG_%04d.jpg", work, i + 1);
      make_jpeg (f, 640 + 40 * i, 480);
    }
  snprintf (lib, sizeof lib, "%s/Library", work);

  /* Lumen's directory, with the LibRaw plug-in as PackMan installs it. */
  {
    char d[760], f[800];
    FILE *fp;
    snprintf (d, sizeof d, "%s/App", work);
    setenv ("Lumen$Dir", d, 1);
    snprintf (d, sizeof d, "%s/App/Plugins/LibRaw", work);
    fsys_mkdirs (d);
    snprintf (f, sizeof f, "%s/Plugin,fff", d);
    if ((fp = fopen (f, "w")))
      {
        fputs ("Name: Camera raw (LibRaw)\nVersion: 1.0\nKind: raw\nProgram: LumenRaw\nPackage: LumenRaw\n", fp);
        fclose (fp);
      }
    snprintf (f, sizeof f, "%s/LumenRaw,ff8", d);
    if ((fp = fopen (f, "w")))
      fclose (fp);
    snprintf (d, sizeof d, "%s/PluginChoices", work);
    setenv ("Lumen$PluginChoices", d, 1);
    snprintf (d, sizeof d, "%s/Scratch", work);
    setenv ("Lumen$Scratch", d, 1);
    snprintf (d, sizeof d, "%s/Choices", work);
    setenv ("Lumen$ChoicesFile", d, 1);
    snprintf (d, sizeof d, "%s/Activity", work);
    setenv ("Lumen$ActivityFile", d, 1);
  }
  fakes ();
  CHECK (app_init (lib, NULL) == 0, "app_init succeeds");
  CHECK (strcmp ((const char *) &iconbar[6], "!lumen") == 0 && iconbar[0] == -1, "icon bar icon !lumen, on the right");
  CHECK (nwindows == 15, "the browser, viewer, Export, main, inspector, Add plug-in, Choices, Activity, Print, "
                        "Caption and keywords, Smart album, Export library, Import library, Check library and Info windows");
  CHECK (vopen_calls == 0, "the viewer stays shut at start");
  CHECK (((unsigned) browser_block[7] & 0xBF000002u) == 0x90000002u, "browser: a child with only a vertical scroll bar");
  CHECK (open_calls >= 1 && caret_window == MAINW, "the main window opens with the input focus, the browser in it");
  CHECK (mainwin_mode () == LAYOUT_BROWSER && mainwin_tab () == TAB_LIBRARY && mainwin_inspector_on (),
         "at start: the Browser, the inspector on its Library tab");
  CHECK (iopened[3] - iopened[1] == 640 && opened[1] > iopened[3] && mopened[4] - opened[4] == 120,
         "the inspector on the left, the browser beside it, under the toolbar");
  CHECK (title_buf && strstr (title_buf, "All photos (0)") != NULL, "title: an empty library");
  CHECK (!app_busy (), "nothing to do yet: no null events wanted");
  redraw ();
  CHECK (text_drawn ("Drag photos or a folder of photos here to import them."), "an empty library says what to do");

  /* The Filer drops the folder on the window. */
  memset (b, 0, sizeof b);
  b[0] = 256; b[1] = 0x5678; b[2] = 77; b[3] = 0; b[4] = 3;   /* Message_DataLoad */
  b[5] = BROWSER; b[6] = -1; b[10] = 0x1000;
  snprintf ((char *) &b[11], 200, "%s", path);
  app_event (User_Message_Recorded, b, 0x5678);
  CHECK (sent_reason == User_Message && sent_to == 0x5678, "DataLoadAck sent to the Filer");
  CHECK (sent_block[4] == 4 && sent_block[3] == 77, "action DataLoadAck, your_ref = my_ref");
  CHECK (strstr (title_buf, "Last import") != NULL, "the window switches to Last import");
  CHECK (app_busy (), "busy: null events wanted");

  {
    /* Imported first; then thumbnails nobody is looking at yet. With another
       program in front they're made only in the Wimp's spare time. */
    unsigned w;
    int saved = caret_window, k = 0, waits = 0;
    caret_window = -1;
    while (app_busy () && !(waits = app_background_wait (&w)) && k++ < 20000)
      app_event (Null_Reason, b, 0);
    CHECK (waits, "imported; thumbnails left, Lumen not in front: made in the Wimp's spare time");
    CHECK (waits && (int) (w - clock_cs) > 0, "Wimp_PollIdle a moment between slices");
    caret_window = MAINW;
    CHECK (!app_background_wait (&w), "Lumen in front: on every null event");
    caret_window = saved;
  }
  i = nulls_until_idle ();
  CHECK (i >= 1, "imported and thumbnails made on null events");
  CHECK (!app_busy (), "and finished");
  CHECK (strstr (title_buf, "Last import (6)") != NULL, "title: 6 photos");
  CHECK (title_redraws > 0, "the title bar was redrawn");
  CHECK (extent[1] <= -1080, "extent set");

  /* First redraw: placeholders, thumbnails queued; then they load. */
  redraw ();
  CHECK (sprite_plots == 0, "first redraw: no sprites yet");
  CHECK (text_drawn ("IMG_0001.jpg") && text_drawn ("IMG_0006.jpg"), "names drawn");
  CHECK (stars == 6 * 5 * 10, "five stars of ten triangles under each photo");
  CHECK (app_busy (), "thumbnails shown: loaded on null events");
  force_redraws = 0;
  nulls_until_idle ();
  CHECK (force_redraws >= 6, "each new thumbnail's cell redrawn");
  redraw ();                            /* now loaded for display */
  nulls_until_idle ();
  redraw ();
  CHECK (sprite_plots == 6, "second redraw: six thumbnails");
  CHECK (last_scale[0] == 1 && last_plot_w == 112, "a smoothed copy 112 pixels wide: the 224-unit box at eig 1, plotted 1:1");

  /* Click the first photo, rate it 4, flag it. */
  click (16 + 40, -16 - 40, Button_Select);
  key ('4');
  key ('/');
  {
    cat_source s = { SRC_FLAGGED, 0, 0, "" };
    cat_list (lib_cat (app_library ()), &s, &items, &n);
    CHECK (n == 1 && items[0].rating == 4 && strcmp (items[0].name, "IMG_0001.jpg") == 0, "rated 4 and flagged");
    free (items);
  }
  /* P does what / does (the menus show it as Shift-P). */
  key ('P');
  {
    cat_source s = { SRC_FLAGGED, 0, 0, "" };
    cat_list (lib_cat (app_library ()), &s, &items, &n);
    CHECK (n == 0, "P: unflagged again");
    free (items);
  }
  key ('p');
  /* Right arrow: the next one alone. Ctrl-A, then 2: all rated 2. */
  key (KEY_RIGHT);
  key ('5');
  key (KEY_CTRL_A);
  key ('2');
  {
    cat_source s = { SRC_ALL, 0, 2, "" };
    cat_list (lib_cat (app_library ()), &s, &items, &n);
    CHECK (n == 6, "Ctrl-A then 2: all six rated");
    for (i = 0; i < n; i++)
      CHECK (items[i].rating == 2, "each rated 2");
    free (items);
  }
  key ('j');
  CHECK (processed_key == 'j', "keys Lumen doesn't use are passed on");
  /* Adjust-click on another photo: both cells redrawn (the old focus frame goes). */
  key (KEY_ESCAPE);
  {
    cat_id *none;
    int k = browser_selection (&none);
    free (none);
    CHECK (k == 0 && browser_focus () < 0, "Escape: nothing selected, not even the photo in focus");
  }
  click (16 + 256 + 40, -16 - 40, Button_Select);   /* not the photo clicked earlier: that would be a double click */
  force_redraws = 0;
  click (16 + 2 * 256 + 40, -16 - 40, Button_Adjust);
  CHECK (force_redraws >= 2, "Adjust: the old focus cell and the new one are redrawn");

  /* The window menu. */
  click (5, -5, Button_Menu);
  CHECK (menu_open && strncmp ((const char *) menu_open, "Lumen", 5) == 0, "Menu: the Lumen menu");
  CHECK (strcmp (item_text (menu_open, 0), "Show") == 0 && strcmp (item_text (menu_open, 3), "Versions and stacks") == 0
         && strcmp (item_text (menu_open, 5), "Plug-ins") == 0 && strcmp (item_text (menu_open, 6), "Quit") == 0,
         "Show ... Versions and stacks, Plug-ins, Quit");
  CHECK ((item (menu_open, 6)[0] & 0x80) != 0, "Quit is the last item");
  CHECK (strcmp (item_text (submenu (menu_open, 5), 0), "LumenRaw 1.0") == 0, "Plug-ins > the LibRaw plug-in, by its package's name");
  {
    const int *show = submenu (menu_open, 0), *proj = submenu (show, 4);
    CHECK (strcmp (item_text (show, 1), "Flagged") == 0, "Show > Flagged");
    CHECK (strcmp (item_text (proj, 0), "Holiday (6)") == 0, "Show > Projects > Holiday (6)");
  }
  choose (0, 1, -1);
  CHECK (strstr (title_buf, "Flagged (1)") != NULL, "Show > Flagged: one photo");
  choose (0, 4, 0);
  CHECK (strstr (title_buf, "Holiday (6)") != NULL, "Show > Projects > Holiday");

  /* A new album from all six. */
  key (KEY_CTRL_A);
  click (5, -5, Button_Menu);
  {
    const int *album = submenu (menu_open, 2), *name = submenu (album, 0);
    CHECK ((item (name, 0)[0] & 0x04) != 0, "a writable item for the album's name");
    snprintf ((char *) item (name, 0)[3], 32, "Trip");
  }
  choose (2, 0, 0);
  {
    cat_group *g;
    int ng;
    cat_groups (lib_cat (app_library ()), SRC_ALBUM, &g, &ng);
    CHECK (ng == 1 && strcmp (g[0].name, "Trip") == 0 && g[0].count == 6, "album Trip holds six");
    free (g);
  }
  /* Thumbnail size: Large. */
  choose (4, 2, -1);
  redraw ();
  nulls_until_idle ();
  redraw ();
  CHECK (last_plot_w == 160, "Large: a 320-unit box, 160 pixels");

  /* ---- The viewer ---- */
  {
    int px, tx0, tx1, ev_mid, sx, sy, k;
    static char text[RECIPE_TEXT_MAX];
    cat_item it;
    browser_focus_on (0);
    browser_item (0, &it);
    key (KEY_RETURN);
    CHECK (vopen_calls >= 1 && mainwin_mode () == LAYOUT_VIEWER && caret_window == MAINW,
           "Return: the Viewer layout");
    CHECK (vopened[1] == opened[1] && vopened[3] == mopened[3], "the viewer where the browser was");
    key ('w');
    key ('W');
    CHECK (mainwin_tab () == TAB_ADJUST, "W twice: the inspector's Adjustments tab");
    CHECK (strstr (vtitle, "IMG_0001.jpg (1 of 6)") != NULL, "viewer title: the photo, 1 of 6");
    CHECK (app_busy (), "the proxy is decoded in idle time");
    memset (b, 0, sizeof b);
    b[0] = VIEWER;
    sprite_plots = 0; smooth_plots = 0;
    app_event (Redraw_Window_Request, b, 0);
    CHECK (sprite_plots == 1 && smooth_plots == 1 && last_plot_w > 256, "straight away: the thumbnail, enlarged smoothly");
    i = nulls_until_idle ();
    CHECK (i > 1, "the proxy took several slices");
    {
      int w, h;
      CHECK (viewer_proxy_size (&w, &h) && w == 640 && h == 480, "proxy: the whole 640x480 photo");
    }
    sprite_plots = 0; ntexts = 0;
    app_event (Redraw_Window_Request, b, 0);
    {
      int box_w = (vopened[3] - vopened[1]) - 48, box_h = (vopened[4] - vopened[2]) - 48;
      int want = box_w * 480 <= box_h * 640 ? box_w / 2 : box_h * 640 / 480 / 2;
      CHECK (sprite_plots == 1 && last_plot_w > 640 && abs (last_plot_w - want) <= 1,
             "then the developed proxy, smoothed to the pixels it fills");
    }
    b[0] = INSP;
    ntexts = 0;
    app_event (Redraw_Window_Request, b, 0);
    CHECK (text_drawn ("Exposure") && text_drawn ("White Balance") && text_drawn ("Reset all") && text_drawn ("Split Toning"), "the panel: groups, sliders, buttons");

    /* Drag Exposure from 0 to +1 EV. Layout as viewer.c's, in the inspector. */
    px = 0;
    {
      int iox = iopened[1] - iopened[5], ioy = iopened[4] - iopened[6];
#define PX(x) (iox + (x))
#define PY(y) (ioy + (y))
    tx0 = px + 232; tx1 = px + 640 - 132;
    ev_mid = -24 - 150 - 16 - 52 - 156 - 44 - (52 + 2 * 56 + 8) - 52 - 28;
    sx = PX ((tx0 + tx1) / 2);
    sy = PY (ev_mid);
    memset (b, 0, sizeof b);
    b[0] = sx; b[1] = sy; b[2] = Button_Select; b[3] = INSP; b[4] = -1;
    app_event (Mouse_Click, b, 0);
    CHECK (drag_calls == 1 && drag_block[0] == INSP && drag_block[1] == 7, "a drag starts: Wimp_DragBox type 7");
    CHECK (drag_block[6] <= PX (tx0) && drag_block[8] >= PX (tx1)
           && drag_block[7] < sy && drag_block[9] > sy, "the pointer is kept on the track's row");
    CHECK (app_busy (), "null events while dragging");
    ptr_x = PX (tx0) + (int) ((1.0 + 4) / 8 * (tx1 - tx0));
    ptr_y = sy;
    memset (b, 0, sizeof b);
    app_event (Null_Reason, b, 0);
    CHECK (viewer_recipe ()->v[P_EV] > 0.9f && viewer_recipe ()->v[P_EV] < 1.1f, "dragging moves exposure to +1");
    cat_get_recipe (lib_cat (app_library ()), it.id, text, sizeof text);
    CHECK (text[0] == '\0', "nothing saved until the button is let go");
    app_event (User_Drag_Box, b, 0);
    cat_get_recipe (lib_cat (app_library ()), it.id, text, sizeof text);
    CHECK (strncmp (text, "exposure ev=", 12) == 0, "let go: saved");
    CHECK (strstr (vtitle, "edited") != NULL, "the title says edited");
    CHECK (!viewer_before (), "showing the edit");
    key ('m');
    CHECK (viewer_before (), "M: before");
    key ('M');
    CHECK (!viewer_before (), "M again: after");
    key (0x1A);
    cat_get_recipe (lib_cat (app_library ()), it.id, text, sizeof text);
    CHECK (text[0] == '\0' && viewer_recipe ()->v[P_EV] == 0, "Ctrl-Z: undone, and saved");
    key (0x19);
    cat_get_recipe (lib_cat (app_library ()), it.id, text, sizeof text);
    CHECK (strncmp (text, "exposure ev=", 12) == 0, "Ctrl-Y: redone");
    /* Adjust on a slider: back to its default. */
    memset (b, 0, sizeof b);
    b[0] = sx; b[1] = sy; b[2] = Button_Adjust; b[3] = INSP; b[4] = -1;
    app_event (Mouse_Click, b, 0);
    CHECK (viewer_recipe ()->v[P_EV] == 0 && drag_calls == 1, "Adjust: back to 0, no drag");
    key (0x1A);
    CHECK (viewer_recipe ()->v[P_EV] > 0.9f, "undo brings +1 back");
    /* Reset all, then undo it. */
    memset (b, 0, sizeof b);
    {
      int bx, by;
      viewer_button_centre (2, &bx, &by);
      b[0] = PX (bx); b[1] = PY (by);
    }
    b[2] = Button_Select; b[3] = INSP; b[4] = -1;
    app_event (Mouse_Click, b, 0);
    CHECK (recipe_is_default (viewer_recipe ()), "Reset all");
    key (0x1A);
    CHECK (viewer_recipe ()->v[P_EV] > 0.9f, "and undo it");
    /* Fold the White Balance group by clicking its name: Exposure moves up. */
    {
      int wb_top = PY (-24 - 150 - 16 - 52 - 156 - 44);
      memset (b, 0, sizeof b);
      b[0] = PX (px + 24 + 60); b[1] = wb_top - 26; b[2] = Button_Select; b[3] = INSP; b[4] = -1;
      app_event (Mouse_Click, b, 0);
      memset (b, 0, sizeof b);
      b[0] = sx; b[1] = sy + 2 * 56; b[2] = Button_Adjust; b[3] = INSP; b[4] = -1;
      app_event (Mouse_Click, b, 0);
      CHECK (viewer_recipe ()->v[P_EV] == 0, "White Balance folded: Exposure's slider is two rows higher");
      key (0x1A);
      memset (b, 0, sizeof b);
      b[0] = PX (px + 24 + 60); b[1] = wb_top - 26; b[2] = Button_Select; b[3] = INSP; b[4] = -1;
      app_event (Mouse_Click, b, 0);
      CHECK (viewer_recipe ()->v[P_EV] > 0.9f, "and unfolded again");
      /* Adjust on its name: a concertina, only White Balance open; again, all open. */
      b[2] = Button_Adjust;
      app_event (Mouse_Click, b, 0);
      ntexts = 0;
      memset (b, 0, sizeof b);
      b[0] = INSP;
      app_event (Redraw_Window_Request, b, 0);
      CHECK (text_drawn ("Temperature") && text_drawn ("Split Toning") && !text_drawn ("Highlights") && !text_drawn ("Vibrancy"),
             "Adjust on a group's name: it open, the others folded (their names still there)");
      memset (b, 0, sizeof b);
      b[0] = PX (px + 24 + 60); b[1] = wb_top - 26; b[2] = Button_Adjust; b[3] = INSP; b[4] = -1;
      app_event (Mouse_Click, b, 0);
      ntexts = 0;
      memset (b, 0, sizeof b);
      b[0] = INSP;
      app_event (Redraw_Window_Request, b, 0);
      CHECK (text_drawn ("Temperature") && text_drawn ("Highlights") && text_drawn ("Vibrancy"), "Adjust again: all open");
    }
    /* Curves: click the middle of the graph and drag up. */
    {
      int cr[4], k0 = drag_calls;
      CHECK (viewer_curve_rect (cr), "the curves editor is open");
      memset (b, 0, sizeof b);
      b[0] = PX ((cr[0] + cr[2]) / 2); b[1] = PY ((cr[1] + cr[3]) / 2);
      b[2] = Button_Select; b[3] = INSP; b[4] = -1;
      app_event (Mouse_Click, b, 0);
      CHECK (drag_calls == k0 + 1 && viewer_recipe ()->curve[CURVE_RGB].n == 3, "a click adds a point and drags it");
      ptr_x = b[0]; ptr_y = PY (cr[1] + (cr[3] - cr[1]) * 3 / 4);
      memset (b, 0, sizeof b);
      app_event (Null_Reason, b, 0);
      CHECK (fabsf (viewer_recipe ()->curve[CURVE_RGB].y[1] - 0.75f) < 0.01f, "the point follows the pointer up");
      app_event (User_Drag_Box, b, 0);
      cat_get_recipe (lib_cat (app_library ()), it.id, text, sizeof text);
      CHECK (strstr (text, "curves rgb=0.000:0.000,0.500:0.750,1.000:1.000") != NULL, "let go: saved");
      memset (b, 0, sizeof b);
      b[0] = ptr_x; b[1] = ptr_y; b[2] = Button_Adjust; b[3] = INSP; b[4] = -1;
      app_event (Mouse_Click, b, 0);
      CHECK (viewer_recipe ()->curve[CURVE_RGB].n == 0, "Adjust on the point: gone, a straight line again");
      key (0x1A);
      CHECK (viewer_recipe ()->curve[CURVE_RGB].n == 3, "and undo brings it back");
      key (0x1A);
      CHECK (viewer_recipe ()->curve[CURVE_RGB].n == 0 && viewer_recipe ()->v[P_EV] > 0.9f, "undo again: as before the curve");
    }
    /* The colour label's dots beside the rating: pick Blue, then Blue again for none. */
    {
      cat_item li;
      memset (b, 0, sizeof b);
      b[0] = PX (250 + 5 * 24); b[1] = PY (-24 - 150 - 16 - 24); b[2] = Button_Select; b[3] = INSP; b[4] = -1;
      app_event (Mouse_Click, b, 0);
      browser_item (viewer_index (), &li);
      CHECK (li.label == CAT_LABEL_BLUE, "the panel's blue dot: labelled blue");
      app_event (Mouse_Click, b, 0);
      browser_item (viewer_index (), &li);
      CHECK (li.label == 0, "the same dot again: no label");
    }
    /* Colour: Blue's sliders show first; pick Green, drag its Saturation to -100. */
    {
      int sw[4], top, k0 = drag_calls;
      CHECK (viewer_row_top (HSL_PARAM (HSL_BLUE, HSL_SAT)) != 0 && viewer_row_top (HSL_PARAM (HSL_GREEN, HSL_SAT)) == 0,
             "Colour: Blue's three sliders shown, not Green's");
      CHECK (viewer_hsl_swatch (HSL_GREEN, sw), "the band swatches");
      memset (b, 0, sizeof b);
      b[0] = PX ((sw[0] + sw[2]) / 2); b[1] = PY ((sw[1] + sw[3]) / 2); b[2] = Button_Select; b[3] = INSP; b[4] = -1;
      app_event (Mouse_Click, b, 0);
      top = viewer_row_top (HSL_PARAM (HSL_GREEN, HSL_SAT));
      CHECK (top != 0 && viewer_row_top (HSL_PARAM (HSL_BLUE, HSL_SAT)) == 0, "Green chosen: its sliders instead");
      memset (b, 0, sizeof b);
      b[0] = sx; b[1] = PY (top - 28); b[2] = Button_Select; b[3] = INSP; b[4] = -1;
      app_event (Mouse_Click, b, 0);
      CHECK (drag_calls == k0 + 1, "a drag on Green's Saturation");
      ptr_x = PX (tx0) - 40; ptr_y = PY (top - 28);
      memset (b, 0, sizeof b);
      app_event (Null_Reason, b, 0);
      app_event (User_Drag_Box, b, 0);
      cat_get_recipe (lib_cat (app_library ()), it.id, text, sizeof text);
      CHECK (viewer_recipe ()->v[HSL_PARAM (HSL_GREEN, HSL_SAT)] == -100 && strstr (text, "hsl green_saturation=-100") != NULL,
             "dragged to the left end: -100, saved");
      b[0] = PX ((sw[0] + sw[2]) / 2); b[1] = PY ((sw[1] + sw[3]) / 2);
      ntexts = 0;
      b[0] = INSP;
      app_event (Redraw_Window_Request, b, 0);
      CHECK (text_drawn ("Green") && text_drawn ("Saturation") && text_drawn ("Noise Reduction") && text_drawn ("Vignette")
           && !text_drawn ("Raw"),
             "drawn: the band's name and sliders; Noise Reduction and Vignette groups (no Raw: a JPEG)");
      key (0x1A);
      CHECK (viewer_recipe ()->v[HSL_PARAM (HSL_GREEN, HSL_SAT)] == 0, "undone");
    }
#undef PX
#undef PY
    }

    /* Rate it from the viewer. */
    key ('5');
    browser_item (0, &it);
    CHECK (it.rating == 5, "5 in the viewer: rated 5");
    redraw ();                          /* the Browser asks for its new thumbnail */
    nulls_until_idle ();
    browser_item (0, &it);
    CHECK (it.edited && it.thumb_state == THUMB_READY, "the thumbnail is made again, marked edited");
    key (KEY_RIGHT);
    CHECK (strstr (vtitle, "(2 of 6)") != NULL, "right: the next photo");
    nulls_until_idle ();
    key (KEY_LEFT);
    CHECK (strstr (vtitle, "IMG_0001.jpg (1 of 6)") != NULL, "left: back");
    CHECK (viewer_recipe ()->v[P_EV] > 0.9f, "with its edit");
    nulls_until_idle ();

    /* ---- Tools: auto, crop, rotate, grid, white balance, presets ---- */
    {
      int pr[4], gw2, gh2, vw = vopened[3] - vopened[1], vh = vopened[4] - vopened[2];
      const int *pm, *save;
      recipe kept;
      /* Where the photo sits (as viewer.c's photo_rect), in screen coordinates. */
#define MB_SHOW_TEST 4
#define MB_GRADIENT_TEST 1
#define MB_DELETE_TEST 5
#define PHOTO_RECT()                                                          \
      do {                                                                    \
        int bw = vw - 48, bh = vh - 48, mul, div, dw, dh;               \
        viewer_shown_size (&gw2, &gh2);                                       \
        if ((long long) bw * gh2 <= (long long) bh * gw2) { mul = bw; div = gw2; } \
        else { mul = bh; div = gh2; }                                         \
        dw = gw2 * mul / div; dh = gh2 * mul / div;                           \
        pr[0] = vopened[1] + 24 + (bw - dw) / 2;                              \
        pr[1] = vopened[4] - vh + 24 + (bh - dh) / 2;                         \
        pr[2] = pr[0] + dw; pr[3] = pr[1] + dh;                               \
      } while (0)

      key ('a');
      CHECK (viewer_recipe ()->v[P_EV] != 1 || viewer_recipe ()->v[P_CONTRAST] != 0, "A: auto adjust changes the look");
      cat_get_recipe (lib_cat (app_library ()), it.id, text, sizeof text);
      CHECK (strstr (text, "enhance") != NULL || strstr (text, "levels") != NULL, "and it is saved");
      key (0x1A);
      CHECK (fabsf (viewer_recipe ()->v[P_EV] - 1) < 0.01f && viewer_recipe ()->v[P_CONTRAST] == 0, "undo: auto is one step");

      /* Crop: drag the top left corner to the middle, Return. */
      key ('c');
      CHECK (viewer_crop_mode () && strstr (vtitle, "cropping") != NULL, "C: the crop tool");
      CHECK (viewer_shown_size (&gw2, &gh2) && gw2 == 640 && gh2 == 480, "it shows the whole picture");
      PHOTO_RECT ();
      memset (b, 0, sizeof b);
      b[0] = pr[0] + 2; b[1] = pr[3] - 2; b[2] = Button_Select; b[3] = VIEWER; b[4] = -1;
      k = drag_calls;
      app_event (Mouse_Click, b, 0);
      CHECK (drag_calls == k + 1 && drag_block[6] == pr[0] && drag_block[8] == pr[2], "the corner drags, kept on the photo");
      ptr_x = (pr[0] + pr[2]) / 2; ptr_y = (pr[1] + pr[3]) / 2;
      app_event (Null_Reason, b, 0);
      CHECK (fabsf (viewer_recipe ()->v[P_CROP_X0] - 0.5f) < 0.01f && fabsf (viewer_recipe ()->v[P_CROP_Y0] - 0.5f) < 0.01f
             && viewer_recipe ()->v[P_CROP_X1] == 1, "the top left corner follows the pointer");
      app_event (User_Drag_Box, b, 0);
      cat_get_recipe (lib_cat (app_library ()), it.id, text, sizeof text);
      CHECK (strstr (text, "crop") == NULL, "nothing saved until Return");
      key (KEY_RETURN);
      cat_get_recipe (lib_cat (app_library ()), it.id, text, sizeof text);
      CHECK (!viewer_crop_mode () && strstr (text, "crop x0=0.5") != NULL, "Return: cropped, and saved");
      CHECK (viewer_shown_size (&gw2, &gh2) && gw2 == 320 && gh2 == 240, "the viewer shows the crop");

      /* Again, but Escape: the crop stays as it was. */
      key ('C');
      PHOTO_RECT ();
      b[0] = (pr[0] + 3 * pr[2]) / 4; b[1] = pr[1] + (pr[3] - pr[1]) / 4;   /* inside the crop: moves it */
      app_event (Mouse_Click, b, 0);
      ptr_x = b[0] - (pr[2] - pr[0]) / 4; ptr_y = b[1];
      app_event (Null_Reason, b, 0);
      CHECK (fabsf (viewer_recipe ()->v[P_CROP_X0] - 0.25f) < 0.01f && fabsf (viewer_recipe ()->v[P_CROP_X1] - 0.75f) < 0.01f,
             "inside the crop: it moves");
      app_event (User_Drag_Box, b, 0);
      key (KEY_ESCAPE);
      CHECK (!viewer_crop_mode () && vclosed == 0 && fabsf (viewer_recipe ()->v[P_CROP_X0] - 0.5f) < 0.01f,
             "Escape: the old crop back, the viewer still open");

      /* Retouch: a click paints out a spot, a drag a painted one. Masks:
         a drag paints a stroke on a new mask, Adjust takes it away; a
         gradient drawn; a slider; Show mask; Delete. All undone after. */
      {
        int k0, mx, my, mr[4];
        key ('r');
        CHECK (viewer_retouching () && strstr (vtitle, "retouching") != NULL, "R: the Retouch tool");
        PHOTO_RECT ();
        mx = (pr[0] + pr[2]) / 2; my = (pr[1] + pr[3]) / 2;
        memset (b, 0, sizeof b);
        b[0] = mx; b[1] = my; b[2] = Button_Select; b[3] = VIEWER; b[4] = -1;
        ptr_x = mx; ptr_y = my;
        app_event (Mouse_Click, b, 0);
        app_event (User_Drag_Box, b, 0);
        cat_get_recipe (lib_cat (app_library ()), it.id, text, sizeof text);
        CHECK (viewer_recipe ()->nspots == 1 && viewer_recipe ()->spot[0].n == 0 && strstr (text, "spot x=") != NULL
               && strstr (text, " p=") == NULL, "a click: a round spot, saved");
        b[0] = pr[0] + (pr[2] - pr[0]) / 8; b[1] = pr[1] + (pr[3] - pr[1]) / 5;
        ptr_x = b[0]; ptr_y = b[1];
        app_event (Mouse_Click, b, 0);
        ptr_x = mx;
        app_event (Null_Reason, b, 0);
        ptr_x = pr[2] - (pr[2] - pr[0]) / 8;
        app_event (Null_Reason, b, 0);
        app_event (User_Drag_Box, b, 0);
        cat_get_recipe (lib_cat (app_library ()), it.id, text, sizeof text);
        CHECK (viewer_recipe ()->nspots == 2 && viewer_recipe ()->spot[1].n == 3 && strstr (text, " p=") != NULL,
               "a drag: a painted spot along the pointer's path, saved");
        CHECK (fabsf (viewer_recipe ()->spot[1].sy - viewer_recipe ()->spot[1].y) > 0.01f
               || fabsf (viewer_recipe ()->spot[1].sx - viewer_recipe ()->spot[1].x) > 0.01f, "with a source of its own");
        /* Moved: taken hold of anywhere along it. */
        {
          float y0 = viewer_recipe ()->spot[1].y, sy0 = viewer_recipe ()->spot[1].sy;
          b[0] = mx; ptr_x = mx; ptr_y = b[1];
          app_event (Mouse_Click, b, 0);
          ptr_y = b[1] + (pr[3] - pr[1]) / 10;
          app_event (Null_Reason, b, 0);
          app_event (User_Drag_Box, b, 0);
          CHECK (viewer_recipe ()->spot[1].y < y0 - 0.03f && fabsf (viewer_recipe ()->spot[1].sy - sy0 - (viewer_recipe ()->spot[1].y - y0)) < 0.002f,
                 "a painted spot dragged: it and its source move together");
        }
        key ('b');
        CHECK (viewer_brushing () && !viewer_retouching () && strstr (vtitle, "layers") != NULL && viewer_mask_chosen () == -1,
               "B: the Layers tool instead, no layer chosen");
        b[0] = pr[0] + (pr[2] - pr[0]) / 8; b[1] = my; b[2] = Button_Select;
        ptr_x = b[0]; ptr_y = my;
        k0 = drag_calls;
        app_event (Mouse_Click, b, 0);
        CHECK (drag_calls == k0 + 1 && viewer_recipe ()->nstrokes == 0, "a press starts a stroke");
        ptr_x = mx; ptr_y = my;
        app_event (Null_Reason, b, 0);
        ptr_x = pr[2] - (pr[2] - pr[0]) / 8;
        app_event (Null_Reason, b, 0);
        app_event (User_Drag_Box, b, 0);
        cat_get_recipe (lib_cat (app_library ()), it.id, text, sizeof text);
        CHECK (viewer_recipe ()->nmasks == 1 && viewer_recipe ()->nstrokes == 1 && viewer_recipe ()->stroke[0].n == 3
               && viewer_recipe ()->stroke[0].kind == BRUSH_ADD && viewer_mask_chosen () == 0
               && strstr (text, "mask k=brush ev=0.50\npaint m=0 k=add a=1.00") != NULL,
               "let go: a new brush mask (+0.5 EV to start), the stroke along the pointer's path, saved");
        /* The Adjustments tab is the masks' now: a line for the new mask,
           its sliders. Its Exposure slider, at the right end: +4 EV. */
        CHECK (mainwin_tab () == TAB_ADJUST && mainwin_inspector_on (), "the inspector shows the Adjustments tab");
        ntexts = 0;
        b[0] = INSP;
        app_event (Redraw_Window_Request, b, 0);
        CHECK (text_drawn ("Layers") && text_drawn ("Brush 1") && text_drawn ("+0.50 EV") && text_drawn ("Clarity")
               && !text_drawn ("White Balance"), "drawn: Layers, Brush 1 (+0.50 EV), its sliders; not the photo's groups");
#define IX(x) (iopened[1] - iopened[5] + (x))
#define IY(y) (iopened[4] - iopened[6] + (y))
        viewer_mask_box_part (100 + M_EV, mr);
        memset (b, 0, sizeof b);
        b[0] = IX (mr[2]); b[1] = IY ((mr[1] + mr[3]) / 2); b[2] = Button_Select; b[3] = INSP; b[4] = -1;
        ptr_x = b[0]; ptr_y = b[1];
        app_event (Mouse_Click, b, 0);
        app_event (User_Drag_Box, b, 0);
        CHECK (viewer_recipe ()->mask[0].v[M_EV] == 4, "the mask's Exposure slider: +4 EV");
        b[2] = Button_Adjust;
        app_event (Mouse_Click, b, 0);
        CHECK (viewer_recipe ()->mask[0].v[M_EV] == 0, "Adjust on it: none");
        b[2] = Button_Select;
        viewer_mask_box_part (100 + M_SHADOWS, mr);
        b[0] = IX ((3 * mr[0] + 5 * mr[2]) / 8); b[1] = IY ((mr[1] + mr[3]) / 2);
        app_event (Mouse_Click, b, 0);
        app_event (User_Drag_Box, b, 0);
        CHECK (viewer_recipe ()->mask[0].v[M_SHADOWS] > 15 && viewer_recipe ()->mask[0].v[M_SHADOWS] < 35, "and Shadows, a quarter up");
        /* Show mask: the chosen one in red. */
        viewer_mask_box_part (MB_SHOW_TEST, mr);
        b[0] = IX ((mr[0] + mr[2]) / 2); b[1] = IY ((mr[1] + mr[3]) / 2);
        app_event (Mouse_Click, b, 0);
        CHECK (viewer_mask_showing (), "Show mask: shown");
        app_event (Mouse_Click, b, 0);
        CHECK (!viewer_mask_showing (), "and not");
        /* Its tick: switched off (kept), then on. */
        viewer_mask_box_part (300, mr);
        b[0] = IX ((mr[0] + mr[2]) / 2); b[1] = IY ((mr[1] + mr[3]) / 2);
        app_event (Mouse_Click, b, 0);
        cat_get_recipe (lib_cat (app_library ()), it.id, text, sizeof text);
        CHECK (viewer_recipe ()->mask[0].off && strstr (text, "mask k=brush off=1") != NULL, "its tick: switched off, saved");
        app_event (Mouse_Click, b, 0);
        CHECK (!viewer_recipe ()->mask[0].off, "and on");
        b[3] = VIEWER;
        /* Adjust on the stroke: taken away (the mask stays). */
        b[0] = (mx + pr[2] - (pr[2] - pr[0]) / 8) / 2; b[1] = my; b[2] = Button_Adjust;
        app_event (Mouse_Click, b, 0);
        CHECK (viewer_recipe ()->nstrokes == 0 && viewer_recipe ()->nmasks == 1 && viewer_recipe ()->nspots == 2,
               "Adjust on the stroke: taken away");
        /* New: a gradient, drawn from the top down to the middle. */
        viewer_mask_box_part (MB_GRADIENT_TEST, mr);
        b[0] = IX ((mr[0] + mr[2]) / 2); b[1] = IY ((mr[1] + mr[3]) / 2); b[2] = Button_Select; b[3] = INSP;
        app_event (Mouse_Click, b, 0);
        b[3] = VIEWER;
        b[0] = mx; b[1] = pr[3] - 4;
        ptr_x = mx; ptr_y = b[1];
        app_event (Mouse_Click, b, 0);
        ptr_y = my;
        app_event (Null_Reason, b, 0);
        app_event (User_Drag_Box, b, 0);
        cat_get_recipe (lib_cat (app_library ()), it.id, text, sizeof text);
        CHECK (viewer_recipe ()->nmasks == 2 && viewer_recipe ()->mask[1].kind == MASK_GRADIENT && viewer_mask_chosen () == 1
               && fabsf (viewer_recipe ()->mask[1].y0 - 0.5f) < 0.02f && fabsf (viewer_recipe ()->mask[1].y1 - 0.75f) < 0.02f
               && viewer_recipe ()->mask[1].v[M_SHADOWS] == viewer_recipe ()->mask[0].v[M_SHADOWS]
               && strstr (text, "mask k=gradient") != NULL, "a gradient drawn (the photo is cropped to its lower right quarter), starting like the last mask changed");
        /* Its end dragged lower. */
        {
          float y1 = viewer_recipe ()->mask[1].y1;
          b[0] = mx; b[1] = my;
          ptr_x = mx; ptr_y = my;
          app_event (Mouse_Click, b, 0);
          ptr_y = my - (pr[3] - pr[1]) / 5;
          app_event (Null_Reason, b, 0);
          app_event (User_Drag_Box, b, 0);
          CHECK (viewer_recipe ()->mask[1].y1 > y1 + 0.05f && viewer_recipe ()->nstrokes == 0, "its end handle dragged");
        }
        /* Delete: the chosen mask goes. */
        /* The list: Brush 1 chosen again from its line, then Gradient 2. */
        viewer_mask_box_part (200, mr);
        b[0] = IX ((mr[0] + mr[2]) / 2); b[1] = IY ((mr[1] + mr[3]) / 2); b[3] = INSP;
        app_event (Mouse_Click, b, 0);
        CHECK (viewer_mask_chosen () == 0, "a mask's line in the list: chosen");
        viewer_mask_box_part (201, mr);
        b[0] = IX ((mr[0] + mr[2]) / 2); b[1] = IY ((mr[1] + mr[3]) / 2);
        app_event (Mouse_Click, b, 0);
        CHECK (viewer_mask_chosen () == 1, "and the next");
        viewer_mask_box_part (MB_DELETE_TEST, mr);
        b[0] = IX ((mr[0] + mr[2]) / 2); b[1] = IY ((mr[1] + mr[3]) / 2);
        app_event (Mouse_Click, b, 0);
        b[3] = VIEWER;
        CHECK (viewer_recipe ()->nmasks == 1 && viewer_recipe ()->mask[0].kind == MASK_BRUSH && viewer_mask_chosen () == 0,
               "Delete: the gradient gone, the brush mask chosen");
        key (KEY_ESCAPE);
        CHECK (!viewer_brushing () && !viewer_retouching (), "Escape: the tool put away");
        for (k0 = 0; k0 < 20 && (viewer_recipe ()->nspots || viewer_recipe ()->nmasks); k0++)
          viewer_undo ();
        CHECK (viewer_recipe ()->nspots == 0 && viewer_recipe ()->nmasks == 0 && viewer_recipe ()->nstrokes == 0, "undone");
#undef IX
#undef IY
      }

      /* Crop shape: Square, from the menu. */
      memset (b, 0, sizeof b);
      b[0] = vopened[1] + 200; b[1] = vopened[4] - 200; b[2] = Button_Menu; b[3] = VIEWER;
      app_event (Mouse_Click, b, 0);
      CHECK (strcmp (item_text (menu_open, 14), "Adjust") == 0 && strstr (item_text (submenu (menu_open, 14), 1), "Crop shape") != NULL,
             "Photo > Adjust > Crop shape");
      choose4 (14, 1, 2, -1);
      CHECK (viewer_crop_mode () && fabsf ((viewer_recipe ()->v[P_CROP_X1] - viewer_recipe ()->v[P_CROP_X0]) * 640
                                           - (viewer_recipe ()->v[P_CROP_Y1] - viewer_recipe ()->v[P_CROP_Y0]) * 480) < 2,
             "Square: the crop becomes square");
      key (KEY_RETURN);
      CHECK (viewer_shown_size (&gw2, &gh2) && abs (gw2 - gh2) <= 1 && abs (gw2 - 240) <= 1, "a 240 pixel square");
      viewer_set_crop_shape (0);
      viewer_crop_finish (1);

      /* Turns: the picture and its crop. */
      key (']');
      CHECK (viewer_recipe ()->v[P_ROTATE] == 1, "]: a quarter turn clockwise");
      key ('[');
      key ('[');
      CHECK (viewer_recipe ()->v[P_ROTATE] == 3, "[ twice: anticlockwise");
      key (']');
      CHECK (viewer_recipe ()->v[P_ROTATE] == 0 && fabsf (viewer_recipe ()->v[P_CROP_X0] - 0.5625f) < 0.01f,
             "and back, with the crop where it was");

      /* Grid. */
      key ('g');
      CHECK (viewer_grid () == 1, "G: thirds");
      key ('G'); key ('G'); key ('G');
      CHECK (viewer_grid () == 0, "round to none");

      /* White balance eyedropper. */
      viewer_pick_white ();
      CHECK (viewer_picking (), "the white balance eyedropper");
      PHOTO_RECT ();
      memset (b, 0, sizeof b);
      b[0] = (pr[0] + pr[2]) / 2; b[1] = (pr[1] + pr[3]) / 2; b[2] = Button_Select; b[3] = VIEWER; b[4] = -1;
      k = drag_calls;
      app_event (Mouse_Click, b, 0);
      CHECK (!viewer_picking () && drag_calls == k, "a click picks (no drag), and the eyedropper goes");

      /* Presets: save the look by name, then apply it after a reset. */
      kept = *viewer_recipe ();
      memset (b, 0, sizeof b);
      b[0] = vopened[1] + 200; b[1] = vopened[4] - 200; b[2] = Button_Menu; b[3] = VIEWER;
      app_event (Mouse_Click, b, 0);
      pm = submenu (menu_open, 4);
      CHECK (pm && strcmp (item_text (pm, 0), "Styles") == 0 && strstr (item_text (pm, 1), "No presets") != NULL,
             "Presets: Styles, then none of your own yet");
      CHECK (submenu (pm, 0) && strcmp (item_text (submenu (pm, 0), 0), "Faded Film") == 0
             && strcmp (item_text (submenu (pm, 0), styles_count () - 1), "Platinum Tone") == 0
             && (item (submenu (pm, 0), styles_count () - 1)[0] & 0x80), "Styles: Faded Film ... Platinum Tone");
      save = submenu (pm, 2);
      strcpy ((char *) item (save, 0)[3], "Bright");
      choose (4, 2, 0);
      {
        cat_preset *ps;
        int np;
        char f[1200], line[64];
        FILE *pf;
        presets_list (&ps, &np);
        CHECK (np == 1 && strcmp (ps[0].name, "Bright") == 0 && strstr (ps[0].recipe, "crop") == NULL
               && strstr (ps[0].recipe, "exposure") != NULL, "Save current as: Bright, the look, no crop");
        free (ps);
        cat_presets (lib_cat (app_library ()), &ps, &np);
        CHECK (np == 0, "not in the library's catalogue");
        free (ps);
        snprintf (f, sizeof f, "%s/Presets/Bright", work);
        pf = fopen (f, "r");
        CHECK (pf && fgets (line, sizeof line, pf) && !strcmp (line, "Lumen preset 1\n")
               && fgets (line, sizeof line, pf) && !strcmp (line, "Name: Bright\n"),
               "a Text file, Presets.Bright, beside the Choices");
        if (pf)
          fclose (pf);
      }
      viewer_reset_all ();
      app_event (Mouse_Click, b, 0);
      pm = submenu (menu_open, 4);
      CHECK (strcmp (item_text (pm, 1), "Bright") == 0, "Presets lists it");
      choose (4, 1, -1);
      CHECK (fabsf (viewer_recipe ()->v[P_EV] - kept.v[P_EV]) < 0.01f && viewer_recipe ()->v[P_CROP_X0] == 0,
             "applying it brings the look back (not the crop)");
      {
        /* Sharing: a copy of Bright's file (as from another computer's
           Choices), Bright deleted, then the copy dropped on the icon bar. */
        int pb[64], k2;
        char shared[1200], bright[2048];
        cat_preset *ps;
        int np;
        presets_list (&ps, &np);
        snprintf (bright, sizeof bright, "%s", np ? ps[0].recipe : "");
        free (ps);
        snprintf (shared, sizeof shared, "%s/Show day", work);
        presets_write_file (shared, "Show day", bright);
        /* Delete > Bright */
        app_event (Mouse_Click, b, 0);
        choose (4, 3, 0);
        presets_list (&ps, &np);
        CHECK (np == 0, "Delete > Bright: its file goes");
        free (ps);
        /* The copy dropped on the icon bar icon. */
        memset (pb, 0, sizeof pb);
        k2 = browser_count ();
        pb[1] = 0x7777; pb[2] = 0x6000; pb[4] = 3; pb[5] = -2; pb[6] = 0; pb[10] = 0xFFF;
        strcpy ((char *) &pb[11], shared);
        pb[0] = 44 + (int) ((strlen (shared) + 4) & ~3u);
        app_event (User_Message, pb, 0);
        CHECK (sent_block[4] == 4 && sent_block[3] == 0x6000, "a preset file dropped on the icon bar: DataLoadAck");
        presets_list (&ps, &np);
        CHECK (np == 1 && !strcmp (ps[0].name, "Show day"), "and it's one of the presets");
        free (ps);
        nulls_until_idle ();
        CHECK (browser_count () == k2, "(not imported as a photo)");
        /* A library's presets from before (in its catalogue) move to the directory. */
        cat_preset_save (lib_cat (app_library ()), "Old one", "exposure ev=0.5\n");
        cat_preset_save (lib_cat (app_library ()), "show DAY", "exposure ev=0.9\n");
        app_event (Mouse_Click, b, 0);
        pm = submenu (menu_open, 4);
        CHECK (!strcmp (item_text (pm, 1), "Old one") && !strcmp (item_text (pm, 2), "Show day"),
               "the library's old presets join them (one of a name there already doesn't replace it)");
        cat_presets (lib_cat (app_library ()), &ps, &np);
        CHECK (np == 0, "and leave the catalogue");
        free (ps);
        presets_delete ("Old one");
        presets_delete ("Show day");
        presets_save ("Bright", bright);   /* for Selection > Apply preset, below */
        ptr_window = -1;
      }
      {
        /* A style, then Undo and Redo from the toolbar. */
        int sepia = -1, k;
        for (k = 0; k < styles_count (); k++)
          if (!strcmp (styles_get (k)->name, "Sepia"))
            sepia = k;
        CHECK (mainwin_mode () != LAYOUT_BROWSER, "(the photo is being adjusted)");
        choose (4, 0, sepia);
        CHECK (viewer_recipe ()->v[P_SATURATION] == -100 && viewer_recipe ()->v[P_TONE_SH] > 0
               && viewer_recipe ()->v[P_EV] == 0, "Styles > Sepia: black and white, toned (Bright's exposure replaced)");
        CHECK (viewer_can_undo (), "it can be undone");
        toolbar (3);                     /* Undo */
        CHECK (fabsf (viewer_recipe ()->v[P_EV] - kept.v[P_EV]) < 0.01f && viewer_recipe ()->v[P_TONE_SH] == 0,
               "toolbar Undo: back to Bright");
        toolbar (4);                     /* Redo */
        CHECK (viewer_recipe ()->v[P_TONE_SH] > 0, "toolbar Redo: Sepia again");
        toolbar (3);
        CHECK (fabsf (viewer_recipe ()->v[P_EV] - kept.v[P_EV]) < 0.01f, "and Undo again: Bright");
      }
      {
        /* Styles shown as the pointer goes down the list, kept only when clicked. */
        int k, sepia = -1, cyan = -1;
        char before_text[2048], now_text[2048];
        unsigned wake_at;
        float ev0 = viewer_recipe ()->v[P_EV];
        for (k = 0; k < styles_count (); k++)
          {
            if (!strcmp (styles_get (k)->name, "Sepia")) sepia = k;
            if (!strcmp (styles_get (k)->name, "Cyanotype")) cyan = k;
          }
        cat_get_recipe (lib_cat (app_library ()), viewer_item (), before_text, sizeof before_text);
        memset (b, 0, sizeof b);
        b[0] = vopened[1] + 200; b[1] = vopened[4] - 200; b[2] = Button_Menu; b[3] = VIEWER;
        app_event (Mouse_Click, b, 0);
        CHECK (!menus_watching () && !app_next_wake (&wake_at), "Photo menu open: nothing to watch yet, no extra null events");
        /* The pointer reaches Styles' arrow: the Wimp sends Message_MenuWarning. */
        {
          int w[64];
          memset (w, 0, sizeof w);
          w[0] = 44; w[4] = Message_MenuWarning;
          w[5] = 0x5151; w[6] = 900; w[7] = 700;
          w[8] = 4; w[9] = 0; w[10] = -1;          /* Presets > Styles */
          app_event (User_Message, w, 0);
          CHECK (submenu_opened == 0x5151 && menus_watching () && app_next_wake (&wake_at),
                 "Styles opened by Lumen (Wimp_CreateSubMenu); now it looks at the pointer every 5 cs");
          w[8] = 14; w[9] = 2; w[10] = -1;     /* another submenu (Adjust > Grid): opened, not watched */
          submenu_opened = 0;
          menus_closed ();
          app_event (User_Message, w, 0);
          CHECK (submenu_opened == 0x5151 && !menus_watching (), "Grid's submenu: opened, nothing watched");
          w[8] = 4; w[9] = 0; w[10] = -1;
          app_event (User_Message, w, 0);
        }
        ptr_window = 0x7777; ptr_icon = 3;                  /* a menu's window and item */
        menu_state[0] = 4; menu_state[1] = 0; menu_state[2] = cyan; menu_state[3] = -1;
        app_event (Null_Reason, b, 0);
        cat_get_recipe (lib_cat (app_library ()), viewer_item (), now_text, sizeof now_text);
        CHECK (viewer_previewing () && viewer_recipe ()->v[P_TONE_SH_HUE] == 212, "pointer over Cyanotype: the photo shows it");
        {
          char vt[200];
          viewer_title (vt, sizeof vt);
          CHECK ((strstr (vt, "edited") != NULL) == (strstr (before_text, "exposure") != NULL),
                 "the title says edited only if the photo really is");
        }
        CHECK (!strcmp (before_text, now_text), "and nothing is saved");
        menu_state[2] = sepia;
        app_event (Null_Reason, b, 0);
        CHECK (viewer_recipe ()->v[P_TONE_SH_HUE] == 30, "down to Sepia: Sepia shown");
        menu_state[0] = 0; menu_state[1] = -1; menu_state[2] = -1;
        app_event (Null_Reason, b, 0);
        CHECK (!viewer_previewing () && fabsf (viewer_recipe ()->v[P_EV] - ev0) < 0.01f
               && viewer_recipe ()->v[P_TONE_SH] == 0, "off the list: the photo's own look again");
        menu_state[0] = 4; menu_state[1] = 0; menu_state[2] = cyan; menu_state[3] = -1;
        app_event (Null_Reason, b, 0);
        memset (b, 0, sizeof b);
        b[0] = 20; b[4] = Message_MenusDeleted;
        app_event (User_Message, b, 0);
        CHECK (!viewer_previewing () && !menus_watching () && viewer_recipe ()->v[P_TONE_SH] == 0,
               "menu closed without a choice: the photo's own look");
        /* The menu gone without a message: Wimp_GetMenuState 0 notices. */
        memset (b, 0, sizeof b);
        b[0] = vopened[1] + 200; b[1] = vopened[4] - 200; b[2] = Button_Menu; b[3] = VIEWER;
        app_event (Mouse_Click, b, 0);
        {
          int w[64];
          memset (w, 0, sizeof w);
          w[0] = 44; w[4] = Message_MenuWarning; w[5] = 0x5151; w[8] = 4; w[9] = 0; w[10] = -1;
          app_event (User_Message, w, 0);
        }
        app_event (Null_Reason, b, 0);
        CHECK (viewer_previewing (), "shown");
        menu_open = (void *) -1;
        app_event (Null_Reason, b, 0);
        CHECK (!menus_watching () && !viewer_previewing () && viewer_recipe ()->v[P_TONE_SH] == 0,
               "menu closed with no message: noticed, the photo's own look back, watching stops");
        /* Clicked while shown: kept, as one undo step. */
        memset (b, 0, sizeof b);
        b[0] = vopened[1] + 200; b[1] = vopened[4] - 200; b[2] = Button_Menu; b[3] = VIEWER;
        app_event (Mouse_Click, b, 0);
        {
          int w[64];
          memset (w, 0, sizeof w);
          w[0] = 44; w[4] = Message_MenuWarning; w[5] = 0x5151; w[8] = 4; w[9] = 0; w[10] = -1;
          app_event (User_Message, w, 0);
        }
        app_event (Null_Reason, b, 0);
        CHECK (viewer_previewing (), "shown again");
        choose (4, 0, cyan);
        cat_get_recipe (lib_cat (app_library ()), viewer_item (), now_text, sizeof now_text);
        CHECK (!viewer_previewing () && strstr (now_text, "shadow_hue=212") != NULL, "clicking it keeps it");
        viewer_undo ();
        CHECK (fabsf (viewer_recipe ()->v[P_EV] - ev0) < 0.01f && viewer_recipe ()->v[P_TONE_SH] == 0,
               "and one Undo takes it off");
        ptr_window = -1; ptr_icon = -1;
        menu_state[0] = menu_state[1] = menu_state[2] = menu_state[3] = menu_state[4] = -1;
      }

      /* 100%: Z reads the full-size original, then shows it a pixel a pixel. */
      key ('z');
      CHECK (viewer_zoomed () == 1 && app_busy (), "Z: reading the full-size photo");
      nulls_until_idle ();
      CHECK (viewer_zoomed () == 2, "then at 100%");
      memset (b, 0, sizeof b);
      b[0] = VIEWER;
      sprite_plots = 0; smooth_plots = 0;
      app_event (Redraw_Window_Request, b, 0);
      viewer_shown_size (&gw2, &gh2);
      CHECK (sprite_plots == 1 && smooth_plots == 1 && last_plot_w == gw2 && last_plot_h == gh2,
             "plotted unscaled: the whole (cropped) photo fits the window");
      key (KEY_ESCAPE);
      CHECK (viewer_zoomed () == 0 && vclosed == 0, "Escape: back to fit, still in the viewer");
      PHOTO_RECT ();
      memset (b, 0, sizeof b);
      b[0] = (pr[0] + pr[2]) / 2; b[1] = (pr[1] + pr[3]) / 2; b[2] = Button_Select; b[3] = VIEWER; b[4] = -1;
      app_event (Mouse_Click, b, 0);
      CHECK (viewer_zoomed () == 2, "a click on the photo: 100% (the full size is kept once read)");
      nulls_until_idle ();
      k = drag_calls;
      ptr_x = b[0]; ptr_y = b[1];
      app_event (Mouse_Click, b, 0);
      CHECK (drag_calls == k + 1, "at 100% a click starts dragging the view");
      app_event (User_Drag_Box, b, 0);
      CHECK (viewer_zoomed () == 0, "let go without moving: back to fit");

      /* The loupe: ` turns it on; it follows the pointer over the photo. */
      key ('L');
      CHECK (viewer_loupe () == 1, "L: the loupe");
      key ('l');
      CHECK (viewer_loupe () == 0, "L again: off");
      key ('`');
      CHECK (viewer_loupe () == 1, "`: the loupe, at 100%");
      PHOTO_RECT ();
      ptr_window = VIEWER; ptr_x = (pr[0] + pr[2]) / 2; ptr_y = (pr[1] + pr[3]) / 2;
      memset (b, 0, sizeof b);
      app_event (Null_Reason, b, 0);
      b[0] = VIEWER;
      sprite_plots = 0; masked_plots = 0;
      app_event (Redraw_Window_Request, b, 0);
      CHECK (masked_plots == 1 && last_plot_w == 300, "drawn over the photo: a round (masked) 300-pixel sprite");
      memset (b, 0, sizeof b);
      b[0] = ptr_x; b[1] = ptr_y; b[2] = Button_Adjust; b[3] = VIEWER; b[4] = -1;
      app_event (Mouse_Click, b, 0);
      CHECK (viewer_loupe () == 2, "Adjust on the photo: 200%");
      b[2] = Button_Select;
      app_event (Mouse_Click, b, 0);
      CHECK (viewer_zoomed () == 0 && !app_busy (), "Select pins it (no zoom, no more following)");
      key (KEY_ESCAPE);
      CHECK (viewer_loupe () == 0 && vclosed == 0, "Escape: the loupe goes, the viewer stays");
      ptr_window = -1;

      /* Copy here, paste on photo 2 from the library. */
      key (0x03);
      CHECK (looks_clip () && strstr (looks_clip (), "exposure") != NULL, "Ctrl-C copies the look");
      nulls_until_idle ();
#undef PHOTO_RECT
    }
    key (KEY_ESCAPE);
    CHECK (vclosed == 1 && mainwin_mode () == LAYOUT_BROWSER && browser_focus () == 0, "Escape: back to the Browser, on the photo");
    CHECK (!app_busy (), "nothing left running");

    /* Paste the copied look on photo 2, from the library. */
    {
      cat_item it2;
      browser_focus_on (1);
      key (0x16);
      browser_item (1, &it2);
      cat_get_recipe (lib_cat (app_library ()), it2.id, text, sizeof text);
      CHECK (strstr (text, "exposure") != NULL && strstr (text, "crop") == NULL, "Ctrl-V in the library: photo 2 gets the look");
      /* Selection > Apply preset > Bright, on photo 3. */
      browser_focus_on (2);
      click (16 + 352 + 60, -16 - 60, Button_Menu);
      CHECK (strstr (item_text (submenu (menu_open, 1), 9), "Apply preset") != NULL, "Selection > Apply preset");
      choose (1, 9, 1);
      browser_item (2, &it2);
      cat_get_recipe (lib_cat (app_library ()), it2.id, text, sizeof text);
      CHECK (strstr (text, "exposure") != NULL, "photo 3 gets Bright");
      browser_focus_on (3);
      click (16 + 352 + 60, -16 - 60, Button_Menu);
      choose4 (1, 9, 0, 13);             /* Apply preset > Styles > Cyanotype */
      browser_item (3, &it2);
      cat_get_recipe (lib_cat (app_library ()), it2.id, text, sizeof text);
      CHECK (strstr (text, "tone shadow_hue=212") != NULL && strstr (text, "saturation=-100") != NULL,
             "Selection > Apply preset > Styles > Cyanotype: photo 4 gets it");
      redraw ();
      nulls_until_idle ();
      browser_item (2, &it2);
      CHECK (it2.edited && it2.thumb_state == THUMB_READY, "and a new thumbnail");
      browser_focus_on (0);
    }

    /* ---- Export ---- */
    {
      char path[700], err[200];
      int eb[64];
      FILE *f;
      unsigned char head[8];
      thumbgen *t;
      int w = 0, h = 0, r;
      browser_focus_on (0);
      key (0x05);
      CHECK ((intptr_t) menu_open == EXPORTW, "Ctrl-E: the Export box, opened as a dialogue");
      CHECK (strcmp (exportbox_name (), "IMG_0001") == 0, "named after the photo, without its extension");
      /* PNG, 1024 at most, saved to a typed path. */
      memset (eb, 0, sizeof eb);
      eb[2] = Button_Select; eb[3] = EXPORTW; eb[4] = 4;          /* PNG */
      app_event (Mouse_Click, eb, 0);
      eb[4] = 9;                                                   /* 1024 */
      app_event (Mouse_Click, eb, 0);
      reported[0] = '\0';
      eb[4] = 13;                                                  /* Save, with just a leafname */
      app_event (Mouse_Click, eb, 0);
      CHECK (strstr (reported, "drag the icon") != NULL && !exports_busy (), "Save with a leafname: drag it instead");
      reported[0] = '\0';
      snprintf (path, sizeof path, "%s/IMG_0001-export.png", work);
      exportbox_set_name (path);
      app_event (Mouse_Click, eb, 0);
      CHECK ((intptr_t) menu_open == -1 && exports_busy (), "Save with a full path: the box closes, the export runs");
      browser_update_title ();
      CHECK (activity_busy (), "the status area shows it");
      nulls_until_idle ();
            CHECK (!exports_busy () && history_has ("Exported IMG_0001-export.png"), "done, and in the history");
      f = fopen (path, "rb");
      memset (head, 0, sizeof head);
      if (f) { if (fread (head, 1, 8, f) != 8) head[0] = 0; fclose (f); }
      CHECK (head[1] == 'P' && head[2] == 'N' && head[3] == 'G', "a PNG was written");
      t = thumbgen_start (path, NULL, 1 << 20, 1, err, sizeof err);
      while (t && (r = thumbgen_step (t, 64)) == 0)
        ;
      if (t) thumbgen_size (t, &w, &h);
      thumbgen_free (t);
      CHECK (w == 640 && h == 480, "at the photo's own size (1024 only shrinks larger ones)");

      /* Several photos: a directory of them. */
      browser_select_all (1);
      caret_window = BROWSER;
      key (0x05);
      CHECK (strcmp (exportbox_name (), "Lumen_export") == 0, "several: a directory name");
      snprintf (path, sizeof path, "%s/Batch", work);
      exportbox_set_name (path);
      key (KEY_RETURN);
      nulls_until_idle ();
      {
        int k, all = 1;
        for (k = 1; k <= 6; k++)
          {
            char p2[760];
            snprintf (p2, sizeof p2, "%s/Batch/IMG_%04d.png", work, k);
            f = fopen (p2, "rb");
            if (!f) all = 0; else fclose (f);
          }
        CHECK (all, "six files in the new directory, named after the photos");
      }
      /* Several again: named from a pattern, fitted in a box, Adobe RGB, sharpened. */
      browser_select_all (1);
      caret_window = BROWSER;
      key (0x05);
      memset (eb, 0, sizeof eb);
      eb[2] = Button_Select; eb[3] = EXPORTW;
      eb[4] = 14; app_event (Mouse_Click, eb, 0);                   /* Fit in */
      eb[4] = 21; app_event (Mouse_Click, eb, 0);                   /* Adobe RGB */
      eb[4] = 26; app_event (Mouse_Click, eb, 0);                   /* Standard sharpening */
      exportbox_set_box (200, 100);
      exportbox_set_names ("{project}_{nnn}");
      snprintf (path, sizeof path, "%s/Batch2", work);
      exportbox_set_name (path);
      key (KEY_RETURN);
      nulls_until_idle ();
      {
        char p2[760];
        unsigned char hd[4096];
        size_t got = 0;
        char cname[64];
        snprintf (p2, sizeof p2, "%s/Batch2/Holiday_006.png", work);
        f = fopen (p2, "rb");
        if (f) { got = fread (hd, 1, sizeof hd, f); fclose (f); }
        CHECK (got > 0, "named from the pattern: Holiday_001 ... Holiday_006");
        t = thumbgen_start (p2, NULL, 1 << 20, 1, err, sizeof err);
        while (t && (r = thumbgen_step (t, 64)) == 0)
          ;
        w = h = 0;
        if (t) thumbgen_size (t, &w, &h);
        thumbgen_free (t);
        CHECK (h == 100 && w > 100 && w <= 200, "fitted inside 200 x 100 (photo 6 is cropped wide: 175 x 100)");
        CHECK (got > 0 && memmem (hd, got, "iCCP", 4) != NULL, "with an Adobe RGB profile");
        (void) cname;
        CHECK (app_prefs ()->export_size == -1 && app_prefs ()->export_colour == CS_ADOBE
               && app_prefs ()->export_sharpen == EXPORT_SHARP_STANDARD && !strcmp (app_prefs ()->export_names, "{project}_{nnn}")
               && app_prefs ()->export_box_w == 200, "and these are what the next export starts with (kept in Choices)");
      }
      /* Printing: photo 1 (640 x 480, landscape) on an A4 page (portrait). */
      browser_focus_on (0);
      caret_window = BROWSER;
      key (0x10);
      CHECK ((intptr_t) menu_open == PRINTW, "Ctrl-P: the Print box, as a dialogue");
      CHECK (strstr (printbox_info (), "1 photo, one to a page: 21.0") != NULL, "it says what is printed, on what paper");
      key (KEY_RETURN);
      CHECK (printbox_busy () && (intptr_t) menu_open == -1, "Print: the box closes, the photo is prepared");
      CHECK (activity_busy (), "the status area shows it");
      nulls_until_idle ();
      CHECK (!printbox_busy () && print_jobs == 1 && print_rects == 1 && print_ended == 1 && !print_aborted
             && !print_handle_open, "one job, one page, ended, printer: closed");
      CHECK (print_plots == 1 && print_plot_w == 640, "the photo plotted once, at its own 640 pixels (never enlarged)");
      {
        /* Turned a quarter to suit the portrait page, fitted: the printable area is 566930 x 813544 mp. */
        double aw = 595276 - 2 * 14173, ah = 841890 - 2 * 14173, s = aw / 480 < ah / 640 ? aw / 480 : ah / 640;
        double h = 640 * s, f = h / (640 * 800.0) * 65536;
        CHECK (print_matrix[0] == 0 && print_matrix[3] == 0 && print_matrix[1] > 0 && print_matrix[2] == -print_matrix[1],
               "turned a quarter (matrix 0, s, -s, 0)");
        CHECK (fabs (print_matrix[1] - f) < f * 0.01, "scaled so the photo fills the page's width");
        CHECK (print_rect[0] == 0 && print_rect[2] == 1280 && print_rect[3] == 960, "the drawing: 640 x 480 pixels, 2 OS units each");
        CHECK (abs (print_pos[0] - 595276 + 14173) < 600, "placed from the right-hand margin (turned)");
      }
      CHECK (history_has ("Printed 1 photo on Fake printer"), "in the history");
      {
        char p2[760];
        snprintf (p2, sizeof p2, "%s/LumenPrint-P0.jpg", work);
        f = fopen (p2, "rb");
        CHECK (!f, "the prepared copy is deleted afterwards");
        if (f) fclose (f);
      }
      /* Not turned, at a given size: 10 x 7.5 cm. */
      print_turn = 0;
      caret_window = BROWSER;
      key (0x10);
      eb[3] = PRINTW;
      eb[4] = 12; app_event (Mouse_Click, eb, 0);                   /* untick Turn */
      eb[4] = 7; app_event (Mouse_Click, eb, 0);                    /* the given size */
      eb[3] = EXPORTW;
      key (KEY_RETURN);
      nulls_until_idle ();
      CHECK (print_matrix[1] == 0 && print_matrix[0] > 0, "not turned when told not to");
      {
        double w = 10 / 2.54 * 72000 * 640 / 480, f = w / (640 * 800.0) * 65536;   /* 4:3 in 15 x 10 cm: the height decides */
        CHECK (fabs (print_matrix[0] - f) < f * 0.01, "at most 15 x 10 cm: 13.3 x 10 cm");
      }
      print_turn = 1u << 21;
      /* A printer set to grey only (Colour unticked in !Printers): says so. */
      printer_colour = 0;
      caret_window = BROWSER;
      key (0x10);
      CHECK ((intptr_t) menu_open == PRINTW && strstr (printbox_info (), "Grey only: tick Colour in !Printers") != NULL,
             "a black-and-white printer: says how to print in colour");
      eb[3] = PRINTW;
      eb[4] = 15; app_event (Mouse_Click, eb, 0);                   /* Cancel */
      eb[3] = EXPORTW;
      CHECK ((intptr_t) menu_open != PRINTW && !printbox_busy (), "Cancel: nothing printed");
      printer_colour = 1;
      /* Several to a page: the layouts on A4's printable area (5 mm margins). */
      {
        int A[4] = { 14173, 14173, 595276 - 14173, 841890 - 14173 }, cells[64][4], n, k, ok = 1, gap = 14173;
        n = print_grid (A, 4, 0, 0, gap, 0, 1, cells, 64);
        for (k = 0; k < n; k++)
          if (cells[k][0] < A[0] || cells[k][1] < A[1] || cells[k][2] > A[2] || cells[k][3] > A[3]
              || abs ((cells[k][2] - cells[k][0]) - (cells[0][2] - cells[0][0])) > 1)
            ok = 0;
        CHECK (n == 4 && ok && abs (cells[1][0] - cells[0][2] - gap) <= 1 && abs (cells[0][1] - cells[2][3] - gap) <= 1,
               "4 to a page: 2 x 2 equal cells, inside the margins, a gap between them");
        n = print_grid (A, 2, 0, 0, gap, 0, 1, cells, 64);
        CHECK (n == 2 && cells[0][1] > cells[1][3] && cells[0][0] == cells[1][0], "2 on a portrait page: one above the other");
        n = print_grid (A, 6, 0, 0, gap, 0, 1, cells, 64);
        CHECK (n == 6 && cells[1][0] > cells[0][2] && cells[2][3] < cells[0][1], "6: two across, three down");
        CHECK (print_grid (A, 0, 15, 10, gap, 0, 1, cells, 64) == 2, "as many 15 x 10 cm as fit on A4: 2");
        CHECK (print_grid (A, 0, 6, 4, gap, 0, 1, cells, 64) == 18, "as many 6 x 4 cm: 18 (3 across, 6 down)");
        CHECK (print_grid (A, 0, 25, 21, gap, 0, 1, cells, 64) == 0 && print_grid (A, 0, 25, 20, gap, 0, 1, cells, 64) == 1, "25 x 21 cm: none fit (25 x 20 does, turned)");
        n = print_grid (A, 1, 0, 0, 0, 20000, 1, cells, 64);
        CHECK (n == 1 && cells[0][1] == A[1] + 20000 && cells[0][3] == A[3], "with names: the strip under the photo is kept for its name");
      }
      /* All the photos, 4 to a page, with their names. */
      {
        int n, before_rects = print_rects;
        char says[80];
        browser_select_all (1);
        n = browser_count ();
        caret_window = BROWSER;
        key (0x10);
        eb[3] = PRINTW;
        eb[4] = 20; app_event (Mouse_Click, eb, 0);                 /* 4 per page */
        print_names_opt = 1u << 21;
        eb[4] = 27; app_event (Mouse_Click, eb, 0);                 /* names */
        eb[3] = EXPORTW;
        snprintf (says, sizeof says, "%d photos, 4 to a page: %d pages of 21.0", n, (n + 3) / 4);
        CHECK (strstr (printbox_info (), says) != NULL, "the box says how many to a page, and how many pages");
        print_plots = print_names = print_pages = 0;
        key (KEY_RETURN);
        nulls_until_idle ();
        CHECK (print_pages == (n + 3) / 4 && print_plots == n && print_names == n && print_rects - before_rects == 2 * n,
               "4 photos on each page, each with its name under it");
        snprintf (says, sizeof says, "Printed %d photos, 4 to a page, on Fake printer", n);
        CHECK (history_has (says), "in the history");
        /* Two to a page, filling their cells: only the part of each photo inside its cell is printed. */
        print_names_opt = 0;
        key (0x10);
        eb[3] = PRINTW;
        eb[4] = 27; app_event (Mouse_Click, eb, 0);
        eb[4] = 19; app_event (Mouse_Click, eb, 0);                 /* 2 per page */
        eb[4] = 6; app_event (Mouse_Click, eb, 0);                  /* fill */
        eb[3] = EXPORTW;
        print_pages = 0;
        key (KEY_RETURN);
        nulls_until_idle ();
        CHECK (print_pages == (n + 1) / 2, "2 to a page");
        CHECK (print_rect[2] - print_rect[0] < 2 * print_plot_w || print_rect[3] - print_rect[1] < 2 * print_plot_h,
               "filling a cell: the photo is cut to the cell, not spread over its neighbour");
        /* As many as fit: of the given size. */
        key (0x10);
        eb[3] = PRINTW;
        eb[4] = 23; app_event (Mouse_Click, eb, 0);
        CHECK (strstr (printbox_info (), ", 2 to a page") != NULL, "as many 15 x 10 cm as fit: 2 to a page");
        eb[4] = 18; app_event (Mouse_Click, eb, 0);                 /* back to 1 per page */
        eb[4] = 5; app_event (Mouse_Click, eb, 0);                  /* fit */
        eb[4] = 15; app_event (Mouse_Click, eb, 0);                 /* Cancel */
        eb[3] = EXPORTW;
        browser_select_all (0);
      }
      /* Caption and keywords: Ctrl-K on two photos. */
      {
        cat_id *two;
        char cap[512], kw[1024], says[40];
        int k2, nall;
        browser_select_all (1);
        nall = browser_selection (&two);
        CHECK (nall >= 2, "the photos selected");
        cat_add_keywords (lib_cat (app_library ()), &two[1], 1, "Ferry");   /* only the second has it */
        caret_window = BROWSER;
        tagw_open = 0;
        key (0x0B);
        snprintf (says, sizeof says, "%d photos", nall);
        CHECK (tagw_open && tagbox_is_open () && strstr (tagbox_note (), says) != NULL && !tagbox_keywords_text ()[0],
               "Ctrl-K: the Caption and keywords window, for them all (Ferry isn't shown: only one has it)");
        tagbox_set_texts ("Derwentwater from Friar's Crag", "Lakes, Keswick, lakes");
        eb[3] = TAGW;
        eb[4] = 7; app_event (Mouse_Click, eb, 0);                 /* Save */
        eb[3] = EXPORTW;
        cat_get_tags (lib_cat (app_library ()), two[0], cap, sizeof cap, kw, sizeof kw);
        CHECK (!tagbox_is_open () && !strcmp (cap, "Derwentwater from Friar's Crag") && !strcmp (kw, "Keswick, Lakes"),
               "saved: the caption, two keywords (lakes only once)");
        cat_get_tags (lib_cat (app_library ()), two[1], cap, sizeof cap, kw, sizeof kw);
        CHECK (!strcmp (kw, "Ferry, Keswick, Lakes"), "and the second keeps its own Ferry too");
        /* Again: Lakes taken out of the list goes from both; Ferry stays. */
        caret_window = BROWSER;
        key (0x0B);
        CHECK (!strcmp (tagbox_caption_text (), "Derwentwater from Friar's Crag") && !strcmp (tagbox_keywords_text (), "Keswick, Lakes"),
               "opened again: the caption they share and the keywords both have");
        tagbox_set_texts ("Derwentwater from Friar's Crag", "Keswick");
        /* The keyword menu: the ones in the library; choosing one adds it. */
        eb[3] = TAGW;
        eb[4] = 4; app_event (Mouse_Click, eb, 0);
        eb[3] = EXPORTW;
        CHECK (menu_open && strncmp ((const char *) menu_open, "Keywords", 8) == 0
               && !strcmp (item_text (menu_open, 0), "Ferry") && !strcmp (item_text (menu_open, 2), "Lakes"),
               "the keyword menu lists the library's keywords");
        choose (0, -1, -1);
        CHECK (!strcmp (tagbox_keywords_text (), "Keswick, Ferry"), "Ferry chosen: added to the list");
        key_in (TAGW, KEY_RETURN);
        cat_get_tags (lib_cat (app_library ()), two[0], cap, sizeof cap, kw, sizeof kw);
        CHECK (!strcmp (kw, "Ferry, Keswick"), "Return saves: Lakes gone, Ferry added to the first");
        /* Search finds them; the Info tab shows them. */
        mainwin_set_search ("crag");
        CHECK (browser_count () == nall, "search finds the caption");
        mainwin_set_search ("");
        mainwin_set_tab (TAB_INFO);
        browser_focus_on (0);
        mainwin_info_changed ();
        CHECK (strstr (mainwin_info_text (), "Caption: Derwentwater from Friar's Crag") != NULL
               && strstr (mainwin_info_text (), "Keywords: Ferry, Keswick") != NULL, "the Info tab shows them");
        mainwin_set_tab (TAB_LIBRARY);
        /* A smart album from New: the keyword Ferry, any rating. */
        browser_select_all (0);
        menus_open_new_menu (600, 600);
        CHECK (strstr (item_text (menu_open, 2), "Smart album") != NULL, "New > Smart album...");
        smartw_open = 0;
        choose (2, -1, -1);
        CHECK (smartw_open && smartbox_is_open (), "the Smart album window opens");
        smartbox_set_texts ("Ferries", "ferry", "2026-13", "");
        key_in (SMARTW, KEY_RETURN);
        CHECK (smartbox_is_open () && strstr (smartbox_note (), "date") != NULL, "a date it can't read: it says so, stays open");
        smartbox_set_texts ("Ferries", "ferry", "", "");
        key_in (SMARTW, KEY_RETURN);
        CHECK (!smartbox_is_open () && browser_source ()->kind == SRC_ALBUM && browser_count () == nall,
               "Create: the album shows those with Ferry");
        {
          cat_group *g2;
          int ng2, found = 0;
          cat_id smart = browser_source ()->id;
          cat_groups (lib_cat (app_library ()), SRC_ALBUM, &g2, &ng2);
          for (k2 = 0; k2 < ng2; k2++)
            if (g2[k2].id == smart && g2[k2].smart && g2[k2].count == nall && !strcmp (g2[k2].name, "Ferries"))
              found = 1;
          CHECK (found, "listed in the Library tab as a smart album, with its count");
          free (g2);
          /* Its own menu: Edit smart album...; the window shows its rule. */
          menus_open_group_menu (500, 500, SRC_ALBUM, smart);
          CHECK (!strncmp ((const char *) menu_open, "Smart album", 11) && strstr (item_text (menu_open, 2), "Edit smart album") != NULL,
                 "its menu: Edit smart album...");
          choose (2, -1, -1);
          CHECK (smartbox_is_open (), "the window opens for it");
          /* Only flagged: none now. */
          smart_icon_flags[12] = 1u << 21;
          key_in (SMARTW, KEY_RETURN);
          {
            cat_source fl = { SRC_FLAGGED, 0, 0, "ferry" };
            cat_item *fi;
            int nfl = -1;
            cat_list (lib_cat (app_library ()), &fl, &fi, &nfl);
            free (fi);
            CHECK (browser_count () == nfl && nfl < nall, "flagged only: just the flagged ones");
          }
          {
            cat_rule rr;
            CHECK (cat_smart_get (lib_cat (app_library ()), smart, &rr) == 1 && rr.flagged && !strcmp (rr.words, "ferry"),
                   "the rule saved");
          }
          smart_icon_flags[12] = 0;
          /* Photos can't be added to it by hand: shaded in Album > Add selection to. */
          browser_select_all (1);
          click (5, -5, Button_Menu);
          {
            const int *addto = submenu (submenu (menu_open, 2), 1);
            int shaded = 0;
            for (k2 = 0; k2 < 48; k2++)
              {
                if (strstr (item_text (addto, k2), "Ferries") && (item (addto, k2)[2] & 0x00400000))
                  shaded = 1;
                if (item (addto, k2)[0] & 0x80)   /* the last */
                  break;
              }
            CHECK (shaded, "Album > Add selection to: the smart album is shaded");
            CHECK (strstr (item_text (submenu (menu_open, 2), 3), "New smart album") != NULL, "Album > New smart album...");
          }
          choose (-1, -1, -1);
          browser_select_all (0);
          cat_album_delete (lib_cat (app_library ()), smart);
          {
            cat_source alls = { SRC_ALL, 0, 0, "" };
            browser_set_source (&alls);
          }
        }
        cat_set_caption (lib_cat (app_library ()), two, nall, "");
        cat_remove_keywords (lib_cat (app_library ()), two, nall, "Ferry, Keswick");
        free (two);
      }
      /* Colour labels from the Selection menu; compare two photos. */
      {
        cat_item ci, cj;
        int closed0 = vclosed;
        browser_select_all (0);
        browser_focus_on (2);
        click (5, -5, Button_Menu);
        CHECK (!strcmp (item_text (submenu (menu_open, 1), 1), "Colour label")
               && !strcmp (item_text (submenu (submenu (menu_open, 1), 1), 3), "Yellow"), "Selection > Colour label > Yellow");
        choose (1, 1, 3);
        browser_item (2, &ci);
        CHECK (ci.label == CAT_LABEL_YELLOW, "the photo labelled yellow");
        mainwin_set_tab (TAB_INFO);
        mainwin_info_changed ();
        CHECK (strstr (mainwin_info_text (), "Label: 3") != NULL, "the Info tab has it (its dots)");
        mainwin_set_tab (TAB_LIBRARY);
        /* Two selected, O: the first pinned on the left, the second beside it. */
        browser_focus_on (0);
        browser_toggle_select (3);
        caret_window = BROWSER;
        key ('o');
        nulls_until_idle ();
        browser_item (0, &ci);
        browser_item (3, &cj);
        CHECK (mainwin_mode () == LAYOUT_VIEWER && viewer_comparing () && viewer_compared_item () == ci.id
               && viewer_item () == cj.id, "O with two selected: compared side by side, the last clicked on the right");
        CHECK (strstr (vtitle, "compared with") != NULL, "the title says so");
        caret_window = MAINW;
        key (KEY_RIGHT);
        nulls_until_idle ();
        CHECK (viewer_comparing () && viewer_index () == 4 && viewer_compared_item () == ci.id,
               "Right: the next on the right, the pinned one stays");
        key (KEY_RETURN);
        nulls_until_idle ();
        browser_item (4, &cj);
        CHECK (viewer_compared_item () == cj.id && viewer_index () == 5, "Return: this one pinned, the next beside it");
        key (KEY_ESCAPE);
        CHECK (!viewer_comparing () && mainwin_mode () == LAYOUT_VIEWER, "Escape: comparing ends, still in the Viewer");
        key ('o');
        nulls_until_idle ();
        CHECK (viewer_comparing (), "O in the Viewer: compare with the next");
        key ('z');
        CHECK (!viewer_comparing (), "100% ends comparing");
        key (KEY_ESCAPE);
        key (KEY_ESCAPE);
        CHECK (mainwin_mode () == LAYOUT_BROWSER, "back to the Browser");
        cat_set_label (lib_cat (app_library ()), &ci.id, 1, 0);
        browser_item (2, &ci);
        cat_set_label (lib_cat (app_library ()), &ci.id, 1, 0);
        browser_after_change ();
        browser_select_all (0);
        vclosed = closed0;
      }

      /* Versions and stacks. */
      {
        cat_item vi;
        int before = browser_count (), k3, i0, br[4], closed_before = vclosed;
        cat_id made[4];
        int nmade = 0;
        browser_focus_on (0);
        browser_item (0, &vi);
        caret_window = BROWSER;
        key (0x04);                    /* Ctrl-D */
        CHECK (browser_count () == before + 1 && browser_focus () == 1, "Ctrl-D: a duplicate version, next to it, focused");
        browser_item (1, &vi);
        made[nmade++] = vi.id;
        CHECK (strstr (vi.name, "_v2") != NULL && vi.stack && vi.stack_count == 2 && vi.stack_open && !vi.stack_pick,
               "named _v2, in an open stack of 2");
        browser_item (0, &vi);
        CHECK (vi.stack_pick && vi.stack_count == 2, "the original's version is the pick");
        /* In the Viewer on the version (not the pick): S closes the stack and
           the Viewer moves to the pick, its place right. */
        viewer_open (1);
        caret_window = MAINW;
        key ('s');
        browser_item (0, &vi);
        CHECK (mainwin_mode () == LAYOUT_VIEWER && viewer_index () == 0 && viewer_item () == vi.id && vi.stack_pick,
               "S in the Viewer on a stacked version: closed, the Viewer shows the pick");
        mainwin_set_mode (LAYOUT_BROWSER);
        key ('s');                     /* open again */
        CHECK (browser_count () == before + 1, "S: open again");
        browser_focus_on (0);
        key ('s');                     /* close */
        CHECK (browser_count () == before && browser_focus () == 0, "S: closed, the pick alone, focused");
        redraw ();
        CHECK (browser_badge (0, br), "the pick shows the stack's badge");
        memset (b, 0, sizeof b);
        b[0] = (br[0] + br[2]) / 2; b[1] = (br[1] + br[3]) / 2; b[2] = Button_Select; b[3] = BROWSER;
        app_event (Mouse_Click, b, 0);
        CHECK (browser_count () == before + 1, "a click on the badge opens it");
        /* Make the version the pick, then close: it stands for the stack. */
        browser_focus_on (1);
        key ('\\');
        key ('s');
        browser_item (0, &vi);
        CHECK (browser_count () == before && strstr (vi.name, "_v2") != NULL && vi.stack_pick, "\\: the version the pick");
        key ('s');
        /* Stack two other photos with Ctrl-G, then Ctrl-U. */
        i0 = 2;
        browser_focus_on (i0);
        browser_item (i0 + 1, &vi);
        browser_toggle_select (i0 + 1);   /* as Adjust-click */
        key (0x07);                    /* Ctrl-G */
        browser_item (i0, &vi);
        k3 = vi.stack_count;
        CHECK (k3 == 2 && vi.stack_open, "Ctrl-G: the two in an open stack");
        key (0x15);                    /* Ctrl-U */
        browser_item (i0, &vi);
        CHECK (!vi.stack, "Ctrl-U: unstacked");
        /* The menu: Versions and stacks > New version from original. */
        browser_focus_on (i0);
        click (5, -5, Button_Menu);
        CHECK (!strcmp (item_text (submenu (menu_open, 3), 1), "New version from original"), "Versions and stacks > New version from original");
        choose (3, 1, -1);
        browser_item (browser_focus (), &vi);
        made[nmade++] = vi.id;
        {
          char rec[64];
          cat_get_recipe (lib_cat (app_library ()), vi.id, rec, sizeof rec);
          CHECK (strstr (vi.name, "_v2") != NULL && !rec[0], "a new version from the original: no adjustments");
        }
        /* The Info tab says which version it is. */
        mainwin_set_tab (TAB_INFO);
        mainwin_info_changed ();
        CHECK (strstr (mainwin_info_text (), "Version: 2 of 2 of this photo") != NULL
               && strstr (mainwin_info_text (), "Stack: 2 photos, open") != NULL, "Info: Version 2 of 2, in a stack");
        mainwin_set_tab (TAB_LIBRARY);
        /* In the Viewer: Ctrl-D shows the new version. */
        viewer_open (0);
        caret_window = VIEWER;
        key (0x04);
        CHECK (mainwin_mode () == LAYOUT_VIEWER && viewer_item () != 0, "Ctrl-D in the Viewer");
        {
          cat_item vv;
          browser_item (viewer_index (), &vv);
          CHECK (strstr (vv.name, "_v3") != NULL, "it shows the new version (v3)");
          made[nmade++] = vv.id;
        }
        mainwin_set_mode (LAYOUT_BROWSER);
        /* The Caption window open for a photo that is then removed: it closes. */
        tagbox_open_ids (&made[nmade - 1], 1, 500, 500);
        CHECK (tagbox_is_open (), "the Caption window open for a version");
        browser_removed (&made[nmade - 1], 1);
        CHECK (!tagbox_is_open (), "the version removed: the window closes");
        cat_remove_versions (lib_cat (app_library ()), made, nmade);
        browser_after_change ();
        CHECK (browser_count () == before, "the versions removed: back to as it was");
        browser_select_all (0);
        vclosed = closed_before;       /* the Viewer's closes counted later start from here */
      }
      /* No printer driver: says what to do. */
      have_printer = 0;
      reported[0] = '\0';
      caret_window = BROWSER;
      key (0x10);
      CHECK (strstr (reported, "!Printers") != NULL && (intptr_t) menu_open != PRINTW, "no printer: start !Printers");
      have_printer = 1;

      /* Back as it was, for what follows. */
      key (0x05);
      eb[4] = 4; app_event (Mouse_Click, eb, 0);                    /* PNG */
      eb[4] = 6; app_event (Mouse_Click, eb, 0);                    /* Full */
      eb[4] = 20; app_event (Mouse_Click, eb, 0);                   /* sRGB */
      eb[4] = 24; app_event (Mouse_Click, eb, 0);                   /* no sharpening */
      exportbox_set_names ("{name}");
      eb[4] = 12; app_event (Mouse_Click, eb, 0);                   /* Cancel */

      /* Dragging the icon to a directory display: DataSave, then the Filer's DataSaveAck. */
      browser_focus_on (1);
      caret_window = BROWSER;
      key (0x05);
      eb[2] = 64; eb[4] = 0;                                       /* drag the file icon */
      k = drag_calls;
      app_event (Mouse_Click, eb, 0);
      CHECK (drag_calls == k + 1 && drag_block[1] == 5, "dragging the icon: a fixed box drag");
      ptr_window = FILER; ptr_icon = -1; ptr_x = 900; ptr_y = 500;
      app_event (User_Drag_Box, eb, 0);
      CHECK (sent_block[4] == 1 && sent_to == FILER && sent_block[10] == 0xB60
             && strcmp ((const char *) &sent_block[11], "IMG_0002") == 0, "Message_DataSave to the Filer: PNG, IMG_0002");
      memset (eb, 0, sizeof eb);
      memcpy (eb, sent_block, 44);
      eb[3] = 0x5151; eb[4] = 2; eb[9] = 4321;
      snprintf (path, sizeof path, "%s/Dragged.png", work);
      strcpy ((char *) &eb[11], path);
      eb[0] = 44 + (int) ((strlen (path) + 4) & ~3u);
      app_event (User_Message, eb, 0);
      nulls_until_idle ();
      f = fopen (path, "rb");
      CHECK (f != NULL, "Message_DataSaveAck: written where the Filer said");
      if (f) fclose (f);
      ptr_window = -1;
      caret_window = BROWSER;
      browser_select_all (0);
      browser_focus_on (0);
    }

    /* A double click opens the viewer; its menu takes us back. */
    for (k = 0; k < 2; k++)
      click (16 + 352 + 60, -16 - 60, Button_Select);
    CHECK (mainwin_mode () == LAYOUT_VIEWER && strstr (vtitle, "(2 of 6)") != NULL, "a double click opens the photo clicked");
    memset (b, 0, sizeof b);
    b[0] = vopened[1] + 200; b[1] = vopened[4] - 200; b[2] = Button_Menu; b[3] = VIEWER;
    app_event (Mouse_Click, b, 0);
    CHECK (menu_open && strncmp ((const char *) menu_open, "Photo", 5) == 0, "the viewer's menu: Photo");
    CHECK (!strcmp (item_text (menu_open, 0), "Undo ^Z") && !strcmp (item_text (menu_open, 2), "Auto adjust \x8B" "A")
           && !strcmp (item_text (menu_open, 4), "Presets") && !strcmp (item_text (menu_open, 11), "Export... ^E")
           && !strcmp (item_text (menu_open, 13), "View") && !strcmp (item_text (menu_open, 15), "Versions")
           && (item (menu_open, 16)[0] & 0x80), "Photo menu: the common ones first, then View, Adjust, Versions, Remove");
    CHECK (!strcmp (item_text (menu_open, 6), "Loupe \x8BL") && submenu (menu_open, 6) == submenu (submenu (menu_open, 13), 1),
           "Photo menu: Loupe on it, as well as under View (the same magnifications)");
    CHECK (strstr (item_text (menu_open, 9), "Colour label") != NULL, "Photo menu: Colour label");
    CHECK (!strcmp (item_text (menu_open, 10), "Caption and keywords... ^K"), "Caption and keywords..., its key after one space");
    CHECK (!strcmp (item_text (menu_open, 5), "Before and after \x8BM"), "Before and after: Shift-M shown");
    CHECK (strstr (item_text (submenu (menu_open, 13), 2), "Compare") != NULL, "Photo > View > Compare");
    CHECK (strstr (item_text (submenu (menu_open, 15), 0), "Duplicate version") != NULL, "Photo > Versions > Duplicate version");
    CHECK (strstr (item_text (submenu (menu_open, 14), 3), "Reset all") != NULL, "Photo > Adjust > Reset all adjustments");
    CHECK (strstr (item_text (submenu (menu_open, 13), 3), "Back to the Browser") != NULL, "Back to the Browser");
    {
      /* Loupe from the menu: the toolbar's Loupe button lights up. */
      int r[4];
      mainwin_toolbar_button (8, r);
      nulls_until_idle ();             /* the photo read */
      mainwin_tick ();
      force_redraws = 0;
      choose (6, -1, -1);
      mainwin_tick ();
      CHECK (viewer_loupe () && force_redraws >= 1, "Loupe from the Photo menu: on, and the toolbar's button redrawn");
      viewer_toggle_loupe ();          /* as L or ` does */
      force_redraws = 0;
      mainwin_tick ();
      CHECK (!viewer_loupe () && force_redraws >= 1, "put away by a key: the button redrawn again");
      app_event (Mouse_Click, b, 0);
    }
    choose (13, 3, -1);
    CHECK (vclosed == 2 && mainwin_mode () == LAYOUT_BROWSER, "Back to the Browser: the viewer goes");
    {
      /* Undo and Redo shaded in the Browser: the toolbar draws them grey and a click does nothing. */
      int tr[4], kc = vclosed;
      memset (b, 0, sizeof b);
      b[0] = MAINW;
      ntexts = 0;
      app_event (Redraw_Window_Request, b, 0);
      CHECK (text_drawn ("Undo") && text_drawn ("Redo"), "the toolbar: Undo and Redo");
      mainwin_toolbar_button (3, tr);
      toolbar (3);
      CHECK (vclosed == kc && mainwin_mode () == LAYOUT_BROWSER && !app_busy (), "Undo in the Browser: nothing happens");
    }
  }

  /* ---- The layout: the toolbar, the inspector's tabs, the keys ---- */
  {
    int vb = vopen_calls;
    cat_item it;
/* k: the toolbar as it was (Inspector, Import, New, the layouts, Loupe, Export, Full Screen); Undo and Redo are 3 and 4 now. */
#define TOOL(k) do { int tr[4]; memset (b, 0, sizeof b); \
                     mainwin_toolbar_button ((k) < 3 ? (k) : (k) + 2, tr); \
                     b[0] = mopened[1] + (tr[0] + tr[2]) / 2; \
                     b[1] = mopened[4] - 60; \
                     b[2] = Button_Select; b[3] = MAINW; b[4] = -1; app_event (Mouse_Click, b, 0); } while (0)
#define LIBROW(i) do { memset (b, 0, sizeof b); b[0] = iopened[1] + 200; \
                       b[1] = iopened[4] - iopened[6] - 16 - (i) * 52 - 26; b[2] = Button_Select; b[3] = INSP; \
                       b[4] = -1; app_event (Mouse_Click, b, 0); } while (0)
    mainwin_set_tab (TAB_LIBRARY);
    memset (b, 0, sizeof b);
    b[0] = INSP;
    ntexts = 0;
    app_event (Redraw_Window_Request, b, 0);
    CHECK (text_drawn ("Photos") && text_drawn ("Holiday (6)") && text_drawn ("Trip (6)"),
           "the Library tab: Photos, the projects, the albums");
    LIBROW (8);
    CHECK (browser_source ()->kind == SRC_ALBUM && strstr (title_buf, "Trip (6)") != NULL, "a click on Trip shows the album");
    LIBROW (2);
    CHECK (browser_source ()->kind == SRC_FLAGGED, "and on Flagged");
    LIBROW (1);
    CHECK (browser_source ()->kind == SRC_ALL && browser_count () == 6, "and Photos: all six");
    browser_focus_on (2);
    key ('v');
    CHECK (mainwin_mode () == LAYOUT_SPLIT && vopen_calls > vb && vopened[2] > opened[4],
           "V: Split View, the viewer above the browser");
    CHECK (strstr (vtitle, "IMG_0003.jpg (3 of 6)") != NULL, "showing the focused photo");
    key (KEY_RIGHT);
    CHECK (strstr (vtitle, "(4 of 6)") != NULL && browser_focus () == 3, "right moves the browser's focus; the viewer follows");
    key ('4');
    browser_item (3, &it);
    CHECK (it.rating == 4, "and rates in the browser");
    key ('w');
    CHECK (mainwin_tab () == TAB_INFO, "W: the Info tab");
    memset (b, 0, sizeof b);
    b[0] = INSP;
    ntexts = 0;
    app_event (Redraw_Window_Request, b, 0);
    CHECK (text_drawn ("IMG_0004.jpg") && (text_drawn ("760 x 480 pixels (0.4\xA0MP)") || (text_drawn ("760 x 480 pixels") && text_drawn ("(0.4\xA0MP)"))), "Info: the photo's name and size");
    CHECK (text_drawn ("EXIF") && text_drawn ("No EXIF data in this file."), "and says when the file has no EXIF");
    key ('v');
    CHECK (mainwin_mode () == LAYOUT_VIEWER, "V again: the Viewer");
    key ('v');
    CHECK (mainwin_mode () == LAYOUT_BROWSER && vclosed == 3, "and the Browser");
    key ('i');
    CHECK (!mainwin_inspector_on () && opened[1] < mopened[1] + 100, "I: the inspector goes, the browser takes its room");
    TOOL (0);
    CHECK (mainwin_inspector_on () && opened[1] > iopened[3], "the Inspector button brings it back");
    TOOL (4);
    CHECK (mainwin_mode () == LAYOUT_SPLIT, "the Split View button");
    TOOL (3);
    CHECK (mainwin_mode () == LAYOUT_BROWSER, "the Browser button");
    mainwin_set_tab (TAB_ADJUST);
    CHECK (mainwin_mode () == LAYOUT_SPLIT, "the Adjustments tab from the Browser: Split View, to see the photo");
    TOOL (3);
    TOOL (2);
    CHECK (menu_open && strncmp ((const char *) menu_open, "New", 3) == 0, "New: a menu");
    {
      const int *name = submenu (menu_open, 1);
      snprintf ((char *) item (name, 0)[3], 32, "Empty");
    }
    choose (1, 0, -1);
    {
      cat_group *g;
      int ng;
      cat_groups (lib_cat (app_library ()), SRC_ALBUM, &g, &ng);
      CHECK (ng == 2, "New > Empty album makes one");
      free (g);
    }
    mainwin_set_tab (TAB_LIBRARY);

    /* Search: the field in the bottom bar. A click puts the caret in it
       (the Wimp does that); typing goes in (the Wimp) and each key is told
       to Lumen (KN), which searches. */
    {
      char *field = (char *) win_icon[6];
      int mode0 = mainwin_mode (), bx, by;
      CHECK (win_icon[0] == MAINW && field && strcmp ((const char *) win_icon[7], "Pptr_write;KN") == 0
             && ((unsigned) win_icon[5] & 0xF000u) == 0xF000u, "a writable search field, told of every key");
      CHECK (win_icon[3] < mopened[3] - mopened[1] && win_icon[2] < -(mopened[4] - mopened[2]) + 56,
             "in the bottom bar, at the right");
      memset (b, 0, sizeof b);
      b[0] = mopened[1] + win_icon[1] + 20; b[1] = mopened[4] + win_icon[2] + 20;
      b[2] = Button_Select; b[3] = MAINW; b[4] = 0;
      app_event (Mouse_Click, b, 0);
      CHECK (caret_icon != 0 || caret_window != MAINW || 1, "Lumen leaves the caret to the Wimp");
      caret_window = MAINW; caret_icon = 0;
      strcpy (field, "img_0003");
      key ('3');
      CHECK (browser_count () == 1 && strstr (title_buf, "\"img_0003\"") != NULL, "typing img_0003: one photo, named in the title");
      strcpy (field, "zzz");
      key ('z');
      CHECK (browser_count () == 0, "zzz: none");
      redraw ();
      CHECK (text_drawn ("No photos match \"zzz\"."), "and the browser says so");
      key ('v');
      CHECK (mainwin_mode () == mode0, "V typed in the field is text, not the layout key");
      key (KEY_ESCAPE);
      CHECK (field[0] == '\0' && browser_count () == 6, "Escape: cleared, all six back");
      caret_icon = -1;
      /* The rating filter, left of the field. */
      bx = mopened[1] + win_icon[1] - 200;
      by = mopened[4] + win_icon[2] + 20;
      memset (b, 0, sizeof b);
      b[0] = bx; b[1] = by; b[2] = Button_Select; b[3] = MAINW; b[4] = -1;
      app_event (Mouse_Click, b, 0);
      CHECK (menu_open && strcmp (item_text (menu_open, 4), "4 stars and up") == 0, "the rating button: a menu");
      choose (4, -1, -1);
      CHECK (browser_source ()->min_rating == 4 && browser_count () == 2, "4 stars and up: the two rated 4 and 5");
      app_event (Mouse_Click, b, 0);
      choose (0, -1, -1);
      CHECK (browser_source ()->min_rating == 0 && browser_count () == 6, "Any rating: all six");
    }

    /* Full Screen: only the photo, no frame; Escape comes back. */
    {
      int box[4];
      memcpy (box, &mopened[1], sizeof box);
      browser_focus_on (1);
      key ('f');
      CHECK (mainwin_full_screen () && mainwin_mode () == LAYOUT_VIEWER, "F: full screen, the Viewer");
      CHECK ((main_flags & 0x0F000000u) == 0, "no title, back, close or toggle-size icon");
      CHECK (mopened[1] == 0 && mopened[2] == 0 && mopened[3] == 3840 && mopened[4] == 2160, "the whole screen");
      CHECK (memcmp (&vopened[1], &mopened[1], 4 * sizeof (int)) == 0, "the photo over all of it: no toolbar, no bottom bar");
      CHECK (strstr (vtitle, "IMG_0002.jpg") != NULL, "the focused photo");
      key (KEY_RIGHT);
      CHECK (strstr (vtitle, "(3 of 6)") != NULL, "right: the next photo, still full screen");
      key (KEY_ESCAPE);
      CHECK (!mainwin_full_screen () && mainwin_mode () == LAYOUT_BROWSER && main_flags == 0xAF000002u,
             "Escape: back to the Browser, the frame back");
      CHECK (memcmp (box, &mopened[1], sizeof box) == 0, "where the window was");
      key ('v');
      key ('f');
      key ('f');
      CHECK (!mainwin_full_screen () && mainwin_mode () == LAYOUT_SPLIT, "F twice from Split View: Split View again");
      key ('v');
      key ('v');
    }

    key ('v');
    CHECK (mainwin_mode () == LAYOUT_SPLIT, "Split View again");
    memset (b, 0, sizeof b);
    b[0] = MAINW;
    app_event (Close_Window_Request, b, 0);
    CHECK (mainwin_mode () == LAYOUT_BROWSER && viewer_item () == 0, "closed: the photo let go, Browser next time");
    memset (b, 0, sizeof b);
    b[0] = 1000; b[2] = Button_Select; b[3] = -2;
    app_event (Mouse_Click, b, 0);
    CHECK (mainwin_is_open () && strstr (title_buf, "All photos") != NULL, "the icon bar icon opens it again, on the Browser");
#undef TOOL
#undef LIBROW
  }

  /* ---- A photo whose file has moved ---- */
  {
    char from[760], to[760], dir[760];
    int k, idx = -1;
    cat_item it;
    snprintf (from, sizeof from, "%s/Holiday/IMG_0004.jpg", work);
    snprintf (dir, sizeof dir, "%s/Elsewhere", work);
    mkdir (dir, 0777);
    snprintf (to, sizeof to, "%s/IMG_0004.jpg", dir);
    CHECK (rename (from, to) == 0, "IMG_0004.jpg moved to Elsewhere");
    mainwin_set_mode (LAYOUT_BROWSER);
    mainwin_set_tab (TAB_LIBRARY);
    lib_verify_start (app_library ());
    CHECK (app_busy (), "checking the files wants idle time");
    nulls_until_idle ();
    for (k = 0; k < browser_count (); k++)
      if (browser_item (k, &it) && strcmp (it.name, "IMG_0004.jpg") == 0)
        idx = k;
    CHECK (idx >= 0 && browser_item (idx, &it) && it.missing, "found missing");
    redraw ();
    CHECK (text_drawn ("Missing"), "the Browser marks it");
    memset (b, 0, sizeof b);
    b[0] = INSP;
    ntexts = 0;
    app_event (Redraw_Window_Request, b, 0);
    CHECK (text_drawn ("Missing Files (1)"), "the Library tab lists Missing Files (1)");
    browser_focus_on (idx);
    mainwin_set_tab (TAB_INFO);
    b[0] = INSP;
    ntexts = 0;
    app_event (Redraw_Window_Request, b, 0);
    CHECK (text_drawn ("Missing"), "the Info tab says so");
    viewer_open (idx);
    nulls_until_idle ();
    memset (b, 0, sizeof b);
    b[0] = VIEWER;
    ntexts = 0;
    app_event (Redraw_Window_Request, b, 0);
    {
      int said = 0;
      for (k = 0; k < ntexts; k++)
        if (strstr (texts[k], "can't be found"))
          said = 1;
      CHECK (said, "the Viewer says the file can't be found, and what to do");
    }
    mainwin_set_mode (LAYOUT_BROWSER);
    /* Drag Elsewhere in: found again, nothing added. */
    k = browser_count ();
    memset (b, 0, sizeof b);
    b[0] = 256; b[1] = 0x5678; b[2] = 78; b[3] = 0; b[4] = 3;
    b[5] = BROWSER; b[6] = -1; b[10] = 0x1000;
    snprintf ((char *) &b[11], 200, "%s", dir);
    app_event (User_Message_Recorded, b, 0x5678);
    nulls_until_idle ();
    CHECK (cat_count_missing (lib_cat (app_library ())) == 0, "dragging its new folder in finds it");
    {
      cat_source s = { SRC_ALL, 0, 0, "" };
      browser_set_source (&s);
    }
    CHECK (browser_count () == k, "and adds nothing");
    mainwin_set_tab (TAB_LIBRARY);
  }

  /* ---- Removing photos; renaming and deleting albums and projects ---- */
  {
    cat_group *g;
    int ng, before, k;
    cat_id gone;
    cat_item it;
    mainwin_set_mode (LAYOUT_BROWSER);
    browser_focus_on (5);
    browser_item (5, &it);
    gone = it.id;
    before = browser_count ();
    answer = 2;
    caret_window = MAINW; caret_icon = -1;
    key (0x7F);
    CHECK (strstr (asked, "Remove this photo from the library?") != NULL && strcmp (asked_button, "Remove") == 0,
           "Delete asks first, with a Remove button");
    CHECK (browser_count () == before, "Cancel: nothing removed");
    answer = 3;
    key (0x7F);
    CHECK (browser_count () == before - 1, "Remove: one fewer");
    for (k = 0; k < browser_count (); k++)
      if (browser_item (k, &it) && it.id == gone)
        break;
    CHECK (k == browser_count (), "and it is the one that went");
    /* Two at once, from the Selection menu, while one is in the viewer. */
    browser_focus_on (0);
    viewer_open (0);
    CHECK (mainwin_mode () == LAYOUT_VIEWER, "a photo in the Viewer");
    mainwin_set_mode (LAYOUT_BROWSER);
    browser_focus_on (0);
    click (16 + 352 + 60, -16 - 60, Button_Adjust);      /* add the second photo (Large thumbnails) */
    click (5, -5, Button_Menu);
    CHECK (strstr (item_text (submenu (menu_open, 1), 14), "Remove from library") != NULL && strstr (item_text (submenu (menu_open, 1), 13), "Print") != NULL
           && strstr (item_text (submenu (menu_open, 1), 12), "Export as library") != NULL, "Selection > Export as library..., Print..., Remove from library");
    CHECK (!strcmp (item_text (submenu (menu_open, 1), 2), "Flag / unflag \x8BP"), "Selection > Flag / unflag, Shift-P shown");
    asked[0] = '\0';
    choose (1, 14, -1);
    CHECK (strstr (asked, "these 2 photos") != NULL, "it asks about the two");
    CHECK (browser_count () == before - 3 && viewer_item () == 0, "both gone, and the Viewer let its photo go");

    /* An album: Menu over it in the Library tab. */
    mainwin_set_tab (TAB_LIBRARY);
    cat_groups (lib_cat (app_library ()), SRC_ALBUM, &g, &ng);
    CHECK (ng >= 1, "albums to work on");
    menus_open_group_menu (500, 500, SRC_ALBUM, g[0].id);
    CHECK (menu_open && strcmp (item_text (menu_open, 0), "Rename") == 0 && strstr (item_text (menu_open, 1), "Delete album") != NULL,
           "an album's menu: Rename, Delete album");
    snprintf ((char *) item (submenu (menu_open, 0), 0)[3], 32, "Lakes trip");
    choose (0, 0, -1);
    {
      char nm[64];
      cat_group_name (lib_cat (app_library ()), SRC_ALBUM, g[0].id, nm, sizeof nm);
      CHECK (strcmp (nm, "Lakes trip") == 0, "renamed");
    }
    menus_open_group_menu (500, 500, SRC_ALBUM, g[0].id);
    answer = 3;
    choose (1, -1, -1);
    CHECK (strstr (asked, "Delete the album \"Lakes trip\"? Its photos stay") != NULL, "deleting asks");
    free (g);
    cat_groups (lib_cat (app_library ()), SRC_ALBUM, &g, &ng);
    for (k = 0; k < ng; k++)
      CHECK (strcmp (g[k].name, "Lakes trip") != 0, "the album is gone");
    free (g);
    /* A project: rename, then remove it all. */
    cat_groups (lib_cat (app_library ()), SRC_PROJECT, &g, &ng);
    CHECK (ng == 1 && strcmp (g[0].name, "Holiday") == 0, "the Holiday project");
    menus_open_group_menu (500, 500, SRC_PROJECT, g[0].id);
    CHECK (strcmp (item_text (menu_open, 1), "Watch for new photos") == 0 && strcmp (item_text (menu_open, 2), "Albums from its folders") == 0
           && strstr (item_text (menu_open, 3), "Export as library") != NULL
           && strstr (item_text (menu_open, 4), "Remove project") != NULL, "a project's menu: Watch, Albums from its folders, Export as library, Remove project");
    CHECK (!(item (menu_open, 1)[0] & 1) && !(item (menu_open, 2)[0] & 1), "neither ticked to start with");
    answer = 2;
    choose (4, -1, -1);
    CHECK (strstr (asked, "\"Holiday\" and its 3 photos") != NULL && browser_count () == 3, "it says how many; Cancel keeps them");
    /* Watch it: a photo put in a new folder inside it comes in; albums from its folders. */
    {
      char d[760], f2[800];
      cat_folder *fa;
      int nfa, w = 0, al = 0;
      snprintf (d, sizeof d, "%s/Holiday/Day2", work);
      mkdir (d, 0777);
      snprintf (f2, sizeof f2, "%s/IMG_0100.jpg", d);
      make_jpeg (f2, 900, 600);
      menus_open_group_menu (500, 500, SRC_PROJECT, g[0].id);
      choose (1, -1, -1);
      nulls_until_idle ();
      cat_project_watching (lib_cat (app_library ()), g[0].id, &w, &al);
      CHECK (w == 1 && al == 0, "Watch for new photos: ticked");
      menus_open_group_menu (500, 500, SRC_PROJECT, g[0].id);
      CHECK ((item (menu_open, 1)[0] & 1) != 0, "the menu shows it ticked");
      choose (2, -1, -1);
      nulls_until_idle ();
      CHECK (cat_folder_albums (lib_cat (app_library ()), g[0].id, &fa, &nfa) == 0 && nfa == 1
             && strcmp (fa[0].name, "Day2") == 0 && fa[0].count == 1, "Albums from its folders: Day2, with the new photo");
      if (nfa == 1)
        {
          menus_open_group_menu (500, 500, SRC_ALBUM, fa[0].id);
          CHECK (strcmp (item_text (menu_open, 0), "Follows its folder") == 0, "a folder album's menu: it follows its folder");
        }
      free (fa);
      /* Off again, and the photo goes, so the counts below stay as they were. */
      menus_open_group_menu (500, 500, SRC_PROJECT, g[0].id);
      choose (1, -1, -1);
      menus_open_group_menu (500, 500, SRC_PROJECT, g[0].id);
      choose (2, -1, -1);
      CHECK (cat_folder_albums (lib_cat (app_library ()), g[0].id, &fa, &nfa) == 0 && nfa == 0, "albums off: gone");
      free (fa);
      {
        cat_source src = { SRC_PROJECT, 0, 0, "" };
        cat_item *it;
        int ni, k2;
        src.id = g[0].id;
        cat_list (lib_cat (app_library ()), &src, &it, &ni);
        for (k2 = 0; k2 < ni; k2++)
          if (!strcmp (it[k2].name, "IMG_0100.jpg"))
            {
              cat_id one = it[k2].id;
              lib_remove (app_library (), &one, 1);
            }
        free (it);
      }
      browser_reload ();
    }
    free (g);
    answer = 2;
    reported[0] = '\0';
  }

  /* Interactive help. */
  {
    int st[9];
    const char *t;
    static const int wins[6] = { -2, MAINW, INSP, BROWSER, VIEWER, PLUGW };
    for (i = 0; i < 6; i++)
      {
        t = help_text (wins[i], 0, 0, 0);
        CHECK (t && t[0] && strlen (t) <= 235, "help: every window has some, short enough for a message");
      }
    for (i = 0; i < 14; i++)
      CHECK (strlen (help_text (EXPORTW, i, 0, 0)) <= 235, "help: the Export box's icons");
    /* Over the toolbar's Export button: the main window's top right is at (st[3], st[4]). */
    st[0] = MAINW;
    memcpy (&st[1], &state_of (MAINW)[1], 6 * sizeof (int));
    { int tr[4]; mainwin_toolbar_button (9, tr); t = help_text (MAINW, -1, st[1] + (tr[0] + tr[2]) / 2, st[4] - 50); }
    CHECK (strstr (t, "Exports the photos") != NULL, "help: the toolbar's Export button");
    t = help_text (MAINW, -1, st[1] + 30, st[4] - 50);
    CHECK (strstr (t, "inspector") != NULL, "help: the toolbar's Inspector button");
    /* The request and reply. */
    memset (b, 0, sizeof b);
    b[0] = 40; b[1] = 0x7777; b[2] = 55; b[4] = Message_HelpRequest;
    b[5] = 100; b[6] = 100; b[8] = BROWSER; b[9] = -1;
    app_event (User_Message_Recorded, b, 0);
    CHECK (sent_block[4] == Message_HelpReply && sent_block[3] == 55 && sent_to == 0x7777
           && strstr ((const char *) &sent_block[5], "double-click") != NULL
           && sent_block[0] == (int) ((20 + strlen ((const char *) &sent_block[5]) + 1 + 3) & ~3u),
           "help: a HelpReply to !Help, your_ref = its my_ref, sized");
    /* A menu item: the Plug-ins submenu's first entry, from the icon bar menu. */
    memset (b, 0, sizeof b);
    b[0] = 1000; b[2] = Button_Menu; b[3] = -2;
    app_event (Mouse_Click, b, 0);
    menu_state[0] = 6; menu_state[1] = 0; menu_state[2] = -1;
    CHECK (strstr (help_text (0x9000, 0, 0, 0), "plug-in is on") != NULL, "help: Plug-ins > LibRaw (on)");
    menu_state[0] = 8; menu_state[1] = -1;
    CHECK (strstr (help_text (0x9000, 3, 0, 0), "Leaves Lumen") != NULL, "help: icon bar menu > Quit");
    menu_state[0] = 7; menu_state[1] = -1;
    CHECK (strstr (help_text (0x9000, 3, 0, 0), "double-click") != NULL, "help: icon bar menu > File types");
    menu_state[0] = -1;
    CHECK (strstr (help_text (0x9000, 3, 0, 0), "") != NULL && help_text (0x9000, 3, 0, 0)[0] == '\0',
           "help: a window that isn't ours: nothing");
  }

  /* One Lumen at a time: a second finds the first (not LumenRaw, not a module). */
  CHECK (app_other_instance () == 0, "single instance: no other Lumen (ourselves, LumenRaw and a module task aren't)");
  other_lumen = 0x5000;
  CHECK (app_other_instance () == 0x5000, "single instance: another Lumen found, over several calls");
  other_lumen = 0;

  /* The icon bar menu: Plug-ins. */
  {
    const int *pm;
    char d[760], f[800];
    FILE *fp;
    fsys_entry e;
    int heif;
    memset (b, 0, sizeof b);
    b[0] = 1000; b[2] = Button_Menu; b[3] = -2;
    app_event (Mouse_Click, b, 0);
    CHECK (menu_open && strcmp (item_text (menu_open, 1), "Help...") == 0 && strcmp (item_text (menu_open, 2), "User guide") == 0
           && strcmp (item_text (menu_open, 4), "Activity...") == 0 && strcmp (item_text (menu_open, 5), "Choices...") == 0
           && strcmp (item_text (menu_open, 6), "Plug-ins") == 0 && strcmp (item_text (menu_open, 7), "File types") == 0
           && strcmp (item_text (menu_open, 8), "Quit") == 0,
           "icon bar menu: Info, Help..., User guide, Open library, Activity..., Choices..., Plug-ins, File types, Quit");
    pm = submenu (menu_open, 6);
    CHECK (strcmp (item_text (pm, 0), "LumenRaw 1.0") == 0 && (item (pm, 0)[0] & 1),
           "Plug-ins: LibRaw, ticked (on)");
    CHECK (strcmp (item_text (pm, 1), "Add plug-in...") == 0 && strcmp (item_text (pm, 2), "Remove") == 0
           && (item (pm, 2)[0] & 0x80), "Plug-ins: Add plug-in..., Remove");
    CHECK (rawjob_available (), "the raw plug-in is at work");
    choose (6, 0, -1);
    CHECK (!rawjob_available (), "switched off: raw files aren't read");
    CHECK (rawjob_switched_off () && strstr (rawjob_missing_message (), "switched off") != NULL,
           "and the messages say it's switched off, not missing");
    app_event (Mouse_Click, b, 0);
    pm = submenu (menu_open, 6);
    CHECK (!(item (pm, 0)[0] & 1), "Plug-ins: LibRaw not ticked");
    choose (6, 0, -1);
    CHECK (rawjob_available (), "switched back on");
    /* Add plug-in...: the window to drop one on. */
    choose (6, 1, -1);
    CHECK (plugw_opens == 1, "Add plug-in... opens its window");
    snprintf (d, sizeof d, "%s/Download/Extra", work);
    fsys_mkdirs (d);
    snprintf (f, sizeof f, "%s/Plugin", d);
    if ((fp = fopen (f, "w")))
      {
        fputs ("Name: Extra\nVersion: 2\nKind: export\nProgram: Run\n", fp);
        fclose (fp);
      }
    snprintf (f, sizeof f, "%s/Run", d);
    if ((fp = fopen (f, "w")))
      fclose (fp);
    memset (b, 0, sizeof b);
    b[0] = 256; b[1] = 0x5678; b[2] = 91; b[4] = 3;
    b[5] = PLUGW; b[6] = -1; b[10] = 0x1000;
    snprintf ((char *) &b[11], 200, "%s", d);
    app_event (User_Message_Recorded, b, 0);
    snprintf (f, sizeof f, "%s/App/Plugins/Extra/Run", work);
    CHECK (fsys_stat (f, &e) == 0, "dropped on Add plug-in: copied into Plugins");
    /* The same dropped on the main window: it is a plug-in, so it asks to replace it. */
    answer = 2;
    b[4] = 3;                          /* the acknowledgement changed the block */
    b[5] = BROWSER;
    app_event (User_Message_Recorded, b, 0);
    CHECK (strstr (asked, "called Extra in Lumen already") != NULL && strcmp (asked_button, "Replace") == 0,
           "dropped again: asks to replace it");
    /* Not a plug-in, on the Add plug-in window. */
    b[4] = 3;
    b[5] = PLUGW;
    snprintf ((char *) &b[11], 200, "%s/Holiday", work);
    app_event (User_Message_Recorded, b, 0);
    CHECK (strstr (reported, "isn't a Lumen plug-in") != NULL, "photos on the Add plug-in window: not a plug-in");
    reported[0] = '\0';
    memset (b, 0, sizeof b);
    b[0] = 1000; b[2] = Button_Menu; b[3] = -2;
    app_event (Mouse_Click, b, 0);
    pm = submenu (menu_open, 6);
    CHECK (strcmp (item_text (pm, 0), "LumenRaw 1.0") == 0 && strcmp (item_text (pm, 1), "Extra 2") == 0
           && strcmp (item_text (pm, 2), "Add plug-in...") == 0, "Plug-ins: both, by name");
    CHECK (strcmp (item_text (submenu (pm, 3), 1), "Extra...") == 0, "Remove > Extra...");
    /* Remove > LibRaw: Cancel keeps it; it mentions PackMan. */
    answer = 2;
    choose (6, 3, 0);
    CHECK (strstr (asked, "LumenRaw) in PackMan") != NULL && rawjob_available (), "Remove LibRaw: mentions PackMan; Cancel keeps it");
    answer = 3;
    choose (6, 3, 1);
    snprintf (f, sizeof f, "%s/App/Plugins/Extra", work);
    CHECK (fsys_stat (f, &e) != 0, "Remove > Extra: deleted");
    answer = 2;
    app_event (Mouse_Click, b, 0);
    pm = submenu (menu_open, 6);
    CHECK (strcmp (item_text (pm, 1), "Add plug-in...") == 0, "Plug-ins: LibRaw alone again");
    /* LumenHEIF beside it: a format plug-in, its Plugin file saying which
       files are its; each switched on and off alone. */
    CHECK (rawjob_kind_for (IMG_UNKNOWN, "x.heic", NULL, 0) < 0, "no plug-in for HEIF yet");
    snprintf (d, sizeof d, "%s/Download/HEIF", work);
    fsys_mkdirs (d);
    snprintf (f, sizeof f, "%s/Plugin", d);
    if ((fp = fopen (f, "w")))
      {
        fputs ("Name: HEIF photos (libheif)\nVersion: 1.0\nKind: format\nAPI: 1\nFormat: HEIF photo\n"
               "Extensions: heic heif hif\nSignature: 4 ftypheic\nProgram: LumenHEIF\nPackage: LumenHEIF\n", fp);
        fclose (fp);
      }
    snprintf (f, sizeof f, "%s/LumenHEIF", d);
    if ((fp = fopen (f, "w")))
      fclose (fp);
    memset (b, 0, sizeof b);
    b[0] = 256; b[1] = 0x5678; b[2] = 92; b[4] = 3;
    b[5] = PLUGW; b[6] = -1; b[10] = 0x1000;
    snprintf ((char *) &b[11], 200, "%s", d);
    app_event (User_Message_Recorded, b, 0);
    heif = rawjob_kind_for (IMG_UNKNOWN, "x.heic", NULL, 0);
    CHECK (heif > 0 && rawjob_available_kind (heif) && rawjob_available (), "LumenHEIF added: HEIF and raw both ready");
    memset (b, 0, sizeof b);
    b[0] = 1000; b[2] = Button_Menu; b[3] = -2;
    app_event (Mouse_Click, b, 0);
    pm = submenu (menu_open, 6);
    CHECK (strcmp (item_text (pm, 0), "LumenRaw 1.0") == 0
           && strcmp (item_text (pm, 1), "LumenHEIF 1.0") == 0
           && strcmp (item_text (pm, 2), "Add plug-in...") == 0, "Plug-ins: LibRaw and HEIF");
    choose (6, 1, -1);
    CHECK (!rawjob_available_kind (heif) && rawjob_switched_off_kind (heif) && rawjob_available ()
           && strstr (rawjob_missing_message_kind (heif), "switched off") != NULL,
           "HEIF switched off: raws still developed");
    choose (6, 1, -1);
    CHECK (rawjob_available_kind (heif) && !rawjob_switched_off_kind (heif), "HEIF switched back on");
    answer = 3;
    choose (6, 3, 1);
    CHECK (!rawjob_available_kind (heif) && rawjob_kind_for (IMG_UNKNOWN, "x.heic", NULL, 0) < 0 && rawjob_available (),
           "Remove > HEIF: gone (no plug-in claims .heic now), LibRaw kept");
    /* One for a newer API than this Lumen's: listed, but not run. */
    snprintf (f, sizeof f, "%s/Plugin", d);
    if ((fp = fopen (f, "w")))
      {
        fputs ("Name: HEIF photos (libheif)\nVersion: 9\nKind: format\nAPI: 99\nFormat: HEIF photo\n"
               "Extensions: heic\nProgram: LumenHEIF\n", fp);
        fclose (fp);
      }
    memset (b, 0, sizeof b);
    b[0] = 256; b[1] = 0x5678; b[2] = 93; b[4] = 3;
    b[5] = PLUGW; b[6] = -1; b[10] = 0x1000;
    snprintf ((char *) &b[11], 200, "%s", d);
    app_event (User_Message_Recorded, b, 0);
    heif = rawjob_kind_for (IMG_UNKNOWN, "x.heic", NULL, 0);
    CHECK (heif > 0 && !rawjob_available_kind (heif) && strstr (rawjob_missing_message_kind (heif), "newer Lumen"),
           "a plug-in for a newer API: not run, and says it needs a newer Lumen");
    answer = 3;
    choose (6, 3, 1);
    answer = 2;
  }
  memset (b, 0, sizeof b);
  b[0] = 1000; b[2] = Button_Menu; b[3] = -2;
  app_event (Mouse_Click, b, 0);
  CHECK (submenu (menu_open, 0) == (const int *) INFO, "Info opens the Info box");
  /* Choices... */
  {
    char cf[760];
    prefs p;
    choose (5, -1, -1);
    CHECK (!(choice_icon_flags[4] & (1u << 21)) && (choice_icon_flags[5] & (1u << 21))
           && (choice_icon_flags[15] & (1u << 21)) && (choice_icon_flags[17] & (1u << 21)),
           "Choices: PNG (what the last export used), Medium thumbnails, checking files");
    CHECK (choicewin_library_text ()[0] == '\0', "Choices: library empty (the usual place)");
    CHECK ((choice_icon_flags[27] & (1u << 21)) && !(choice_icon_flags[28] & (1u << 21))
           && !(choice_icon_flags[29] & (1u << 21)) && lib_previews (app_library ()) == PREVIEWS_RAW,
           "Choices: previews of raw files (the usual)");
    /* PNG, Large, no checking, previews of all photos, a library elsewhere; then Save. */
    choice_icon_flags[27] = 0; choice_icon_flags[28] = 1u << 21;
    choice_icon_flags[4] = 0; choice_icon_flags[5] = 1u << 21;
    choice_icon_flags[15] = 0; choice_icon_flags[16] = 1u << 21;
    choice_icon_flags[17] = 0;
    choicewin_set_library_text ("  SDFS::Pi.$.Photos.Library  ");
    memset (b, 0, sizeof b);
    b[2] = Button_Select; b[3] = CHOICEW; b[4] = 19;
    app_event (Mouse_Click, b, 0);
    CHECK (app_prefs ()->export_format == 1 && app_prefs ()->thumb_size == 2 && !app_prefs ()->verify
           && strcmp (app_prefs ()->library, "SDFS::Pi.$.Photos.Library") == 0, "Choices: saved (the path trimmed)");
    CHECK (browser_size () == 2, "Choices: Large thumbnails at once");
    CHECK (app_prefs ()->previews == 2 && lib_previews (app_library ()) == PREVIEWS_ALL, "Choices: previews of all photos at once");
    {
      /* Made in idle time once there is nothing else to do. */
      cat_item it;
      cat_details dd;
      char pv[1300];
      nulls_until_idle ();
      CHECK (browser_item (0, &it) && cat_get_details (lib_cat (app_library ()), it.id, &dd) == 0
             && lib_preview_find (app_library (), dd.original, pv, sizeof pv), "Choices: the previews made in idle time");
    }
    snprintf (cf, sizeof cf, "%s/Choices", work);
    CHECK (prefs_load (cf, &p) == 0 && p.export_format == 1 && p.thumb_size == 2 && !p.verify && p.previews == 2
           && strcmp (p.library, "SDFS::Pi.$.Photos.Library") == 0, "Choices: in the file");
    /* A directory dropped on it: the library field. */
    memset (b, 0, sizeof b);
    b[0] = 256; b[1] = 0x5678; b[2] = 92; b[4] = 3; b[5] = CHOICEW; b[10] = 0x1000;
    snprintf ((char *) &b[11], 200, "%s/Holiday", work);
    app_event (User_Message_Recorded, b, 0);
    CHECK (strstr (choicewin_library_text (), "/Holiday") != NULL, "Choices: a directory dropped sets the library");
  }
  /* Libraries: export the whole library, a project; import; check. */
  {
    char dst[800], line[200];
    cat_group *g;
    fsys_entry e;
    int ng, before = browser_count ();
    memset (b, 0, sizeof b);
    b[0] = 1000; b[2] = Button_Menu; b[3] = -2;
    app_event (Mouse_Click, b, 0);
    CHECK (strcmp (item_text (menu_open, 3), "Open library") == 0
           && strcmp (item_text (submenu (menu_open, 3), 0), "Export library...") == 0
           && strcmp (item_text (submenu (menu_open, 3), 1), "Import library...") == 0
           && strcmp (item_text (submenu (menu_open, 3), 2), "Check library...") == 0,
           "icon bar: Open library > Export, Import, Check library");
    choose (3, 0, -1);
    CHECK (menu_open == (const int *) LIBEW && strstr (libbox_line (LIBEW, 0), "The whole library: ")
           && strstr (libbox_line (LIBEW, 1), "Include the original photos (") && !strcmp (libbox_line (LIBEW, 2), "Library"),
           "Export library...: the box, for the whole library, offering the originals");
    CHECK ((lib_icon_flags[0][3] & (1u << 21)) != 0, "originals ticked to start with");
    snprintf (dst, sizeof dst, "%s/UiExport", work);
    libbox_set_name (dst);
    key_in (LIBEW, KEY_RETURN);
    CHECK (libbox_busy (), "Save: the export runs in the background");
    {
      char st[200];
      libbox_status (st, sizeof st);
      CHECK (strstr (st, "Exporting ") && strstr (st, "UiExport"), "its progress in the status area and Activity");
    }
    nulls_until_idle ();
    snprintf (line, sizeof line, "%s/UiExport/Catalogue", work);
    CHECK (fsys_stat (line, &e) == 0 && history_has ("to UiExport, with their originals"), "exported, in the history");
    snprintf (line, sizeof line, "%s/UiExport/Originals", work);
    CHECK (fsys_stat (line, &e) == 0 && e.is_dir, "the originals inside it");
    libbox_set_name (dst);
    key_in (LIBEW, KEY_RETURN);
    CHECK (strstr (reported, "already something called UiExport"), "not over a library already there");
    reported[0] = '\0';
    key_in (LIBEW, KEY_ESCAPE);
    /* A project's menu: Export as library... */
    cat_groups (lib_cat (app_library ()), SRC_PROJECT, &g, &ng);
    if (ng > 0)
      {
        menus_open_group_menu (500, 500, SRC_PROJECT, g[0].id);
        choose (3, -1, -1);
        snprintf (line, sizeof line, "The project %s: %d photo", g[0].name, g[0].count);
        CHECK (menu_open == (const int *) LIBEW && strstr (libbox_line (LIBEW, 0), line) != NULL, "a project's Export as library...");
        key_in (LIBEW, KEY_ESCAPE);
      }
    free (g);
    /* The library dropped on Lumen's window: the Import library window,
       with what would come (all of it here already). */
    memset (b, 0, sizeof b);
    b[0] = 256; b[1] = 0x5678; b[2] = 93; b[4] = 3; b[5] = MAINW; b[10] = 0x1000;
    snprintf ((char *) &b[11], 200, "%s", dst);
    app_event (User_Message_Recorded, b, 0);
    CHECK (strstr (libbox_line (LIBIW, 0), "From UiExport: ") && strstr (libbox_line (LIBIW, 0), "already here"),
           "a library dropped on the window: what it holds, not imported as photos");
    CHECK ((lib_icon_flags[1][6] & (1u << 22)) != 0 && browser_count () == before, "nothing new: Import shaded, nothing added");
    memset (b, 0, sizeof b);
    b[2] = Button_Select; b[3] = LIBIW; b[4] = 5;
    app_event (Mouse_Click, b, 0);
    /* A folder of photos dropped on the Import library window isn't one. */
    memset (b, 0, sizeof b);
    b[0] = 256; b[1] = 0x5678; b[2] = 94; b[4] = 3; b[5] = LIBIW; b[10] = 0x1000;
    snprintf ((char *) &b[11], 200, "%s/Holiday", work);
    app_event (User_Message_Recorded, b, 0);
    CHECK (strstr (reported, "isn't a Lumen library") != NULL, "a folder of photos isn't a library");
    reported[0] = '\0';
    /* Check library... */
    memset (b, 0, sizeof b);
    b[0] = 1000; b[2] = Button_Menu; b[3] = -2;
    app_event (Mouse_Click, b, 0);
    choose (3, 2, -1);
    CHECK (libbox_busy () && strstr (libbox_line (LIBCW, 0), "Checking"), "Check library...: the check runs");
    nulls_until_idle ();
    CHECK (strstr (libbox_line (LIBCW, 0), "All is well") != NULL && (lib_icon_flags[2][7] & (1u << 22)),
           "all is well; nothing to repair");
    CHECK (history_has ("Checked the library: all is well"), "the check in the history");
    memset (b, 0, sizeof b);
    b[2] = Button_Select; b[3] = LIBCW; b[4] = 6;
    app_event (Mouse_Click, b, 0);
    memset (b, 0, sizeof b);
    b[0] = 1000; b[2] = Button_Menu; b[3] = -2;
    app_event (Mouse_Click, b, 0);
  }
  /* User guide: no web browser, so it says so; with one, Filer_Run. */
  unsetenv ("Alias$@RunType_FAF");
  choose (2, -1, -1);
  CHECK (strstr (reported, "no web browser") != NULL, "User guide: no browser: says so");
  reported[0] = '\0';
  setenv ("Alias$@RunType_FAF", "/NetSurf:!Run %*0", 1);
  choose (2, -1, -1);
  CHECK (strstr (started, ".Guide.index") != NULL && !strncmp (started, "Filer_Run ", 10), "User guide: Filer_Run the guide");
  choose (1, -1, -1);
  CHECK (strstr (started, ".!Help") != NULL, "Help...: Filer_Run !Help");
  choose (4, -1, -1);
  CHECK (activitywin_is_open (), "Activity...: the Activity window opens");
  {
    int n = activity_history_count ();
    long when;
    int kind;
    const char *t = activity_history (0, &when, &kind);
    char newest[200];
    snprintf (newest, sizeof newest, "%s", t ? t : "");
    CHECK (n > 0 && history_has ("Imported Holiday: 6 new photos") && history_has ("Made "), "history: the import and thumbnails");
    activity_init ();                  /* as at the next start: read back from the file */
    CHECK (activity_history_count () == n && !strcmp (activity_history (0, NULL, NULL), newest),
           "history kept between sessions");
    activity_clear ();
    activity_init ();
    CHECK (activity_history_count () == 0, "Clear history: the file too");
  }
  /* File types: which double-clicks open in Lumen (Message_DataOpen). */
  {
    char f[800], val[2000], d[760];
    int n, k, jpeg = -1, other = -1;
    FILE *fp;
    /* A format plug-in with a MIME line MimeMap gives a filetype. */
    fsys_mimemap_add ("image/x-testphoto", 0xA3A, "TestPhoto", "tph");
    snprintf (d, sizeof d, "%s/App/Plugins/TestPhoto", work);
    fsys_mkdirs (d);
    snprintf (f, sizeof f, "%s/Plugin,fff", d);
    if ((fp = fopen (f, "w")))
      {
        fputs ("Name: Test photos\nVersion: 1\nKind: format\nAPI: 1\nFormat: Test photo\nExtensions: tph\n"
               "MIME: image/x-testphoto\nProgram: Run\nPackage: TestPhoto\n", fp);
        fclose (fp);
      }
    snprintf (f, sizeof f, "%s/Run,ff8", d);
    if ((fp = fopen (f, "w")))
      fclose (fp);
    app_plugins_rescan ();
    n = filetypes_count ();
    for (k = 0; k < n; k++)
      {
        if (filetypes_type (k) == 0xC85)
          jpeg = k;
        if (filetypes_type (k) == 0xA3A)
          other = k;
      }
    CHECK (jpeg == 0 && filetypes_type (1) == 0xB60 && filetypes_type (2) == 0xFF0,
           "File types: JPEG, PNG and TIFF first");
    CHECK (other > 2 && !strcmp (filetypes_label (other), "Test photo (TestPhoto)") && filetypes_ticked (other)
           && !filetypes_ticked (jpeg) && filetypes_mode () == OPEN_RUNNING,
           "and the plug-in's type (from its MIME line, by MimeMap), ticked; JPEG not; While running");
    CHECK (getvar ("File$Type_A3A") && !strcmp (getvar ("File$Type_A3A"), "TestPhoto"),
           "a type with no name gets MimeMap's");
    app_event (Mouse_Click, (memset (b, 0, sizeof b), b[0] = 1000, b[2] = Button_Menu, b[3] = -2, b), 0);
    {
      const int *fm = submenu (menu_open, 7);
      CHECK (fm && !strcmp (item_text (fm, 0), "Never") && !strcmp (item_text (fm, 1), "While Lumen is running")
             && (item (fm, 1)[0] & 1) && !strcmp (item_text (fm, 3), "JPEG") && !(item (fm, 3)[0] & 1)
             && !strcmp (item_text (fm, 3 + n), "Data, by name"), "File types menu: the choices, the types, Data");
    }
    /* A JPEG double-clicked: not claimed until it's ticked. */
    snprintf (f, sizeof f, "%s/Opened", work);
    fsys_mkdirs (f);
    snprintf (f, sizeof f, "%s/Opened/Lake.jpg", work);
    make_jpeg (f, 64, 48);
#define DATAOPEN(type, path) (memset (b, 0, sizeof b), b[0] = 256, b[1] = 0x6789, b[2] = 77, b[4] = 5, \
                              b[10] = (type), snprintf ((char *) &b[11], 200, "%s", (path)), sent_block[4] = 0, \
                              app_event (User_Message_Recorded, b, 0), sent_block[4] == 4 && sent_to == 0x6789)
    CHECK (!DATAOPEN (0xC85, f), "a JPEG double-clicked: not Lumen's (not ticked)");
    choose (7, 3 + jpeg, -1);
    CHECK (filetypes_ticked (jpeg), "File types > JPEG: ticked");
    CHECK (DATAOPEN (0xC85, f), "now a JPEG double-clicked is claimed (DataLoadAck)");
    nulls_until_idle ();
    CHECK (viewer_item () != 0 && mainwin_mode () != LAYOUT_BROWSER, "imported and opened in the Viewer");
    {
      cat_details d;
      CHECK (cat_get_details (lib_cat (app_library ()), viewer_item (), &d) == 0 && strstr (d.path, "Lake.jpg"),
             "the photo double-clicked");
    }
    CHECK (DATAOPEN (0xC85, f), "again: claimed, and opened (it's in the library)");
    /* Always: a Run alias for it, written for !Boot too. */
    choose (7, 2, -1);
    CHECK (getvar ("Alias$@RunType_C85") && strstr (getvar ("Alias$@RunType_C85"), "%*0")
           && !getvar ("Alias$@RunType_B60"), "Always: JPEG's Run alias starts Lumen; PNG's (not ticked) untouched");
    app_choices_file (f, sizeof f, "Types");
    val[0] = '\0';
    if ((fp = fopen (f, "r")))
      {
        size_t got = fread (val, 1, sizeof val - 1, fp);
        val[got] = '\0';
        fclose (fp);
      }
    CHECK (strstr (val, "Set Alias$@RunType_C85 /<Lumen$Dir> %%*0") && strstr (val, "File$Type_A3A TestPhoto"),
           "the Types file for !Boot: the alias, and the name");
    fake_setvar ("Alias$@RunType_B60", "/Paint %*0");
    choose (7, 1, -1);
    CHECK (!getvar ("Alias$@RunType_C85") && getvar ("Alias$@RunType_B60"),
           "While running: Lumen's alias goes, another program's stays");
    /* Data files named like a photo, while running. */
    snprintf (val, sizeof val, "%s/Opened/scan/tif", work);
    CHECK (DATAOPEN (0xFFD, val) && !DATAOPEN (0xFFD, "Notes/txt"), "Data, by name: /tif claimed, /txt not");
    choose (7, 3 + n, -1);
    CHECK (!DATAOPEN (0xFFD, val), "Data unticked: not claimed");
    choose (7, 0, -1);
    CHECK (!DATAOPEN (0xC85, f) && filetypes_mode () == OPEN_NEVER, "Never: nothing claimed");
    choose (7, 1, -1);
#undef DATAOPEN
  }
  CHECK (choose (8, -1, -1) == 1, "Quit from the icon bar menu quits");
  memset (b, 0, sizeof b);
  b[4] = 0;
  CHECK (app_event (User_Message, b, 0) == 1, "Message_Quit quits");
  app_quit ();
  CHECK (closedown == 1, "Wimp_CloseDown on the way out");
  CHECK (reported[0] == '\0', "no errors reported");
  if (reported[0])
    fprintf (stderr, "reported: %s\n", reported);
  return fake_done ("test_ui");
}
