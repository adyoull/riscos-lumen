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
#include "rawjob.h"
#include "fsys.h"
#include "plugins.h"

static int task;
static library *lib;
static unsigned last_reload, last_title;
static unsigned last_watch;            /* when the watched folders were last looked through */
static int nwatched = -1;              /* how many there are (-1: not counted yet) */
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
  activity_log (ACT_PROBLEM, "%s", msg);
  _swix (Wimp_ReportError, _INR (0, 2), &e, 1, APP_NAME);
}

/* A question with a button for yes (Wimp_ReportError, new style: the
   question category, Cancel and the given button). 1 if it was chosen. */
int app_confirm (const char *msg, const char *button)
{
  _kernel_oserror e;
  int chosen = 0;
  e.errnum = 0;
  snprintf (e.errmess, sizeof e.errmess, "%s", msg);
  /* Flags: Cancel (bit 1), title given (4), new style (8), category 4: question (bits 9-11). */
  if (_swix (Wimp_ReportError, _INR (0, 5) | _OUT (1), &e, 2 | 0x10 | 0x100 | (4 << 9), APP_NAME,
             "!lumen", 1, button, &chosen))
    return 0;
  return chosen == 3;
}

static int have_sprite (const char *name)
{
  /* Wimp_SpriteOp 40 (read sprite information): an error if it isn't there. */
  int w, h;
  return _swix (Wimp_SpriteOp, _INR (0, 2) | _OUT (3) | _OUT (4), 40, 0, name, &w, &h) == NULL;
}

/* The !Sprites are normally loaded by !Boot (when the Filer first shows
   the application) and by !Run. If neither did - !RunImage started some
   other way, or the files lost their types on the way here - load them
   now, so the icon bar icon has its picture. */
static void ensure_sprites (void)
{
  char cmd[300];
  const char *v;
  if (have_sprite ("!lumen"))
    return;
  v = getenv ("Lumen$Dir");
  if (!v || !*v)
    return;
  snprintf (cmd, sizeof cmd, "IconSprites %s.!Sprites", v);
  _swix (OS_CLI, _IN (0), cmd);
}

static int create_iconbar_icon (void)
{
  int b[9];
  memset (b, 0, sizeof b);
  b[0] = -1;                           /* the right of the icon bar */
  if (have_sprite ("!lumen"))
    {
      b[1] = 0; b[2] = 0; b[3] = 68; b[4] = 68;
      b[5] = 0x301A;                   /* sprite, centred, click */
      memcpy (&b[6], "!lumen", 7);
    }
  else
    {
      /* No picture to be had: the name, so there is always something to click. */
      b[1] = 0; b[2] = 0; b[3] = 120; b[4] = 68;
      b[5] = (int) 0x17003019;         /* text, centred, click; black on grey */
      memcpy (&b[6], "Lumen", 6);
    }
  return _swix (Wimp_CreateIcon, _INR (0, 1), 0, b) ? -1 : 0;
}

static int closed_down;

/* However Lumen ends - Quit, an error, an exit from deep inside a
   library - it leaves the Wimp properly, so no dead task is left behind
   in the Task Manager. */
static void close_down (void)
{
  if (task && !closed_down)
    {
      closed_down = 1;
      _swix (Wimp_CloseDown, _INR (0, 1), task, TASK_WORD);
    }
}

/* --- the LibRaw plug-in: a program run in a task window (rawjob.h) ---

   Lumen is the task window's parent (*TaskWindow ... -task &<Lumen>
   -txt &<tag>), so TaskWindow tells Lumen, not a task window server, what
   happens: TaskWindow_Ego gives the child's task handle for our tag,
   TaskWindow_Morio says it has ended, TaskWindow_Output carries anything
   it prints (it prints nothing; not acknowledged, see the PRM's erratum).
   PRM: *TaskWindow p986, the messages p1359-1361. */
#define Message_TaskWindow_Output 0x808C1
#define Message_TaskWindow_Ego 0x808C2
#define Message_TaskWindow_Morio 0x808C3
#define RAW_CHILDREN 8
static struct { int tag, child; } children[RAW_CHILDREN];

static int run_in_task_window (const char *cmdline, int tag)
{
  char cmd[2600];
  int k;
  /* Starts small; UnixLib grows the slot as LibRaw needs (a big raw file
     can take a few hundred megabytes while it is developed). */
  snprintf (cmd, sizeof cmd, "TaskWindow \"%s\" -wimpslot 8000K -name LumenRaw -quit -task &%08X -txt &%08X",
            cmdline, (unsigned) task, (unsigned) tag);
  for (k = 0; k < RAW_CHILDREN; k++)
    if (!children[k].tag)
      {
        children[k].tag = tag;
        children[k].child = 0;
        break;
      }
  return _swix (Wimp_StartTask, _IN (0), cmd) ? -1 : 0;
}

static void raw_message (int *b)
{
  int k;
  if (b[4] == Message_TaskWindow_Ego)
    {
      for (k = 0; k < RAW_CHILDREN; k++)
        if (children[k].tag && children[k].tag == b[5])
          children[k].child = b[1];
    }
  else if (b[4] == Message_TaskWindow_Morio)
    for (k = 0; k < RAW_CHILDREN; k++)
      if (children[k].tag && children[k].child == b[1])
        {
          rawjob_ended (children[k].tag);
          children[k].tag = 0;
        }
}

/* --- plug-ins (plugins.h)

   They live in <Lumen$Dir>.Plugins, one directory each (PackMan installs
   them there: LumenRaw, for instance, into Apps.Graphics.!Lumen.Plugins.
   LibRaw). Which are switched off is kept in <Choices$Write>.Lumen.
   Plugins. A raw plug-in that is on is the one rawjob runs. */
/* --- !Help and the User Guide */
void app_open_doc (const char *leaf)
{
  const char *v = getenv ("Lumen$Dir");
  char dir[600], cmd[700];
  snprintf (dir, sizeof dir, "%s", v ? v : "");
  if (!dir[0])
    return;
  if (!strcmp (leaf, "Guide.index/html"))
    {
      /* An HTML file runs only if something says how: a web browser
         sets Alias$@RunType_FAF when the Filer sees it. */
      const char *run = getenv ("Alias$@RunType_FAF");
      if (!run || !*run)
        {
          app_report ("There's no web browser to show the User Guide in. Open the directory a browser such as "
                      "NetSurf is in (so the Filer sees it), then try again. The guide is !Lumen.Guide.index/html.");
          return;
        }
    }
  snprintf (cmd, sizeof cmd, "Filer_Run %s.%s", dir, leaf);
  if (_swix (Wimp_StartTask, _IN (0), cmd))
    app_report ("Lumen couldn't open that file.");
}

/* --- Choices (prefs.h) */
static prefs the_prefs;
static char prefs_path[700];

const prefs *app_prefs (void) { return &the_prefs; }

void app_load_prefs (void)
{
  const char *v = getenv ("Lumen$ChoicesFile");   /* the tests' own */
  char dir[600];
  if (v && *v)
    snprintf (prefs_path, sizeof prefs_path, "%s", v);
  else if (fsys_canonical ("<Choices$Write>.Lumen", dir, sizeof dir) == 0)
    fsys_join (prefs_path, sizeof prefs_path, dir, "Choices");
  else
    snprintf (prefs_path, sizeof prefs_path, "<Choices$Write>.Lumen.Choices");
  prefs_load (prefs_path, &the_prefs);
}

int app_set_prefs (const prefs *p)
{
  if (prefs_save (prefs_path, p))
    {
      app_report ("Lumen couldn't save its Choices.");
      return -1;
    }
  the_prefs = *p;
  lib_set_new_folders (lib, p->watch_new, p->albums_new);
  browser_set_size (p->thumb_size);
  mainwin_status_changed ();
  exportbox_use_prefs (p);
  return 0;
}

static plugin_info plugin_list[PLUGINS_MAX];
static int nplugins;
static char plugins_dir[700], plugins_state[700], raw_helper[1024];

int app_plugins (const plugin_info **list)
{
  *list = plugin_list;
  return nplugins;
}

static void plugin_paths (void)
{
  const char *v = getenv ("Lumen$Dir");
  char dir[600];
  snprintf (dir, sizeof dir, "%s", v ? v : "");   /* copied at once (getenv's buffer is reused) */
  if (dir[0])
    fsys_join (plugins_dir, sizeof plugins_dir, dir, "Plugins");
  v = getenv ("Lumen$PluginChoices");   /* the tests' own */
  if (v && *v)
    snprintf (plugins_state, sizeof plugins_state, "%s", v);
  else if (fsys_canonical ("<Choices$Write>.Lumen", dir, sizeof dir) == 0)
    fsys_join (plugins_state, sizeof plugins_state, dir, "Plugins");
  else
    snprintf (plugins_state, sizeof plugins_state, "<Choices$Write>.Lumen.Plugins");
}

void app_plugins_rescan (void)
{
  const plugin_info *raw;
  char scratch[700];
  if (!plugins_dir[0])
    plugin_paths ();
  nplugins = plugins_dir[0] ? plugins_scan (plugins_dir, plugins_state, plugin_list, PLUGINS_MAX) : 0;
  raw = plugins_find (plugin_list, nplugins, "raw");
  {
    int k, off = 0;
    for (k = 0; k < nplugins; k++)
      if (!strcmp (plugin_list[k].kind, "raw") && !plugin_list[k].enabled)
        off = 1;
    rawjob_set_switched_off (!raw && off);
  }
  if (!strcmp (raw ? raw->program : "", raw_helper))
    return;                            /* the same as before: jobs carry on */
  snprintf (raw_helper, sizeof raw_helper, "%s", raw ? raw->program : "");
  if (!raw)
    {
      rawjob_setup (NULL, NULL, NULL);
      return;
    }
  {
    const char *v = getenv ("Lumen$Scratch");   /* the tests' own */
    if (v && *v)
      snprintf (scratch, sizeof scratch, "%s", v);
    else if (fsys_canonical ("<Wimp$ScrapDir>.LumenRaw", scratch, sizeof scratch))
      snprintf (scratch, sizeof scratch, "<Wimp$ScrapDir>.LumenRaw");
  }
  rawjob_setup (raw_helper, scratch, run_in_task_window);
  if (lib)
    lib_retry_failed_thumbs (lib);     /* raw files imported while there was no plug-in */
}

/* A plug-in can't change under a job that is running it. */
static int plugins_in_use (void)
{
  if (!rawjob_busy ())
    return 0;
  app_report ("A plug-in is at work just now. Try again when Lumen has finished making thumbnails.");
  return 1;
}

static void plugins_changed (void)
{
  app_plugins_rescan ();
  browser_reload ();
  mainwin_info_changed ();
}

void app_plugin_toggle (int k)
{
  if (k < 0 || k >= nplugins || plugins_in_use ())
    return;
  if (plugins_set_enabled (plugins_state, plugin_list[k].dir, !plugin_list[k].enabled))
    {
      app_report ("Lumen couldn't save the plug-in choices.");
      return;
    }
  plugins_changed ();
}

void app_plugin_remove (int k)
{
  char msg[600], err[300];
  plugin_info p;
  if (k < 0 || k >= nplugins || plugins_in_use ())
    return;
  p = plugin_list[k];
  if (p.package[0])
    snprintf (msg, sizeof msg,
              "Remove the plug-in %s? Its directory is deleted from Lumen's Plugins. "
              "If it came from PackMan, remove its package (%s) in PackMan instead, so PackMan knows it has gone.",
              p.name, p.package);
  else
    snprintf (msg, sizeof msg, "Remove the plug-in %s? Its directory is deleted from Lumen's Plugins.", p.name);
  if (!app_confirm (msg, "Remove"))
    return;
  if (plugins_remove (plugins_dir, p.dir, err, sizeof err))
    app_report (err);
  plugins_set_enabled (plugins_state, p.dir, 1);   /* forget it was off */
  plugins_changed ();
}

int app_is_plugin (const char *path)
{
  char f[1100];
  fsys_entry e;
  fsys_join (f, sizeof f, path, "Plugin");
  if (fsys_stat (f, &e) == 0 && !e.is_dir)
    return 1;
  fsys_join (f, sizeof f, path, "Plugin,fff");
  return fsys_stat (f, &e) == 0 && !e.is_dir;
}

void app_plugin_add (const char *path)
{
  char err[300], msg[400];
  plugin_info p;
  int rc;
  if (!plugins_dir[0])
    {
      app_report ("Lumen doesn't know where its Plugins directory is (Lumen$Dir isn't set).");
      return;
    }
  if (plugins_in_use ())
    return;
  rc = plugins_add (plugins_dir, path, 0, err, sizeof err);
  if (rc == 1)
    {
      plugins_read (path, &p);
      snprintf (msg, sizeof msg, "There is a plug-in called %s in Lumen already. Replace it with this one%s%s?",
                p.dir, p.version[0] ? ", version " : "", p.version);
      if (!app_confirm (msg, "Replace"))
        return;
      rc = plugins_add (plugins_dir, path, 1, err, sizeof err);
    }
  if (rc)
    {
      app_report (err);
      return;
    }
  plugins_changed ();
}

/* --- one Lumen at a time

   Two copies would open the same library. So a second copy looks for the
   first among the desktop's tasks (TaskManager_EnumerateTasks, PRM p658:
   4-word records of handle, name, slot size, flags), clicks the first's
   icon bar icon for it (a Mouse_Click event sent with Wimp_SendMessage
   to its task handle: the first opens its window) and quits. A handle's
   low 16 bits pick out the task, so only they are compared with our own.
   (A Lumen just started isn't listed yet: the Task Manager hears of new
   tasks as it polls.) */
#ifndef TaskManager_EnumerateTasks
#define TaskManager_EnumerateTasks 0x42681
#endif

static int name_is (const char *s, const char *want)
{
  size_t k;
  for (k = 0; want[k]; k++)
    if (s[k] != want[k])
      return 0;
  return (unsigned char) s[k] < 32;    /* control-terminated */
}

int app_other_instance (void)
{
  int next = 0, guard = 0;
  do
    {
      int buf[4 * 16], *end = buf, *r;
      if (_swix (TaskManager_EnumerateTasks, _INR (0, 2) | _OUT (0) | _OUT (1), next, buf, (int) sizeof buf,
                 &next, &end))
        return 0;                      /* no Task Manager: carry on */
      for (r = buf; r + 4 <= end; r += 4)
        if ((r[0] & 0xFFFF) != (task & 0xFFFF) && r[1] && !(r[3] & 1) && name_is ((const char *) r[1], APP_NAME))
          return r[0];
    }
  while (next >= 0 && ++guard < 64);
  return 0;
}

int app_init (const char *library_path)
{
  static const int messages[] = { Message_DataLoad, 2 /* DataSaveAck */, Message_ModeChange, Message_HelpRequest,
                                  Message_TaskWindow_Output, Message_TaskWindow_Ego, Message_TaskWindow_Morio, 0 };
  char err[300];
  if (_swix (Wimp_Initialise, _INR (0, 3) | _OUT (1), 380, TASK_WORD, APP_NAME, messages, &task))
    return 1;
  atexit (close_down);
  {
    int first = app_other_instance ();
    if (first)
      {
        int b[5];
        b[0] = 0; b[1] = 0; b[2] = Button_Select; b[3] = -2; b[4] = 0;
        _swix (Wimp_SendMessage, _INR (0, 2), Mouse_Click, b, first);
        close_down ();
        return 2;                      /* not an error: the first Lumen carries on */
      }
  }
  ensure_sprites ();
  draw_init_mode ();
  thumbcache_init (64u << 20);
  if (lib_open (&lib, library_path, err, sizeof err))
    {
      app_report (err);
      close_down ();
      return 1;
    }
  if (!prefs_path[0])
    app_load_prefs ();
  activity_init ();
  if (create_iconbar_icon () || browser_create () || viewer_create () || exportbox_create () || mainwin_create ()
      || plugwin_create () || choicewin_create () || activitywin_create ())
    {
      app_report ("Lumen couldn't create its windows.");
      close_down ();
      return 1;
    }
  menus_init ();
  if (the_prefs.thumb_size != browser_size ())
    browser_set_size (the_prefs.thumb_size);
  exportbox_use_prefs (&the_prefs);
  lib_set_lazy_thumbs (lib, 1);        /* thumbnails for what is on screen first */
  lib_set_new_folders (lib, the_prefs.watch_new, the_prefs.albums_new);
  app_plugins_rescan ();               /* also retries raw files imported before the plug-in was installed */
  mainwin_open (NULL);
  return 0;
}

void app_quit (void)
{
  lib_close (lib);
  lib = NULL;
  close_down ();
}

int app_busy (void)
{
  return viewer_busy () || lib_busy (lib) || thumbcache_pending () || exports_busy () || lib_verifying (lib)
         || rawjob_busy ();
}

static void thumb_made (void *ctx, cat_id v)
{
  (void) ctx;
  activity_thumb_made ();
  browser_thumb_ready (v);
}

/* Looks through the watched folders now (at start, when the window opens
   again, and every so many minutes). */
void app_watch_now (void)
{
  last_watch = app_now ();
  nwatched = lib_watch_start (lib);
}

/* When Lumen next wants a null event though it has nothing to do: the
   next look at the watched folders. 0 if never. */
int app_next_wake (unsigned *when)
{
  if (!lib || the_prefs.watch_minutes <= 0)
    return 0;
  if (nwatched < 0)
    {
      cat_watch *w;
      int n = 0;
      if (cat_watched (lib_cat (lib), &w, &n) == 0)
        free (w);
      nwatched = n;
    }
  if (nwatched == 0)
    return 0;
  *when = last_watch + (unsigned) the_prefs.watch_minutes * 6000u;
  return 1;
}

/* Lumen is in front: one of its windows has the input focus. */
int app_has_focus (void)
{
  int c[6];
  if (_swix (Wimp_GetCaretPosition, _IN (1), c) || c[0] == -1)
    return 0;
  return c[0] == mainwin_window () || c[0] == browser_window () || c[0] == viewer_window ()
         || c[0] == mainwin_inspector ();
}

/* Only thumbnails nobody is looking at are left to make, and Lumen isn't
   in front: Lumen makes them a little at a time and gives the rest of the
   desktop the computer in between (Wimp_PollIdle until *when). */
int app_background_wait (unsigned *when)
{
  if (!lib || viewer_busy () || thumbcache_pending () || exports_busy () || rawjob_busy ()
      || !lib_background_only (lib) || app_has_focus ())
    return 0;
  *when = app_now () + 4;
  return 1;
}

static void idle (void)
{
  unsigned start, deadline, wake;
  int changed;
  if (app_next_wake (&wake) && (int) (app_now () - wake) >= 0 && !lib_busy (lib))
    app_watch_now ();
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
      /* Checking the originals are still there, when nothing else is on. */
      if (lib_verifying (lib) && lib_verify_step (lib, app_now, deadline))
        {
          browser_reload ();
          mainwin_library_changed ();
          mainwin_info_changed ();
        }
      browser_update_title ();
      return;
    }
  /* Thumbnails off screen while another program is in front: a short
     slice (main gives the others the time between slices). */
  if (lib_background_only (lib) && !app_has_focus ())
    deadline = start + 1;
  changed = lib_work (lib, app_now, deadline, thumb_made, NULL);
  if (changed & LIB_LIST_CHANGED)
    {
      list_dirty = 1;
      nwatched = -1;                   /* an import may have added a watched folder */
    }
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
  if (b[5] != browser_window () && b[5] != mainwin_window () && b[5] != viewer_window ()
      && b[5] != mainwin_inspector () && b[5] != -2 && b[5] != plugwin_window () && b[5] != choicewin_window ())
    return;
  if (b[5] == choicewin_window ())
    {
      /* A directory for the library. */
      b[3] = b[2];
      b[4] = Message_DataLoadAck;
      _swix (Wimp_SendMessage, _INR (0, 2), User_Message, b, b[1]);
      choicewin_drop (path);
      return;
    }
  /* A plug-in, to the Add plug-in window or anywhere else in Lumen. */
  if (b[5] == plugwin_window () || ((type == FT_DIR || type == FT_APP) && app_is_plugin (path)))
    {
      b[3] = b[2];
      b[4] = Message_DataLoadAck;
      _swix (Wimp_SendMessage, _INR (0, 2), User_Message, b, b[1]);
      if (app_is_plugin (path))
        app_plugin_add (path);
      else
        app_report ("That isn't a Lumen plug-in: a plug-in is a directory with a Plugin file in it. "
                    "To add photos, drag them to Lumen's main window.");
      return;
    }
  if (lib_import (lib, path))
    {
      app_report ("Not enough memory to start the import.");
      return;
    }
  /* Acknowledge: same block, action DataLoadAck, your_ref = my_ref. */
  b[3] = b[2];
  b[4] = Message_DataLoadAck;
  _swix (Wimp_SendMessage, _INR (0, 2), User_Message, b, b[1]);
  /* Show what arrives. */
  {
    cat_source s = { SRC_LAST_IMPORT, 0, 0, "" };
    if (browser_source ()->kind != SRC_LAST_IMPORT)
      browser_set_source (&s);
    mainwin_set_mode (LAYOUT_BROWSER);
    mainwin_open (NULL);
  }
}

/* A key in the main window (it has the caret): the layouts first, as
   Aperture's (V, I, W, F); then what is showing. In Split View the
   Browser takes the keys that move about and rate, the Viewer the rest. */
static int app_key (int key)
{
  int mode = mainwin_mode ();
  int browsing = key == KEY_LEFT || key == KEY_RIGHT || key == KEY_UP || key == KEY_DOWN || key == KEY_HOME
                 || key == KEY_END || key == KEY_PAGEUP || key == KEY_PAGEDOWN || (key >= '0' && key <= '5')
                 || key == '9' || key == '/' || key == KEY_RETURN || key == KEY_CTRL_A;
  if (mainwin_key (key))
    return 1;
  if (mode == LAYOUT_BROWSER)
    return browser_key (key);
  if (mode == LAYOUT_SPLIT && browsing && browser_key (key))
    return 1;
  return viewer_key (key) || browser_key (key);
}

static int app_event_1 (int reason, int *b, int sender);

/* Every event; then the status area and the Activity window catch up with
   what is going on (jobs started and finished go into the history). */
int app_event (int reason, int *b, int sender)
{
  int r = app_event_1 (reason, b, sender);
  if (!r && reason != Redraw_Window_Request)
    activity_tick ();
  return r;
}

static int app_event_1 (int reason, int *b, int sender)
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
      else if (b[0] == mainwin_window ())
        mainwin_redraw (b);
      else if (b[0] == mainwin_inspector ())
        mainwin_inspector_redraw (b);
      else if (b[0] == viewer_window ())
        viewer_redraw (b);
      else if (b[0] == activitywin_window ())
        activitywin_redraw (b);
      else
        {
          int more = 0;
          _swix (Wimp_RedrawWindow, _IN (1) | _OUT (0), b, &more);
          while (more)
            _swix (Wimp_GetRectangle, _IN (1) | _OUT (0), b, &more);
        }
      break;
    case Open_Window_Request:
      if (b[0] == mainwin_window ())
        mainwin_open (b);
      else if (b[0] == browser_window ())
        browser_open (b);
      else if (b[0] == viewer_window ())
        viewer_open_window (b);
      else if (b[0] == mainwin_inspector ())
        mainwin_open_child (b);
      else
        _swix (Wimp_OpenWindow, _IN (1), b);
      break;
    case Close_Window_Request:
      if (b[0] == mainwin_window ())
        mainwin_close ();
      else if (b[0] == activitywin_window ())
        {
          activitywin_closed ();
          _swix (Wimp_CloseWindow, _IN (1), b);
        }
      else
        _swix (Wimp_CloseWindow, _IN (1), b);
      break;
    case Mouse_Click:
      if (b[3] == -2)                  /* the icon bar */
        {
          if (b[2] == Button_Menu)
            menus_open_iconbar_menu (b[0]);
          else
            mainwin_open (NULL);
        }
      else if (b[3] == mainwin_window ())
        mainwin_click (b);
      else if (b[3] == mainwin_inspector ())
        mainwin_inspector_click (b);
      else if (b[3] == browser_window ())
        browser_click (b);
      else if (b[3] == viewer_window ())
        viewer_click (b);
      else if (b[3] == exportbox_window ())
        exportbox_click (b);
      else if (b[3] == plugwin_window ())
        plugwin_click (b);
      else if (b[3] == choicewin_window ())
        choicewin_click (b);
      else if (b[3] == activitywin_window ())
        activitywin_click (b);
      break;
    case User_Drag_Box:
      if (exportbox_dragging ())
        exportbox_drag_end ();
      else
        viewer_drag_end ();
      break;
    case Key_Pressed:
      if (b[0] == exportbox_window () ? !exportbox_key (b[6])
          : b[0] == choicewin_window () ? !choicewin_key (b[6])
          : b[0] == mainwin_window () && b[1] >= 0 && b[1] == mainwin_search_icon () ? !mainwin_search_key (b[6])
          : !app_key (b[6]))
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
        case Message_HelpRequest:
          help_request (b);
          break;
        case 2:                        /* Message_DataSaveAck */
          exportbox_save_ack (b);
          break;
        case Message_TaskWindow_Ego:
        case Message_TaskWindow_Morio:
          raw_message (b);
          break;
        case Message_ModeChange:
          draw_init_mode ();
          browser_mode_changed ();
          viewer_mode_changed ();
          mainwin_mode_changed ();
          break;
        }
      break;
    }
  return 0;
}
