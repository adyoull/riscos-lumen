/* Screenshot harness: runs Lumen's desktop front end against the Fake
   RISC OS, as test_ui does, and records what it draws (rectangles,
   triangles, text, sprites, menus, its window's state) to OUT/ops.txt at
   chosen moments. render.py turns the record into pictures. Nothing here
   is checked: test_ui does that.

   Usage: shot PHOTOS_DIR OUT_DIR */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include "fake.h"
#include "swis.h"
#include "ui.h"
#include "widths.h"

#define BROWSER 0x2a000
#define INFO 0x2b000
#define TASK 0x1234
#define SCREEN_W 3200                  /* OS units: a 1600 x 900 pixel mode, eig 1 */
#define SCREEN_H 1800

static FILE *ops;
static char outdir[512];
static int sprites;
static unsigned gcol;
static int px[3], py[3];               /* the last three OS_Plot points */
static int opened[8], vopened[8];
static char *vtitle;
#define VIEWER 0x2c000
static char *title_buf;
static unsigned clock_cs;
static int caret;
static int extent[4];

static void move (int x, int y)
{
  px[0] = px[1]; py[0] = py[1];
  px[1] = px[2]; py[1] = py[2];
  px[2] = x; py[2] = y;
}

static void dump_menu (const int *m, int depth);

static const _kernel_oserror *swi (int n, int r[10])
{
  int *b = (int *) r[1];
  switch (n)
    {
    case Wimp_Initialise:
      r[0] = 380; r[1] = TASK;
      return NULL;
    case Wimp_CreateIcon:
      r[0] = 1;
      return NULL;
    case Wimp_CreateWindow:
      if (!title_buf)
        {
          title_buf = (char *) b[18];
          r[0] = BROWSER;
        }
      else if (!vtitle)
        {
          vtitle = (char *) b[18];
          r[0] = VIEWER;
        }
      else
        r[0] = INFO;
      return NULL;
    case Wimp_OpenWindow:
      if (b[0] == BROWSER)
        memcpy (opened, b, sizeof opened);
      else if (b[0] == VIEWER)
        memcpy (vopened, b, sizeof vopened);
      return NULL;
    case Wimp_GetWindowState:
      memcpy (&b[1], b[0] == VIEWER ? &vopened[1] : &opened[1], 6 * sizeof (int));
      b[7] = -1; b[8] = 0;
      return NULL;
    case Wimp_SetExtent:
      memcpy (extent, b, sizeof extent);
      return NULL;
    case Wimp_RedrawWindow:
      {
        const int *o = b[0] == VIEWER ? vopened : opened;
        memcpy (&b[1], &o[1], 6 * sizeof (int));
        b[7] = o[1]; b[8] = o[2]; b[9] = o[3]; b[10] = o[4];
      }
      if (ops)
        fprintf (ops, "CLIP %d %d %d %d\n", b[7], b[8], b[9], b[10]);
      r[0] = 1;
      return NULL;
    case Wimp_GetRectangle:
      r[0] = 0;
      return NULL;
    case Wimp_SetCaretPosition:
      caret = r[0];
      return NULL;
    case Wimp_TextOp:
      if ((r[0] & 0xFF) == 1)
        {
          const unsigned char *s = (const unsigned char *) r[1];
          int w = 0, k = 0, lim = r[2];
          while (s[k] >= 32 && (lim == 0 || k < lim))
            w += char_width[s[k++]];
          r[0] = w;
        }
      else if ((r[0] & 0xFF) == 0)
        {
          static unsigned fg, bg;
          fg = (unsigned) r[1]; bg = (unsigned) r[2];
          if (ops)
            fprintf (ops, "TCOL %08x %08x\n", fg, bg);
        }
      else if ((r[0] & 0xFF) == 2 && ops)
        fprintf (ops, "TEXT %d %d %s\n", r[4], r[5], (const char *) r[1]);
      return NULL;
    case Wimp_CreateMenu:
      if (ops && b)
        {
          fprintf (ops, "MENU %d %d\n", r[2], r[3]);
          dump_menu (b, 0);
          fprintf (ops, "ENDMENU\n");
        }
      return NULL;
    case Wimp_GetPointerInfo:
      b[2] = 0;
      return NULL;
    case OS_ReadModeVariable:
      r[2] = r[1] == 4 || r[1] == 5 ? 1 : r[1] == 11 ? SCREEN_W / 2 - 1 : SCREEN_H / 2 - 1;
      return NULL;
    case OS_ReadMonotonicTime:
      r[0] = (int) (clock_cs += 1);
      return NULL;
    case ColourTrans_SetGCOL:
      gcol = (unsigned) r[0];
      return NULL;
    case ColourTrans_SelectTable:
      r[4] = 0;
      return NULL;
    case OS_Plot:
      move (r[1], r[2]);
      if (!ops)
        return NULL;
      if (r[0] == 101)
        fprintf (ops, "RECT %d %d %d %d %08x\n", px[1], py[1], px[2], py[2], gcol);
      else if (r[0] == 85)
        fprintf (ops, "TRI %d %d %d %d %d %d %08x\n", px[0], py[0], px[1], py[1], px[2], py[2], gcol);
      return NULL;
    case OS_SpriteOp:
      if (ops)
        {
          const unsigned *sp = (const unsigned *) r[2];
          const int *sc = (const int *) r[6];
          int w = (int) sp[4] + 1, h = (int) sp[5] + 1;
          char path[700];
          FILE *f;
          snprintf (path, sizeof path, "%s/spr%d.rgb", outdir, sprites);
          f = fopen (path, "wb");
          if (f)
            {
              int i;
              const unsigned *pix = sp + 11;
              for (i = 0; i < w * h; i++)
                {
                  unsigned char c[3] = { (unsigned char) pix[i], (unsigned char) (pix[i] >> 8), (unsigned char) (pix[i] >> 16) };
                  fwrite (c, 1, 3, f);
                }
              fclose (f);
            }
          /* Destination size: pixels = source * mul / div; 2 OS units a pixel. */
          if (sc)
            fprintf (ops, "SPRITE spr%d.rgb %d %d %d %d %d %d\n", sprites, w, h, r[3], r[4],
                     (w * sc[0] / sc[2]) * 2, (h * sc[1] / sc[3]) * 2);
          else                         /* unscaled (Lumen's own smoothed copy) */
            fprintf (ops, "SPRITE spr%d.rgb %d %d %d %d %d %d\n", sprites, w, h, r[3], r[4], w * 2, h * 2);
          sprites++;
        }
      return NULL;
    }
  return NULL;                         /* the rest: nothing to record */
}

static void dump_menu (const int *m, int depth)
{
  int i = 0;
  fprintf (ops, "M %d %d %.12s\n", depth, m[4], (const char *) m);
  for (;;)
    {
      const int *it = m + 7 + 6 * i;
      const char *text = (const char *) it[3];
      int has_sub = it[1] != -1;
      fprintf (ops, "I %d %d %d %d %s\n", depth, it[0], (it[2] >> 22) & 1, has_sub,
               (it[0] & 0x04) ? "[writable]" : (text ? text : ""));
      if (has_sub && it[1] != INFO)
        dump_menu ((const int *) it[1], depth + 1);
      if (it[0] & 0x80)
        break;
      i++;
    }
}

static void fakes (void)
{
  static const int list[] = {
    Wimp_Initialise, Wimp_CreateIcon, Wimp_CreateWindow, Wimp_OpenWindow, Wimp_GetWindowState,
    Wimp_SetExtent, Wimp_ForceRedraw, Wimp_SetCaretPosition, Wimp_RedrawWindow, Wimp_GetRectangle,
    Wimp_TextOp, Wimp_SendMessage, Wimp_CreateMenu, Wimp_GetPointerInfo, Wimp_ProcessKey,
    Wimp_ReportError, Wimp_CloseWindow, Wimp_CloseDown, OS_ReadModeVariable, ColourTrans_SetGCOL,
    ColourTrans_SelectTable, OS_Plot, OS_SpriteOp, OS_ReadMonotonicTime };
  size_t i;
  fake_reset ();
  for (i = 0; i < sizeof list / sizeof list[0]; i++)
    fake_swi (list[i], NULL, swi);
}

static void redraw (void)
{
  int b[64] = { 0 };
  b[0] = BROWSER;
  app_event (Redraw_Window_Request, b, 0);
}

static void begin_shot (const char *name)
{
  fprintf (ops, "SHOT %s\n", name);
  fprintf (ops, "WINDOW %d %d %d %d %d %d %d 1\n", opened[1], opened[2], opened[3], opened[4],
           opened[5], opened[6], caret == BROWSER);
  fprintf (ops, "TITLE %s\n", title_buf);
  fprintf (ops, "EXTENT %d %d %d %d\n", extent[0], extent[1], extent[2], extent[3]);
}

/* Null events until done or n calls; the Wimp redraws after each change,
   which is what queues thumbnails for loading. */
static void run (int n)
{
  int b[64], k;
  for (k = 0; k < n && app_busy (); k++)
    {
      app_event (Null_Reason, b, 0);
      if (k % 4 == 0)
        {
          FILE *save = ops;
          ops = NULL;
          redraw ();
          ops = save;
        }
    }
}

static void click (int index, int buttons, int cols, int cell_w, int cell_h)
{
  int b[64] = { 0 };
  int col = index % cols, row = index / cols;
  int wx = 16 + col * cell_w + cell_w / 2, wy = -(16 + row * cell_h + cell_h / 2);
  b[0] = wx + opened[1] - opened[5];
  b[1] = wy + opened[4] - opened[6];
  b[2] = buttons;
  b[3] = BROWSER;
  app_event (Mouse_Click, b, 0);
}

static void open_at (int x0, int y0, int w, int h)
{
  int b[64] = { 0 };
  b[0] = BROWSER; b[1] = x0; b[2] = y0; b[3] = x0 + w; b[4] = y0 + h;
  b[5] = 0; b[6] = 0; b[7] = -1;
  app_event (Open_Window_Request, b, 0);
}

static int menu_choose (int a, int b2, int c)
{
  int blk[64];
  blk[0] = a; blk[1] = b2; blk[2] = c; blk[3] = -1;
  return app_event (Menu_Selection, blk, 0);
}

static char opsname[700];

static void viewer_shot (const char *name)
{
  int vb[64] = { 0 };
  ops = fopen (opsname, "a");
  fprintf (ops, "SHOT %s\n", name);
  fprintf (ops, "WINDOW %d %d %d %d 0 0 1 0\n", vopened[1], vopened[2], vopened[3], vopened[4]);
  fprintf (ops, "TITLE %s\n", vtitle);
  fprintf (ops, "EXTENT 0 -1800 3200 0\n");
  vb[0] = VIEWER;
  app_event (Redraw_Window_Request, vb, 0);
  fclose (ops);
  ops = NULL;
}

int main (int argc, char **argv)
{
  char lib[700];
  int b[64];
  cat_item *items;
  int n, i;
  if (argc < 3)
    return 2;
  snprintf (outdir, sizeof outdir, "%s", argv[2]);
  mkdir (outdir, 0777);
  snprintf (lib, sizeof lib, "%s/Library", outdir);
  snprintf (opsname, sizeof opsname, "%s/ops.txt", outdir);
  ops = NULL;

  fakes ();
  if (app_init (lib))
    return 1;
  open_at (240, 220, 1800, 1300);

  /* 1. Importing: the folder dropped from the Filer, half the thumbnails made. */
  memset (b, 0, sizeof b);
  b[0] = 256; b[1] = 0x5678; b[2] = 77; b[4] = Message_DataLoad;
  b[5] = BROWSER; b[6] = -1; b[10] = 0x1000;
  snprintf ((char *) &b[11], 200, "%s", argv[1]);
  app_event (User_Message_Recorded, b, 0x5678);
  {
    lib_progress p;
    do
      {
        run (1);
        lib_get_progress (app_library (), &p);
      }
    while (app_busy () && (p.importing || p.thumbs_pending > 9));
  }
  run (10);                            /* let the loaded ones reach the screen */
  ops = fopen (opsname, "w");
  if (!ops)
    return 1;
  begin_shot ("importing");
  redraw ();
  fclose (ops);
  ops = NULL;
  run (100000);

  /* 2. The project, large thumbnails, rated and flagged, two selected. */
  {
    cat_group *g;
    int ng;
    cat_groups (lib_cat (app_library ()), SRC_PROJECT, &g, &ng);
    if (ng)
      {
        cat_source s = { SRC_PROJECT, g[0].id, 0 };
        browser_set_source (&s);
      }
    free (g);
  }
  browser_set_size (2);
  {
    static const int ratings[15] = { 3, 4, 5, 2, 5, 4, 3, 2, 5, 4, 3, 4, 3, 2, 4 };
    static const int flags[15] = { 0, 1, 0, 0, 1, 0, 0, 0, 1, 0, 0, 0, 0, 0, 0 };
    cat_source s = *browser_source ();
    cat_list (lib_cat (app_library ()), &s, &items, &n);
    for (i = 0; i < n && i < 15; i++)
      {
        int one = ratings[i];
        cat_set_rating (lib_cat (app_library ()), &items[i].id, 1, one);
        if (flags[i])
          cat_set_flag (lib_cat (app_library ()), &items[i].id, 1, 1);
      }
    /* Catbells is edited: warmer, a touch brighter, more contrast and vibrancy. */
    if (n > 4)
      {
        cat_set_recipe (lib_cat (app_library ()), items[4].id,
                        "wb temp=18 tint=0\nexposure ev=0.25\nlevels black=2.0 white=100.0 mid=8\n"
                        "enhance contrast=22 saturation=0 vibrance=30\n");
        lib_thumbs_changed (app_library ());
      }
    free (items);
    browser_after_change ();
  }
  run (100000);
  redraw ();                           /* queues the large thumbnails */
  run (100000);
  click (4, Button_Select, 5, 353, 420);
  click (8, Button_Adjust, 5, 353, 420);
  ops = fopen (opsname, "a");
  begin_shot ("library");
  redraw ();
  fclose (ops);
  ops = NULL;

  /* 3. The Lumen menu over it. */
  ops = fopen (opsname, "a");
  begin_shot ("menu");
  redraw ();
  {
    int blk[64] = { 0 };
    blk[0] = opened[1] + 1260; blk[1] = opened[4] - 300; blk[2] = Button_Menu; blk[3] = BROWSER;
    app_event (Mouse_Click, blk, 0);
  }
  fclose (ops);
  ops = NULL;

  /* 4. Flagged only, medium size. */
  menu_choose (0, 1, -1);
  browser_set_size (1);
  run (100000);
  redraw ();
  run (100000);
  ops = fopen (opsname, "a");
  begin_shot ("flagged");
  redraw ();
  fclose (ops);
  ops = NULL;

  /* 5. The viewer on the edited photo. */
  {
    cat_group *g;
    int ng;
    cat_groups (lib_cat (app_library ()), SRC_PROJECT, &g, &ng);
    if (ng)
      {
        cat_source s = { SRC_PROJECT, g[0].id, 0 };
        browser_set_source (&s);
      }
    free (g);
    browser_set_size (2);
    viewer_open (4);
    run (100000);
    viewer_shot ("viewer");
  }

  /* 6. The crop tool, square, over the whole picture. */
  viewer_crop_start ();
  viewer_set_crop_shape (2);
  run (100000);
  viewer_shot ("crop");

  /* 7. Cropped, auto adjusted, with the thirds grid. */
  viewer_crop_finish (1);
  viewer_auto ();
  viewer_set_grid (1);
  run (100000);
  viewer_shot ("auto");

  /* 8. Straightened and keystoned, with the fine grid. */
  viewer_reset_all ();
  viewer_set (P_ANGLE, 6);
  viewer_set (P_SHARPEN, 60);
  viewer_set (P_PERSP_V, 40);
  viewer_set_grid (3);
  run (100000);
  viewer_shot ("straighten");

  app_quit ();
  printf ("shot: %d sprites recorded in %s\n", sprites, outdir);
  return 0;
}
