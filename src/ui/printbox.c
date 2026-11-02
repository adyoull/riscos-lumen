/* printbox.c: printing photos through the RISC OS printer drivers.

   The Print box (a transient dialogue, as the Export box) shows the
   printer chosen in !Printers and sets the copies, the size on the page
   (fit the page, fill it, or a given size in centimetres), whether a
   photo may be turned a quarter to suit the page, and sharpening for
   print. One photo goes on each page.

   Printing is in two parts. First each photo is developed, in the
   background as an export (export.h) into a JPEG in <Wimp$ScrapDir>, at
   the size it will be printed at the printer's resolution (at most 300
   dots an inch, never larger than the photo), sharpened for print. Then
   the job runs: each photo's JPEG is plotted with JPEG_PlotScaled in the
   printer driver's rectangle loop (PDriver_GiveRectangle with a transformation matrix
   that scales it, and turns it if need be, then PDriver_DrawPage and
   PDriver_GetRectangle; classic PRM, Printer drivers; the 2026 edition
   doesn't have the chapter). The job is a file opened on "printer:".

   Units: 1 OS unit is 1/180 inch is 400 millipoints; matrices are 16.16
   fixed point. */
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <kernel.h>
#include <swis.h>
#include "colour.h"
#include "export.h"
#include "fsys.h"
#include "thumbgen.h"
#include "ui.h"

#ifndef JPEG_Info
#define JPEG_Info 0x49980          /* SpriteExtend, PRM p197 */
#define JPEG_PlotScaled 0x49982
#endif

enum
{
  PB_PRINTER_LABEL, PB_PRINTER, PB_COPIES_LABEL, PB_COPIES, PB_SIZE_LABEL, PB_FIT, PB_FILL, PB_GIVEN, PB_W, PB_X, PB_H, PB_CM,
  PB_ROTATE, PB_SHARPEN, PB_INFO, PB_CANCEL, PB_PRINT, PB_COUNT
};
enum { SIZE_FIT, SIZE_FILL, SIZE_GIVEN };

#define BOX_W 820
#define BOX_H 520
#define T_LABEL   0x17000311u
#define T_NOTE    0x17000111u
#define T_WRITE   0x0700F135u
#define T_RADIO   0x1700B113u
#define T_OPTION  0x1700B113u          /* as a radio, but no group: Select toggles it */
#define T_BUTTON  0x1700313Du

static int win = -1, is_open;
static char printer_name[64] = "", copies_text[4] = "1", w_text[6] = "15", h_text[6] = "10", info[80] = "";
static int size_mode = SIZE_FIT, rotate = 1, sharpen = 1;
static cat_id *box_ids;
static int box_n;

/* The job being prepared and printed */
typedef struct
{
  cat_id id;
  char jpeg[256];                      /* the developed photo, in the scrap directory */
  int ready;
} page;
static page *pages;
static int npages, ipage, failed, copies;
static exporter *cur;
static char job_error[200];
static int printing;                   /* preparing or printing */
static int job_size_mode, job_rotate;
static double job_w_cm, job_h_cm;

int printbox_window (void) { return win; }
int printbox_busy (void) { return printing; }

static void icon_at (int *ic, int x0, int y0, int x1, int y1, unsigned flags, const char *text, const char *valid,
                     int size)
{
  ic[0] = x0; ic[1] = y0; ic[2] = x1; ic[3] = y1;
  ic[4] = (int) flags;
  ic[5] = (int) text; ic[6] = valid ? (int) valid : -1; ic[7] = size;
}

int printbox_create (void)
{
  static const char v_radio[] = "Sradiooff,radioon", v_option[] = "Soptoff,opton", v_num[] = "A0-9;Pptr_write";
  static const char v_dec[] = "A0-9.;Pptr_write", v_print[] = "R6,3", v_cancel[] = "R5,3";
  int b[22 + 8 * PB_COUNT];
  int *ic = &b[22];
  memset (b, 0, sizeof b);
  b[0] = 0; b[1] = 0; b[2] = BOX_W; b[3] = BOX_H;
  b[6] = -1;
  b[7] = (int) 0x84000012;
  ((unsigned char *) b)[32] = 7;  ((unsigned char *) b)[33] = 2;
  ((unsigned char *) b)[34] = 7;  ((unsigned char *) b)[35] = 1;
  ((unsigned char *) b)[36] = 3;  ((unsigned char *) b)[37] = 1;
  ((unsigned char *) b)[38] = 12;
  b[10] = 0; b[11] = -BOX_H; b[12] = BOX_W; b[13] = 0;
  b[14] = 0x3D;
  b[16] = 1;
  memcpy (&b[18], "Print", 6);
  b[21] = PB_COUNT;
  icon_at (ic + 8 * PB_PRINTER_LABEL, 24, -72, 196, -28, T_LABEL, "Printer", NULL, 8);
  icon_at (ic + 8 * PB_PRINTER, 212, -72, BOX_W - 24, -28, T_NOTE, printer_name, NULL, (int) sizeof printer_name);
  icon_at (ic + 8 * PB_COPIES_LABEL, 24, -132, 196, -88, T_LABEL, "Copies", NULL, 7);
  icon_at (ic + 8 * PB_COPIES, 212, -132, 300, -88, T_WRITE, copies_text, v_num, (int) sizeof copies_text);
  icon_at (ic + 8 * PB_SIZE_LABEL, 24, -192, 196, -148, T_LABEL, "Size", NULL, 5);
  icon_at (ic + 8 * PB_FIT, 212, -192, 420, -148, T_RADIO | (1u << 16) | (1u << 21), "Fit the page", v_radio, 13);
  icon_at (ic + 8 * PB_FILL, 436, -192, 690, -148, T_RADIO | (1u << 16), "Fill the page", v_radio, 14);
  icon_at (ic + 8 * PB_GIVEN, 212, -252, 300, -208, T_RADIO | (1u << 16), "", v_radio, 1);
  icon_at (ic + 8 * PB_W, 300, -252, 400, -208, T_WRITE, w_text, v_dec, (int) sizeof w_text);
  icon_at (ic + 8 * PB_X, 400, -252, 440, -208, 0x17000119u, "\xD7", NULL, 2);
  icon_at (ic + 8 * PB_H, 440, -252, 540, -208, T_WRITE, h_text, v_dec, (int) sizeof h_text);
  icon_at (ic + 8 * PB_CM, 548, -252, 760, -208, T_NOTE, "cm, at most", NULL, 12);
  icon_at (ic + 8 * PB_ROTATE, 212, -312, 760, -268, T_OPTION | (1u << 21), "Turn photos to suit the page", v_option, 29);
  icon_at (ic + 8 * PB_SHARPEN, 212, -372, 760, -328, T_OPTION | (1u << 21), "Sharpen for print", v_option, 18);
  icon_at (ic + 8 * PB_INFO, 24, -432, BOX_W - 24, -388, T_NOTE, info, NULL, (int) sizeof info);
  icon_at (ic + 8 * PB_CANCEL, BOX_W - 384, -500, BOX_W - 220, -444, T_BUTTON, "Cancel", v_cancel, 7);
  icon_at (ic + 8 * PB_PRINT, BOX_W - 200, -504, BOX_W - 24, -440, T_BUTTON, "Print", v_print, 6);
  return _swix (Wimp_CreateWindow, _IN (1) | _OUT (0), b, &win) ? -1 : 0;
}

static void set_selected (int icon, int on)
{
  int st[4];
  st[0] = win; st[1] = icon;
  st[2] = on ? 1 << 21 : 0;
  st[3] = 1 << 21;
  _swix (Wimp_SetIconState, _IN (1), st);
}

static void redraw_icon (int i)
{
  int s[4];
  s[0] = win; s[1] = i; s[2] = 0; s[3] = 0;
  _swix (Wimp_SetIconState, _IN (1), s);
}

static int is_on (int icon)
{
  int st[10];
  st[0] = win; st[1] = icon;
  if (_swix (Wimp_GetIconState, _IN (1), st))
    return 0;
  return (st[6] >> 21) & 1;
}

static void show_state (void)
{
  set_selected (PB_FIT, size_mode == SIZE_FIT);
  set_selected (PB_FILL, size_mode == SIZE_FILL);
  set_selected (PB_GIVEN, size_mode == SIZE_GIVEN);
  set_selected (PB_ROTATE, rotate);
  set_selected (PB_SHARPEN, sharpen);
}

/* The printer chosen in !Printers: its name, resolution and whether it is
   set to print in colour (PDriver_Info R3 bit 0: PostScript drivers print in
   grey unless Colour is ticked in the printer's Configure). 0 if there is one. */
static int printer_info (char *name, size_t n, int *dpi, int *colour)
{
  int xres = 0, yres = 0, features = 0;
  const char *nm = NULL;
  if (_swix (PDriver_Info, _OUTR (1, 4), &xres, &yres, &features, &nm))
    return -1;
  *colour = features & 1;
  snprintf (name, n, "%s", nm && *nm ? nm : "Printer");
  *dpi = xres < yres ? xres : yres;
  if (*dpi <= 0)
    *dpi = 300;
  return 0;
}

/* The printable part of the page, in millipoints (x0, y0, x1, y1). */
static int printable (int r[4], int *page_w, int *page_h)
{
  return _swix (PDriver_PageSize, _OUTR (1, 6), page_w, page_h, &r[0], &r[1], &r[2], &r[3]) ? -1 : 0;
}

void printbox_open (int from_viewer, int x, int y)
{
  int dpi, colour, r[4], pw, ph;
  if (printing)
    {
      app_report ("Lumen is already printing. Wait for it to finish, then try again.");
      return;
    }
  if (printer_info (printer_name, sizeof printer_name, &dpi, &colour))
    {
      app_report ("No printer is set up. Start !Printers (in the Printing directory) and choose a printer, "
                  "then try again.");
      return;
    }
  free (box_ids);
  box_ids = NULL;
  box_n = 0;
  if (from_viewer)
    {
      if (!viewer_item () || !(box_ids = malloc (sizeof *box_ids)))
        return;
      box_ids[0] = viewer_item ();
      box_n = 1;
    }
  else
    box_n = browser_selection (&box_ids);
  if (box_n <= 0)
    return;
  if (!colour)
    snprintf (info, sizeof info, "%d photo%s. Grey only: tick Colour in !Printers", box_n, box_n == 1 ? "" : "s");
  else if (printable (r, &pw, &ph) == 0)
    snprintf (info, sizeof info, "%d photo%s, one to a page: %.1f \xD7 %.1f cm paper", box_n, box_n == 1 ? "" : "s",
              pw / 72000.0 * 2.54, ph / 72000.0 * 2.54);
  else
    snprintf (info, sizeof info, "%d photo%s, one to a page", box_n, box_n == 1 ? "" : "s");
  show_state ();
  redraw_icon (PB_PRINTER);
  redraw_icon (PB_INFO);
  is_open = 1;
  _swix (Wimp_CreateMenu, _INR (1, 3), win, x - 64, y);
  _swix (Wimp_SetCaretPosition, _INR (0, 5), win, PB_COPIES, 0, 0, -1, (int) strlen (copies_text));
}

static void close_box (void)
{
  if (is_open)
    _swix (Wimp_CreateMenu, _IN (1), -1);
  is_open = 0;
}

/* --- placing a photo on the page
   For a photo of pw x ph pixels on the printable area a (millipoints),
   the size it is printed at (millipoints, as placed: w x h after any
   turn) and whether it is turned. */
void print_place (int pw, int ph, const int a[4], int mode, double w_cm, double h_cm, int may_turn, int *out_w,
                  int *out_h, int *turned)
{
  double aw = a[2] - a[0], ah = a[3] - a[1], s;
  int t = 0;
  if (may_turn && pw > 0 && ph > 0 && ((pw > ph) != (aw > ah)) && pw != ph)
    t = 1;
  if (t)
    {
      int k = pw;
      pw = ph;
      ph = k;
    }
  if (mode == SIZE_GIVEN && w_cm > 0 && h_cm > 0)
    {
      double gw = w_cm / 2.54 * 72000, gh = h_cm / 2.54 * 72000;
      if (t)
        {
          double k = gw;
          gw = gh;
          gh = k;
        }
      if (gw > aw) gw = aw;
      if (gh > ah) gh = ah;
      s = gw / pw < gh / ph ? gw / pw : gh / ph;
    }
  else if (mode == SIZE_FILL)
    s = aw / pw > ah / ph ? aw / pw : ah / ph;
  else
    s = aw / pw < ah / ph ? aw / pw : ah / ph;
  *out_w = (int) lround (pw * s);
  *out_h = (int) lround (ph * s);
  *turned = t;
}

/* --- preparing: each photo developed into a scrap JPEG */
static void scrap_path (int k, char *out, size_t n)
{
#ifdef __riscos__
  snprintf (out, n, "<Wimp$ScrapDir>.LumenPrint.P%d", k);
#else
  const char *d = getenv ("Lumen$Scratch");
  snprintf (out, n, "%s/LumenPrint-P%d.jpg", d && *d ? d : "/tmp", k);
#endif
}

static void finish_job (void);

/* The Print box's note, as last shown (for the tests). */
const char *printbox_info (void)
{
  return info;
}

static int start_next (void)
{
  int dpi = 300, colour, a[4], pw = 0, ph = 0;
  printer_info (printer_name, sizeof printer_name, &dpi, &colour);
  if (dpi > 300)
    dpi = 300;
  if (dpi < 150)
    dpi = 150;
  while (ipage < npages)
    {
      page *p = &pages[ipage];
      cat_details d;
      char rtext[RECIPE_TEXT_MAX], err[200];
      recipe r;
      export_opts o;
      int ow, oh, w, h, turned;
      if (cat_get_details (lib_cat (app_library ()), p->id, &d)
          || cat_get_recipe (lib_cat (app_library ()), p->id, rtext, sizeof rtext))
        {
          failed++;
          ipage++;
          continue;
        }
      if (d.missing)
        {
          if (!failed++)
            snprintf (job_error, sizeof job_error, "%s can't be found", fsys_leaf (d.path));
          ipage++;
          continue;
        }
      recipe_default (&r);
      recipe_parse (&r, rtext);
      /* How big it will be on paper, so how many pixels it needs. */
      if (printable (a, &pw, &ph))
        {
          a[0] = 0; a[1] = 0; a[2] = 595 * 1000; a[3] = 842 * 1000;   /* A4 */
        }
      {
        int fw = d.width, fh = d.height;
        if (d.orientation >= 5)
          {
            fw = d.height;
            fh = d.width;
          }
        export_plan_size (fw, fh, &r, 0, &ow, &oh);
      }
      print_place (ow, oh, a, job_size_mode, job_w_cm, job_h_cm, job_rotate, &w, &h, &turned);
      memset (&o, 0, sizeof o);
      o.format = EXPORT_JPEG;
      o.quality = 95;
      o.colour = CS_SRGB;
      o.out_sharpen = sharpen ? EXPORT_SHARP_HIGH : EXPORT_SHARP_NONE;
      /* Pixels needed: the printed size (as placed) at dpi; turned, the photo's own width is the paper's height. */
      {
        int need_w = (int) ceil ((turned ? h : w) / 72000.0 * dpi), need_h = (int) ceil ((turned ? w : h) / 72000.0 * dpi);
        o.box_w = need_w < 16 ? 16 : need_w;
        o.box_h = need_h < 16 ? 16 : need_h;
      }
      scrap_path (ipage, p->jpeg, sizeof p->jpeg);
      cur = export_start (d.path, d.width, d.height, d.orientation, &r, p->jpeg, &o, err, sizeof err);
      if (!cur)
        {
          if (!failed++)
            snprintf (job_error, sizeof job_error, "%s", err);
          ipage++;
          continue;
        }
      return 0;
    }
  return -1;                           /* all prepared */
}

void printbox_idle (unsigned deadline)
{
  if (!printing)
    return;
  do
    {
      int r;
      if (!cur && start_next ())
        {
          finish_job ();
          return;
        }
      r = export_step (cur, 32);
      if (r != 0)
        {
          if (r < 0 && !failed++)
            snprintf (job_error, sizeof job_error, "%s", export_error (cur));
          else if (r > 0)
            pages[ipage].ready = 1;
          export_free (cur);
          cur = NULL;
          ipage++;
        }
    }
  while ((int) (app_now () - deadline) < 0);
}

int printbox_percent (void)
{
  if (!printing || npages <= 0)
    return 0;
  return (ipage * 100 + (cur ? export_progress (cur) : 0)) / npages;
}

void printbox_status (char *out, size_t n)
{
  if (!printing)
    {
      out[0] = '\0';
      return;
    }
  snprintf (out, n, "Preparing to print %d of %d", ipage + 1 < npages ? ipage + 1 : npages, npages);
}

/* --- the job
   Each photo's JPEG is plotted with JPEG_PlotScaled at 1:1, which printer
   drivers take over (JPEG_PDriverIntercept): a PostScript driver sends the
   JPEG itself, others decode it at the printer's resolution. 1:1 maps a
   JPEG pixel to a pixel of the screen mode, (1 << eig) OS units, so the
   drawing is that big and the transformation matrix scales it onto the
   paper. (A 32bpp sprite was tried first: PDriverPS printed it as grey
   halftones.) */
static unsigned char *read_file (const char *path, size_t *len)
{
  FILE *f = fopen (path, "rb");
  unsigned char *buf = NULL;
  long n;
  if (!f)
    return NULL;
  if (fseek (f, 0, SEEK_END) == 0 && (n = ftell (f)) > 0 && fseek (f, 0, SEEK_SET) == 0 && (buf = malloc ((size_t) n)))
    {
      if (fread (buf, 1, (size_t) n, f) != (size_t) n)
        {
          free (buf);
          buf = NULL;
        }
      else
        *len = (size_t) n;
    }
  fclose (f);
  return buf;
}

static const _kernel_oserror *plot_page (const page *p, int a[4], int xeig, int yeig)
{
  size_t len = 0;
  unsigned char *jpeg = read_file (p->jpeg, &len);
  int iw = 0, ih = 0, w, h, turned, rect[4], m[4], pos[2], more = 0, id = 0, blk[4], ow, oh;
  const _kernel_oserror *e;
  static _kernel_oserror unreadable = { 0, "a prepared photo couldn't be read back (is the disc full?)" };
  if (!jpeg)
    return &unreadable;
  e = _swix (JPEG_Info, _INR (0, 2) | _OUTR (2, 3), 1, jpeg, len, &iw, &ih);
  if (e || iw <= 0 || ih <= 0)
    {
      free (jpeg);
      return e ? e : &unreadable;
    }
  print_place (iw, ih, a, job_size_mode, job_w_cm, job_h_cm, job_rotate, &w, &h, &turned);
  /* Our drawing: the JPEG at (0,0), ow x oh OS units. On paper an OS unit
     is 400 millipoints, so the scale is the printed size over (OS units *
     400); turned, a quarter turn anticlockwise. */
  ow = iw << xeig;
  oh = ih << yeig;
  {
    double s = turned ? (double) h / (ow * 400.0) : (double) w / (ow * 400.0);
    int f = (int) lround (s * 65536);
    rect[0] = 0; rect[1] = 0; rect[2] = ow; rect[3] = oh;
    if (!turned)
      {
        m[0] = f; m[1] = 0; m[2] = 0; m[3] = f;
        pos[0] = a[0] + ((a[2] - a[0]) - w) / 2;
        pos[1] = a[1] + ((a[3] - a[1]) - h) / 2;
      }
    else
      {
        /* x' = -s y, y' = s x: the picture's bottom left goes to the right. */
        m[0] = 0; m[1] = f; m[2] = -f; m[3] = 0;
        pos[0] = a[0] + ((a[2] - a[0]) - w) / 2 + w;
        pos[1] = a[1] + ((a[3] - a[1]) - h) / 2;
      }
  }
  e = _swix (PDriver_GiveRectangle, _INR (0, 4), 0, rect, m, pos, 0xFFFFFF00);
  if (!e)
    e = _swix (PDriver_DrawPage, _INR (0, 3) | _OUT (0) | _OUT (2), copies, blk, 0, 0, &more, &id);
  while (!e && more)
    {
      e = _swix (JPEG_PlotScaled, _INR (0, 5), jpeg, 0, 0, 0, len, 0);
      if (!e)
        e = _swix (PDriver_GetRectangle, _IN (1) | _OUT (0) | _OUT (2), blk, &more, &id);
    }
  free (jpeg);
  return e;
}

static void remove_scraps (void)
{
  int k;
  char dir[256];
  for (k = 0; k < npages; k++)
    if (pages[k].jpeg[0])
      remove (pages[k].jpeg);
#ifdef __riscos__
  snprintf (dir, sizeof dir, "<Wimp$ScrapDir>.LumenPrint");
  _swix (OS_File, _INR (0, 1), 6, dir);   /* the directory too (OS_File 6: delete; only if empty) */
#else
  (void) dir;
#endif
}

/* Lumen is quitting: a job being prepared is dropped and its scraps go. */
void printbox_quit (void)
{
  if (!printing)
    return;
  export_free (cur);
  cur = NULL;
  remove_scraps ();
  free (pages);
  pages = NULL;
  npages = ipage = failed = 0;
  printing = 0;
}

static void finish_job (void)
{
  int k, ready = 0, fh = 0, old = 0, a[4], pw, ph, printed = 0, xeig = 1, yeig = 1;
  const _kernel_oserror *e = NULL;
  /* The screen mode's pixel: what JPEG_PlotScaled's 1:1 means. */
  _swix (OS_ReadModeVariable, _INR (0, 1) | _OUT (2), -1, 4, &xeig);
  _swix (OS_ReadModeVariable, _INR (0, 1) | _OUT (2), -1, 5, &yeig);
  for (k = 0; k < npages; k++)
    ready += pages[k].ready;
  if (ready && printable (a, &pw, &ph))
    {
      static _kernel_oserror no_size = { 0, "Lumen couldn't read the page size from the printer driver." };
      e = &no_size;
    }
  if (ready && !e)
    {
      int selected = 0;
      /* OS_Find &8F: open out, errors if it can't. */
      e = _swix (OS_Find, _INR (0, 1) | _OUT (0), 0x8F, "printer:", &fh);
      if (!e)
        e = _swix (PDriver_SelectJob, _INR (0, 1) | _OUT (0), fh, "Lumen", &old);
      selected = fh && !e;
      _swix (Hourglass_On, 0);
      for (k = 0; !e && k < npages; k++)
        if (pages[k].ready)
          {
            e = plot_page (&pages[k], a, xeig, yeig);
            printed += !e;
          }
      _swix (Hourglass_Off, 0);
      if (selected)
        {
          /* An error, or EndJob failing: the job is thrown away, and none of
             it was printed. */
          if (!e)
            e = _swix (PDriver_EndJob, _IN (0), fh);
          if (e)
            {
              _swix (PDriver_AbortJob, _IN (0), fh);
              printed = 0;
            }
          _swix (PDriver_SelectJob, _INR (0, 1), old, 0);   /* the job that was current before ours */
        }
      if (fh)
        _swix (OS_Find, _INR (0, 1), 0, fh);
    }
  remove_scraps ();
  if (e)
    {
      char m[300];
      snprintf (m, sizeof m, "Printing stopped: %s", e->errmess);
      app_report (m);
    }
  else if (failed)
    {
      char m[300];
      snprintf (m, sizeof m, "%d of %d photos couldn't be printed. The first: %s", failed, npages,
                job_error[0] ? job_error : "it couldn't be prepared");
      app_report (m);
    }
  if (printed)
    activity_log (ACT_EXPORT, "Printed %d photo%s on %s%s", printed, printed == 1 ? "" : "s", printer_name,
                  copies > 1 ? " (more than one copy each)" : "");
  free (pages);
  pages = NULL;
  npages = ipage = failed = 0;
  printing = 0;
}

static void print_clicked (void)
{
  int k;
  copies = atoi (copies_text);
  if (copies < 1) copies = 1;
  if (copies > 99) copies = 99;
  job_size_mode = size_mode;
  job_rotate = rotate;
  job_w_cm = atof (w_text);
  job_h_cm = atof (h_text);
  if (size_mode == SIZE_GIVEN && (job_w_cm <= 0 || job_h_cm <= 0))
    {
      app_report ("Type the size the photos should be, in centimetres.");
      return;
    }
  pages = calloc ((size_t) box_n, sizeof *pages);
  if (!pages)
    return;
  for (k = 0; k < box_n; k++)
    pages[k].id = box_ids[k];
  npages = box_n;
  ipage = failed = 0;
  job_error[0] = '\0';
#ifdef __riscos__
  fsys_mkdirs ("<Wimp$ScrapDir>.LumenPrint");
#endif
  printing = 1;
  close_box ();
}

void printbox_click (int *b)
{
  if (b[2] == Button_Menu)
    return;
  switch (b[4])
    {
    case PB_FIT: size_mode = SIZE_FIT; break;
    case PB_FILL: size_mode = SIZE_FILL; break;
    case PB_GIVEN: size_mode = SIZE_GIVEN; break;
    case PB_W: case PB_H:
      if (size_mode != SIZE_GIVEN)
        {
          size_mode = SIZE_GIVEN;
          show_state ();
        }
      break;
    case PB_ROTATE: case PB_SHARPEN:       /* the Wimp ticks and unticks them */
      rotate = is_on (PB_ROTATE);
      sharpen = is_on (PB_SHARPEN);
      break;
    case PB_CANCEL: close_box (); break;
    case PB_PRINT: print_clicked (); break;
    }
}

int printbox_key (int key)
{
  if (key == KEY_RETURN)
    {
      print_clicked ();
      return 1;
    }
  if (key == KEY_ESCAPE)
    {
      close_box ();
      return 1;
    }
  return 0;
}
