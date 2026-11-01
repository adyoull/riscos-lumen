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
#include "ui.h"

#define BROWSER 0x2a000
#define INFO 0x2b000
#define TASK 0x1234

static char work[512];
static int nwindows, browser_block[23];
static char *title_buf;
static int iconbar[9];
static int opened[8], open_calls;
static int extent[4];
static int caret_window;
static int force_redraws, title_redraws;
static int sprite_plots, last_scale[4];
static int stars;
static char texts[64][80];
static int ntexts;
static int sent_reason, sent_block[64], sent_to;
static const int *menu_open;
static char reported[256];
static int scroll_y;
static int closedown;
static int processed_key;

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
      memcpy (iconbar, b, sizeof iconbar);
      r[0] = 1;
      return NULL;
    case Wimp_CreateWindow:
      if (nwindows++ == 0)
        {
          memcpy (browser_block, b, sizeof browser_block);
          title_buf = (char *) b[18];
          r[0] = BROWSER;
        }
      else
        r[0] = INFO;
      return NULL;
    case Wimp_OpenWindow:
      if (b[0] == BROWSER)
        {
          memcpy (opened, b, sizeof opened);
          scroll_y = b[6];
          open_calls++;
        }
      return NULL;
    case Wimp_GetWindowState:
      CHECK (b[0] == BROWSER, "window state: the browser");
      memcpy (&b[1], &opened[1], 6 * sizeof (int));
      b[7] = -1; b[8] = 0;
      return NULL;
    case Wimp_SetExtent:
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
      return NULL;
    case Wimp_RedrawWindow:
      /* The whole visible area, one rectangle. */
      memcpy (&b[1], &opened[1], 6 * sizeof (int));
      b[7] = opened[1]; b[8] = opened[2]; b[9] = opened[3]; b[10] = opened[4];
      r[0] = 1;
      return NULL;
    case Wimp_GetRectangle:
      r[0] = 0;
      return NULL;
    case Wimp_TextOp:
      if ((r[0] & 0xFF) == 1)
        r[0] = (int) strlen ((const char *) r[1]) * 16;
      else if ((r[0] & 0xFF) == 2 && ntexts < 64)
        snprintf (texts[ntexts++], sizeof texts[0], "%s", (const char *) r[1]);
      return NULL;
    case Wimp_SendMessage:
      sent_reason = r[0];
      memcpy (sent_block, b, sizeof sent_block);
      sent_to = r[2];
      return NULL;
    case Wimp_CreateMenu:
      menu_open = b;
      return NULL;
    case Wimp_GetPointerInfo:
      b[2] = 0;
      return NULL;
    case Wimp_ProcessKey:
      processed_key = r[0];
      return NULL;
    case Wimp_ReportError:
      snprintf (reported, sizeof reported, "%s", ((const _kernel_oserror *) r[0])->errmess);
      return NULL;
    case Wimp_CloseWindow:
      return NULL;
    case Wimp_CloseDown:
      CHECK (r[0] == TASK && r[1] == 0x4B534154, "Wimp_CloseDown: task handle, TASK");
      closedown++;
      return NULL;
    }
  return NULL;
}

static unsigned clock_cs;

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
      memcpy (last_scale, (const int *) r[6], sizeof last_scale);
      sprite_plots++;
      return NULL;
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
    { Wimp_RedrawWindow, "Wimp_RedrawWindow", 1 }, { Wimp_GetRectangle, "Wimp_GetRectangle", 1 },
    { Wimp_TextOp, "Wimp_TextOp", 1 }, { Wimp_SendMessage, "Wimp_SendMessage", 1 },
    { Wimp_CreateMenu, "Wimp_CreateMenu", 1 }, { Wimp_GetPointerInfo, "Wimp_GetPointerInfo", 1 },
    { Wimp_ProcessKey, "Wimp_ProcessKey", 1 }, { Wimp_ReportError, "Wimp_ReportError", 1 },
    { Wimp_CloseWindow, "Wimp_CloseWindow", 1 }, { Wimp_CloseDown, "Wimp_CloseDown", 1 },
    { OS_ReadModeVariable, "OS_ReadModeVariable", 0 }, { ColourTrans_SetGCOL, "ColourTrans_SetGCOL", 0 },
    { ColourTrans_SelectTable, "ColourTrans_SelectTable", 0 }, { OS_Plot, "OS_Plot", 0 },
    { OS_SpriteOp, "OS_SpriteOp", 0 }, { OS_ReadMonotonicTime, "OS_ReadMonotonicTime", 0 } };
  size_t i;
  fake_reset ();
  for (i = 0; i < sizeof list / sizeof list[0]; i++)
    fake_swi (list[i].swi, list[i].name, list[i].wimp ? wimp : graphics);
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
  b[0] = BROWSER;
  b[6] = k;
  app_event (Key_Pressed, b, 0);
}

/* Menu block helpers: a 28-byte header, then 24-byte items. */
static const int *item (const int *menu, int i) { return menu + 7 + 6 * i; }
static const char *item_text (const int *menu, int i) { return (const char *) item (menu, i)[3]; }
static const int *submenu (const int *menu, int i) { return (const int *) item (menu, i)[1]; }

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

  fakes ();
  CHECK (app_init (lib) == 0, "app_init succeeds");
  CHECK (strcmp ((const char *) &iconbar[6], "!lumen") == 0 && iconbar[0] == -1, "icon bar icon !lumen, on the right");
  CHECK (nwindows == 2, "the library window and the Info box");
  CHECK (((unsigned) browser_block[7] & 0xBF000002u) == 0xBF000002u, "browser: title, close, back, toggle, scroll, resize");
  CHECK (open_calls == 1 && caret_window == BROWSER, "the library window opens with the input focus");
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

  i = nulls_until_idle ();
  CHECK (i > 6, "imported over many null events");
  CHECK (!app_busy (), "and finished");
  CHECK (strstr (title_buf, "Last import (6)") != NULL, "title: 6 photos");
  CHECK (title_redraws > 0, "the title bar was redrawn");
  CHECK (extent[1] <= -1080, "extent set");

  /* First redraw: placeholders, thumbnails queued; then they load. */
  redraw ();
  CHECK (sprite_plots == 0, "first redraw: no sprites yet");
  CHECK (text_drawn ("IMG_0001.jpg") && text_drawn ("IMG_0006.jpg"), "names drawn");
  CHECK (stars == 6 * 5 * 10, "five stars of ten triangles under each photo");
  CHECK (app_busy (), "thumbnails queued for loading");
  force_redraws = 0;
  nulls_until_idle ();
  CHECK (force_redraws >= 6, "each loaded thumbnail's cell redrawn");
  redraw ();
  CHECK (sprite_plots == 6, "second redraw: six thumbnails");
  CHECK (last_scale[0] == 224 && last_scale[2] == (256 << 1), "scaled to the 224-unit box, eig 1");

  /* Click the first photo, rate it 4, flag it. */
  click (16 + 40, -16 - 40, Button_Select);
  key ('4');
  key ('/');
  {
    cat_source s = { SRC_FLAGGED, 0, 0 };
    cat_list (lib_cat (app_library ()), &s, &items, &n);
    CHECK (n == 1 && items[0].rating == 4 && strcmp (items[0].name, "IMG_0001.jpg") == 0, "rated 4 and flagged");
    free (items);
  }
  /* Right arrow: the next one alone. Ctrl-A, then 2: all rated 2. */
  key (KEY_RIGHT);
  key ('5');
  key (KEY_CTRL_A);
  key ('2');
  {
    cat_source s = { SRC_ALL, 0, 2 };
    cat_list (lib_cat (app_library ()), &s, &items, &n);
    CHECK (n == 6, "Ctrl-A then 2: all six rated");
    for (i = 0; i < n; i++)
      CHECK (items[i].rating == 2, "each rated 2");
    free (items);
  }
  key ('x');
  CHECK (processed_key == 'x', "keys Lumen doesn't use are passed on");

  /* The window menu. */
  click (5, -5, Button_Menu);
  CHECK (menu_open && strncmp ((const char *) menu_open, "Lumen", 5) == 0, "Menu: the Lumen menu");
  CHECK (strcmp (item_text (menu_open, 0), "Show") == 0 && strcmp (item_text (menu_open, 4), "Quit") == 0,
         "Show ... Quit");
  CHECK ((item (menu_open, 4)[0] & 0x80) != 0, "Quit is the last item");
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
  choose (3, 2, -1);
  redraw ();
  CHECK (last_scale[0] == 320, "Large: a 320-unit box");

  /* The icon bar menu: Quit. */
  memset (b, 0, sizeof b);
  b[0] = 1000; b[2] = Button_Menu; b[3] = -2;
  app_event (Mouse_Click, b, 0);
  CHECK (menu_open && strcmp (item_text (menu_open, 2), "Quit") == 0, "icon bar menu: Info, Open library, Quit");
  CHECK (submenu (menu_open, 0) == (const int *) INFO, "Info opens the Info box");
  CHECK (choose (2, -1, -1) == 1, "Quit from the icon bar menu quits");
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
