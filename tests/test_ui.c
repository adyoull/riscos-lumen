#include <math.h>
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
#define VIEWER 0x2c000
#define TASK 0x1234

static char work[512];
static int nwindows, browser_block[23], viewer_block[23];
static int vopened[8], vopen_calls, vclosed, drag_block[10], drag_calls, ptr_x, ptr_y, ptr_buttons;
static char *vtitle;
static int last_plot_w, last_plot_h;
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
      else if (nwindows == 2)
        {
          memcpy (viewer_block, b, sizeof viewer_block);
          vtitle = (char *) b[18];
          r[0] = VIEWER;
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
      else if (b[0] == VIEWER)
        {
          memcpy (vopened, b, sizeof vopened);
          vopen_calls++;
        }
      return NULL;
    case Wimp_GetWindowState:
      CHECK (b[0] == BROWSER || b[0] == VIEWER, "window state: one of ours");
      memcpy (&b[1], b[0] == VIEWER ? &vopened[1] : &opened[1], 6 * sizeof (int));
      b[7] = -1; b[8] = 0;
      return NULL;
    case Wimp_DragBox:
      memcpy (drag_block, b, sizeof drag_block);
      drag_calls++;
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
      {
        const int *o = b[0] == VIEWER ? vopened : opened;
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
      b[0] = ptr_x; b[1] = ptr_y; b[2] = ptr_buttons;
      return NULL;
    case Wimp_ProcessKey:
      processed_key = r[0];
      return NULL;
    case Wimp_ReportError:
      snprintf (reported, sizeof reported, "%s", ((const _kernel_oserror *) r[0])->errmess);
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
      {
        const unsigned *sp = (const unsigned *) r[2];
        last_plot_w = (int) sp[4] + 1;
        last_plot_h = (int) sp[5] + 1;
      }
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
    { OS_SpriteOp, "OS_SpriteOp", 0 }, { OS_ReadMonotonicTime, "OS_ReadMonotonicTime", 0 },
    { Wimp_DragBox, "Wimp_DragBox", 1 } };
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
  b[0] = caret_window;                 /* the Wimp sends keys to the caret's window */
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
  CHECK (nwindows == 3, "the library, viewer and Info windows");
  CHECK (vopen_calls == 0, "the viewer stays shut at start");
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
  /* Adjust-click on another photo: both cells redrawn (the old focus frame goes). */
  key (KEY_ESCAPE);
  click (16 + 256 + 40, -16 - 40, Button_Select);   /* not the photo clicked earlier: that would be a double click */
  force_redraws = 0;
  click (16 + 2 * 256 + 40, -16 - 40, Button_Adjust);
  CHECK (force_redraws >= 2, "Adjust: the old focus cell and the new one are redrawn");

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

  /* ---- The viewer ---- */
  {
    int px, tx0, tx1, ev_mid, sx, sy, k;
    char text[256];
    cat_item it;
    browser_focus_on (0);
    browser_item (0, &it);
    key (KEY_RETURN);
    CHECK (vopen_calls == 1 && caret_window == VIEWER, "Return opens the viewer, with the input focus");
    CHECK (strstr (vtitle, "IMG_0001.jpg (1 of 6)") != NULL, "viewer title: the photo, 1 of 6");
    CHECK (app_busy (), "the proxy is decoded in idle time");
    memset (b, 0, sizeof b);
    b[0] = VIEWER;
    sprite_plots = 0;
    app_event (Redraw_Window_Request, b, 0);
    CHECK (sprite_plots == 1 && last_plot_w <= 256, "straight away: the thumbnail, enlarged");
    i = nulls_until_idle ();
    CHECK (i > 1, "the proxy took several slices");
    {
      int w, h;
      CHECK (viewer_proxy_size (&w, &h) && w == 640 && h == 480, "proxy: the whole 640x480 photo");
    }
    sprite_plots = 0; ntexts = 0;
    app_event (Redraw_Window_Request, b, 0);
    CHECK (sprite_plots == 1 && last_plot_w == 640, "then the developed proxy");
    CHECK (text_drawn ("Exposure") && text_drawn ("White Balance") && text_drawn ("Reset all"), "the panel: groups, sliders, buttons");

    /* Drag Exposure from 0 to +1 EV. Layout as viewer.c's. */
    px = (vopened[3] - vopened[1]) - 640;
    tx0 = px + 232; tx1 = px + 640 - 132;
    ev_mid = -24 - 150 - 16 - 52 - 84 - (52 + 2 * 56 + 8) - 52 - 28;
    sx = vopened[1] + (tx0 + tx1) / 2;
    sy = vopened[4] + ev_mid;
    memset (b, 0, sizeof b);
    b[0] = sx; b[1] = sy; b[2] = Button_Select; b[3] = VIEWER; b[4] = -1;
    app_event (Mouse_Click, b, 0);
    CHECK (drag_calls == 1 && drag_block[0] == VIEWER && drag_block[1] == 7, "a drag starts: Wimp_DragBox type 7");
    CHECK (drag_block[6] <= vopened[1] + tx0 && drag_block[8] >= vopened[1] + tx1
           && drag_block[7] < sy && drag_block[9] > sy, "the pointer is kept on the track's row");
    CHECK (app_busy (), "null events while dragging");
    ptr_x = vopened[1] + tx0 + (int) ((1.0 + 4) / 8 * (tx1 - tx0));
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
    b[0] = sx; b[1] = sy; b[2] = Button_Adjust; b[3] = VIEWER; b[4] = -1;
    app_event (Mouse_Click, b, 0);
    CHECK (viewer_recipe ()->v[P_EV] == 0 && drag_calls == 1, "Adjust: back to 0, no drag");
    key (0x1A);
    CHECK (viewer_recipe ()->v[P_EV] > 0.9f, "undo brings +1 back");
    /* Reset all, then undo it. */
    memset (b, 0, sizeof b);
    b[0] = vopened[1] + px + 24 + 2 * 202 + 93; b[1] = vopened[4] - 24 - 150 - 16 - 52 - 84 - 972 - 16 - 30;
    b[2] = Button_Select; b[3] = VIEWER; b[4] = -1;
    app_event (Mouse_Click, b, 0);
    CHECK (recipe_is_default (viewer_recipe ()), "Reset all");
    key (0x1A);
    CHECK (viewer_recipe ()->v[P_EV] > 0.9f, "and undo it");
    /* Rate it from the viewer. */
    key ('5');
    browser_item (0, &it);
    CHECK (it.rating == 5, "5 in the viewer: rated 5");
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
#define PHOTO_RECT()                                                          \
      do {                                                                    \
        int bw = vw - 640 - 48, bh = vh - 48, mul, div, dw, dh;               \
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

      /* Crop shape: Square, from the menu. */
      memset (b, 0, sizeof b);
      b[0] = vopened[1] + 200; b[1] = vopened[4] - 200; b[2] = Button_Menu; b[3] = VIEWER;
      app_event (Mouse_Click, b, 0);
      CHECK (strstr (item_text (menu_open, 7), "Crop shape") != NULL, "Photo > Crop shape");
      choose (7, 2, -1);
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
      key ('w');
      CHECK (viewer_picking (), "W: pick white balance");
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
      pm = submenu (menu_open, 9);
      CHECK (pm && strstr (item_text (pm, 0), "No presets") != NULL, "Presets: none yet");
      save = submenu (pm, 1);
      strcpy ((char *) item (save, 0)[3], "Bright");
      choose (9, 1, 0);
      {
        cat_preset *ps;
        int np;
        cat_presets (lib_cat (app_library ()), &ps, &np);
        CHECK (np == 1 && strcmp (ps[0].name, "Bright") == 0 && strstr (ps[0].recipe, "crop") == NULL,
               "saved as Bright: the look, no crop");
        free (ps);
      }
      viewer_reset_all ();
      app_event (Mouse_Click, b, 0);
      pm = submenu (menu_open, 9);
      CHECK (strcmp (item_text (pm, 0), "Bright") == 0, "Presets lists it");
      choose (9, 0, -1);
      CHECK (fabsf (viewer_recipe ()->v[P_EV] - kept.v[P_EV]) < 0.01f && viewer_recipe ()->v[P_CROP_X0] == 0,
             "applying it brings the look back (not the crop)");

      /* Copy here, paste on photo 2 from the library. */
      key (0x03);
      CHECK (looks_clip () && strstr (looks_clip (), "exposure") != NULL, "Ctrl-C copies the look");
      nulls_until_idle ();
#undef PHOTO_RECT
    }
    key (KEY_ESCAPE);
    CHECK (vclosed == 1 && caret_window == BROWSER && browser_focus () == 0, "Escape: back to the library, on the photo");
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
      CHECK (strstr (item_text (submenu (menu_open, 1), 7), "Apply preset") != NULL, "Selection > Apply preset");
      choose (1, 7, 0);
      browser_item (2, &it2);
      cat_get_recipe (lib_cat (app_library ()), it2.id, text, sizeof text);
      CHECK (strstr (text, "exposure") != NULL, "photo 3 gets Bright");
      nulls_until_idle ();
      browser_item (2, &it2);
      CHECK (it2.edited && it2.thumb_state == THUMB_READY, "and a new thumbnail");
      browser_focus_on (0);
    }

    /* A double click opens the viewer; its menu takes us back. */
    for (k = 0; k < 2; k++)
      click (16 + 352 + 60, -16 - 60, Button_Select);
    CHECK (vopen_calls == 2 && strstr (vtitle, "(2 of 6)") != NULL, "a double click opens the photo clicked");
    memset (b, 0, sizeof b);
    b[0] = vopened[1] + 200; b[1] = vopened[4] - 200; b[2] = Button_Menu; b[3] = VIEWER;
    app_event (Mouse_Click, b, 0);
    CHECK (menu_open && strncmp ((const char *) menu_open, "Photo", 5) == 0, "the viewer's menu: Photo");
    CHECK (strstr (item_text (menu_open, 12), "Back to the library") != NULL, "Back to the library");
    choose (12, -1, -1);
    CHECK (vclosed == 2, "Back to the library closes the viewer");
  }

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
