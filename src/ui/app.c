/* app.c: the Wimp task. Starts up, puts an icon on the icon bar, opens the
   library, and dispatches Wimp_Poll events. Background work (imports,
   thumbnails, loading thumbnails for display) runs on null events in
   slices of one or two centiseconds, so the desktop never waits for it.

   Message_DataLoad (PRM p1005): +20 window, +24 icon, +28/+32 position,
   +36 size, +40 filetype, +44 pathname. Acknowledged with
   Message_DataLoadAck: the same block, action 4, your_ref = my_ref. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <kernel.h>
#include <swis.h>
#include "ui.h"

static int task;
static library *lib;
static unsigned last_reload, last_title;
static int list_dirty;

library *app_library (void) { return lib; }
int app_task (void) { return task; }

unsigned app_now (void)
{
  int t = 0;
  _swix (OS_ReadMonotonicTime, _OUT (0), &t);
  return (unsigned) t;
}

void app_report (const char *msg)
{
  _kernel_oserror e;
  e.errnum = 0;
  snprintf (e.errmess, sizeof e.errmess, "%s", msg);
  _swix (Wimp_ReportError, _INR (0, 2), &e, 1, APP_NAME);
}

static int create_iconbar_icon (void)
{
  int b[9];
  memset (b, 0, sizeof b);
  b[0] = -1;                           /* the right of the icon bar */
  b[1] = 0; b[2] = 0; b[3] = 68; b[4] = 68;
  b[5] = 0x301A;                       /* sprite, centred, click */
  memcpy (&b[6], "!lumen", 7);
  return _swix (Wimp_CreateIcon, _INR (0, 1), 0, b) ? -1 : 0;
}

int app_init (const char *library_path)
{
  static const int messages[] = { Message_DataLoad, 2 /* DataSaveAck */, Message_ModeChange, 0 };
  char err[300];
  if (_swix (Wimp_Initialise, _INR (0, 3) | _OUT (1), 380, TASK_WORD, APP_NAME, messages, &task))
    return 1;
  draw_init_mode ();
  thumbcache_init (64u << 20);
  if (lib_open (&lib, library_path, err, sizeof err))
    {
      app_report (err);
      return 1;
    }
  if (create_iconbar_icon () || browser_create () || viewer_create () || exportbox_create ())
    {
      app_report ("Lumen couldn't create its windows.");
      return 1;
    }
  menus_init ();
  browser_open (NULL);
  return 0;
}

void app_quit (void)
{
  lib_close (lib);
  lib = NULL;
  _swix (Wimp_CloseDown, _INR (0, 1), task, TASK_WORD);
}

int app_busy (void)
{
  return viewer_busy () || lib_busy (lib) || thumbcache_pending () || exports_busy ();
}

static void thumb_made (void *ctx, cat_id v)
{
  (void) ctx;
  browser_thumb_ready (v);
}

static void idle (void)
{
  unsigned start, deadline;
  int changed;
  /* The photo being edited comes first. */
  if (viewer_busy ())
    viewer_idle ();
  start = app_now ();
  deadline = start + 2;
  /* What is on screen comes first. */
  if (thumbcache_pending ())
    thumbcache_work (app_now, start + 1, browser_thumb_loaded);
  if (exports_busy ())
    {
      exports_idle (deadline);
      if ((int) (app_now () - last_title) > 25 || !exports_busy ())
        {
          last_title = app_now ();
          browser_update_title ();
        }
    }
  if (!lib_busy (lib))
    {
      browser_update_title ();
      return;
    }
  changed = lib_work (lib, app_now, deadline, thumb_made, NULL);
  if (changed & LIB_LIST_CHANGED)
    list_dirty = 1;
  /* Reload the list at most twice a second while importing: reading it
     back is the slow part on a large library. */
  if (list_dirty && ((int) (app_now () - last_reload) > 50 || !lib_busy (lib)))
    {
      list_dirty = 0;
      last_reload = app_now ();
      browser_reload ();
    }
  else if ((int) (app_now () - last_title) > 25)
    {
      last_title = app_now ();
      browser_update_title ();
    }
}

static void data_load (int *b)
{
  const char *path = (const char *) &b[11];
  int type = b[10];
  if (b[5] != browser_window () && b[5] != -2)
    return;
  if (lib_import (lib, path))
    {
      app_report ("Not enough memory to start the import.");
      return;
    }
  /* Acknowledge: same block, action DataLoadAck, your_ref = my_ref. */
  b[3] = b[2];
  b[4] = Message_DataLoadAck;
  _swix (Wimp_SendMessage, _INR (0, 2), User_Message, b, b[1]);
  (void) type;
  /* Show what arrives. */
  {
    cat_source s = { SRC_LAST_IMPORT, 0, 0 };
    if (browser_source ()->kind != SRC_LAST_IMPORT)
      browser_set_source (&s);
    browser_open (NULL);
  }
}

int app_event (int reason, int *b, int sender)
{
  (void) sender;
  switch (reason)
    {
    case Null_Reason:
      idle ();
      break;
    case Redraw_Window_Request:
      if (b[0] == browser_window ())
        browser_redraw (b);
      else if (b[0] == viewer_window ())
        viewer_redraw (b);
      else
        {
          int more = 0;
          _swix (Wimp_RedrawWindow, _IN (1) | _OUT (0), b, &more);
          while (more)
            _swix (Wimp_GetRectangle, _IN (1) | _OUT (0), b, &more);
        }
      break;
    case Open_Window_Request:
      if (b[0] == browser_window ())
        browser_open (b);
      else if (b[0] == viewer_window ())
        viewer_open_window (b);
      else
        _swix (Wimp_OpenWindow, _IN (1), b);
      break;
    case Close_Window_Request:
      if (b[0] == browser_window ())
        browser_close ();
      else if (b[0] == viewer_window ())
        viewer_close ();
      else
        _swix (Wimp_CloseWindow, _IN (1), b);
      break;
    case Mouse_Click:
      if (b[3] == -2)                  /* the icon bar */
        {
          if (b[2] == Button_Menu)
            menus_open_iconbar_menu (b[0]);
          else
            browser_open (NULL);
        }
      else if (b[3] == browser_window ())
        browser_click (b);
      else if (b[3] == viewer_window ())
        viewer_click (b);
      else if (b[3] == exportbox_window ())
        exportbox_click (b);
      break;
    case User_Drag_Box:
      if (exportbox_dragging ())
        exportbox_drag_end ();
      else
        viewer_drag_end ();
      break;
    case Key_Pressed:
      if (b[0] == exportbox_window () ? !exportbox_key (b[6])
          : b[0] == viewer_window () ? !viewer_key (b[6])
          : (b[0] != browser_window () || !browser_key (b[6])))
        _swix (Wimp_ProcessKey, _IN (0), b[6]);
      break;
    case Menu_Selection:
      return menus_selection (b);
    case User_Message:
    case User_Message_Recorded:
      switch (b[4])
        {
        case Message_Quit:
          return 1;
        case Message_DataLoad:
          data_load (b);
          break;
        case 2:                        /* Message_DataSaveAck */
          exportbox_save_ack (b);
          break;
        case Message_ModeChange:
          draw_init_mode ();
          browser_mode_changed ();
          viewer_mode_changed ();
          break;
        }
      break;
    }
  return 0;
}
