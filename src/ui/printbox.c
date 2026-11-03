/* printbox.c: printing photos through the RISC OS printer drivers.

   The Print box (a transient dialogue, as the Export box) shows the
   printer chosen in !Printers and sets the copies, the size on the page
   (fit the page, fill it, or a given size in centimetres), whether a
   photo may be turned a quarter to suit the page, and sharpening for
   print; and how many photos go on a page: one, 2, 4, 6 or 9 in equal
   cells, or as many of a given size as fit (to cut out), with a gap
   between them and, if asked, each photo's name under it (a contact
   sheet).

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
  PB_ROTATE, PB_SHARPEN, PB_INFO, PB_CANCEL, PB_PRINT,
  PB_PER_LABEL, PB_P1, PB_P2, PB_P4, PB_P6, PB_P9, PB_MANY, PB_GAP_LABEL, PB_GAP, PB_MM, PB_NAMES, PB_COUNT
};
static const int per_choices[] = { 1, 2, 4, 6, 9, 0 };   /* PB_P1 .. PB_MANY; 0: as many as fit */
enum { SIZE_FIT, SIZE_FILL, SIZE_GIVEN };

#define BOX_W 900
#define BOX_H 712
#define MAX_CELLS 64
#define NAME_STRIP 20000             /* millipoints under a photo for its name (about 7 mm) */
#define T_LABEL   0x17000311u
#define T_NOTE    0x17000111u
#define T_WRITE   0x0700F135u
#define T_RADIO   0x1700B113u
#define T_OPTION  0x1700B113u          /* as a radio, but no group: Select toggles it */
#define T_BUTTON  0x1700313Du

static int win = -1, is_open;
static char printer_name[64] = "", copies_text[4] = "1", w_text[6] = "15", h_text[6] = "10", info[100] = "", gap_text[4] = "5";
static int size_mode = SIZE_FIT, rotate = 1, sharpen = 1, per_page = 1, names;
static cat_id *box_ids;
static int box_n;

/* The job being prepared and printed */
typedef struct
{
  cat_id id;
  char jpeg[256];                      /* the developed photo, in the scrap directory */
  char name[64];
  int ready;
} page;
static page *pages;
static int npages, ipage, failed, copies;
static exporter *cur;
static char job_error[200];
static int printing;                   /* preparing or printing */
static int job_size_mode, job_rotate, job_names, job_cells[MAX_CELLS][4], job_ncells;
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
  icon_at (ic + 8 * PB_PER_LABEL, 24, -312, 196, -268, T_LABEL, "Per page", NULL, 9);
  {
    static const char *const t[] = { "1", "2", "4", "6", "9" };
    int k;
    for (k = 0; k < 5; k++)
      icon_at (ic + 8 * (PB_P1 + k), 212 + k * 84, -312, 288 + k * 84, -268, T_RADIO | (2u << 16), t[k], v_radio, 2);
  }
  icon_at (ic + 8 * PB_MANY, 212, -372, 520, -328, T_RADIO | (2u << 16), "As many as fit", v_radio, 15);
  icon_at (ic + 8 * PB_GAP_LABEL, 540, -372, 620, -328, T_LABEL, "Gap", NULL, 4);
  icon_at (ic + 8 * PB_GAP, 628, -372, 708, -328, T_WRITE, gap_text, v_num, (int) sizeof gap_text);
  icon_at (ic + 8 * PB_MM, 716, -372, 800, -328, T_NOTE, "mm", NULL, 3);
  icon_at (ic + 8 * PB_NAMES, 212, -432, 860, -388, T_OPTION, "Names under the photos", v_option, 23);
  icon_at (ic + 8 * PB_ROTATE, 212, -492, 860, -448, T_OPTION | (1u << 21), "Turn photos to suit the page", v_option, 29);
  icon_at (ic + 8 * PB_SHARPEN, 212, -552, 860, -508, T_OPTION | (1u << 21), "Sharpen for print", v_option, 18);
  icon_at (ic + 8 * PB_INFO, 24, -612, BOX_W - 24, -568, T_NOTE, info, NULL, (int) sizeof info);
  icon_at (ic + 8 * PB_CANCEL, BOX_W - 384, -692, BOX_W - 220, -636, T_BUTTON, "Cancel", v_cancel, 7);
  icon_at (ic + 8 * PB_PRINT, BOX_W - 200, -696, BOX_W - 24, -632, T_BUTTON, "Print", v_print, 6);
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
  set_selected (PB_NAMES, names);
  {
    int k;
    for (k = 0; k < 6; k++)
      set_selected (PB_P1 + k, per_page == per_choices[k]);
  }
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

static void update_info (void);

void printbox_open (int from_viewer, int x, int y)
{
  int dpi, colour;
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
  show_state ();
  update_info ();
  redraw_icon (PB_PRINTER);
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

/* --- the cells of a page
   per photos (1, 2, 4, 6 or 9) share the printable area a in equal cells,
   the grid chosen (2 x 3 or 3 x 2, say) for cells nearest a photo's 3:2;
   or (per 0) as many cells of w x h cm as fit, turned a quarter if more
   fit that way, the grid centred. gap (millipoints) between cells; strip
   (millipoints) under each for its name. Out: each cell's photo part
   (millipoints, x0 y0 x1 y1), in reading order. How many (0: none fit). */
int print_grid (const int a[4], int per, double w_cm, double h_cm, int gap, int strip, int may_turn, int cells[][4],
                int max)
{
  double aw = a[2] - a[0], ah = a[3] - a[1], cw = 0, ch = 0;
  int c = 0, r = 0, k, n;
  if (gap < 0)
    gap = 0;
  if (per > 0)
    {
      double best = 1e9;
      int cc;
      for (cc = 1; cc <= per; cc++)
        if (per % cc == 0)
          {
            int rr = per / cc;
            double w = (aw - (cc - 1) * gap) / cc, h = (ah - (rr - 1) * gap) / rr - strip, q;
            if (w <= 0 || h <= 0)
              continue;
            q = fabs (log ((w > h ? w / h : h / w) / 1.5));
            if (q < best)
              {
                best = q;
                c = cc;
                r = rr;
                cw = w;
                ch = h + strip;
              }
          }
    }
  else if (w_cm > 0 && h_cm > 0)
    {
      double gw = w_cm / 2.54 * 72000, gh = h_cm / 2.54 * 72000;
      int t;
      for (t = 0; t <= (may_turn ? 1 : 0); t++)
        {
          double w = t ? gh : gw, h = (t ? gw : gh) + strip;
          int cc = (int) floor ((aw + gap) / (w + gap)), rr = (int) floor ((ah + gap) / (h + gap));
          if (cc > 0 && rr > 0 && cc * rr > c * r)
            {
              c = cc;
              r = rr;
              cw = w;
              ch = h;
            }
        }
    }
  n = c * r;
  if (n > max)
    n = max;
  {
    double x0 = a[0] + (aw - (c * cw + (c - 1) * gap)) / 2, top = a[3] - (ah - (r * ch + (r - 1) * gap)) / 2;
    for (k = 0; k < n; k++)
      {
        double x = x0 + (k % c) * (cw + gap), y1 = top - (k / c) * (ch + gap);
        cells[k][0] = (int) lround (x);
        cells[k][2] = (int) lround (x + cw);
        cells[k][3] = (int) lround (y1);
        cells[k][1] = (int) lround (y1 - ch + strip);
      }
  }
  return n;
}

/* The printable area, or A4 with no margins if the driver won't say. */
static void print_area (int a[4], int *pw, int *ph)
{
  if (printable (a, pw, ph))
    {
      a[0] = 0; a[1] = 0; a[2] = 595 * 1000; a[3] = 842 * 1000;
      *pw = a[2];
      *ph = a[3];
    }
}

static int gap_mp (void)
{
  int mm = atoi (gap_text);
  if (mm < 0) mm = 0;
  if (mm > 50) mm = 50;
  return (int) lround (mm / 25.4 * 72000);
}

/* How many photos go on a page with the box's settings now (0: none fit). */
static int cells_now (int cells[][4])
{
  int a[4], pw, ph;
  print_area (a, &pw, &ph);
  return print_grid (a, per_page, atof (w_text), atof (h_text), gap_mp (), names ? NAME_STRIP : 0, rotate, cells,
                     MAX_CELLS);
}

/* The box's note: what will be printed, on how many pages of what paper. */
static void update_info (void)
{
  int dpi = 0, colour = 1, a[4], pw, ph, per, pages;
  static int cells[MAX_CELLS][4];
  char what[60], paper[40] = "";
  printer_info (printer_name, sizeof printer_name, &dpi, &colour);
  per = cells_now (cells);
  pages = per > 0 ? (box_n + per - 1) / per : 0;
  if (printable (a, &pw, &ph) == 0)
    snprintf (paper, sizeof paper, "%.1f \xD7 %.1f cm", pw / 72000.0 * 2.54, ph / 72000.0 * 2.54);
  if (per == 1)
    snprintf (what, sizeof what, "%d photo%s, one to a page", box_n, box_n == 1 ? "" : "s");
  else
    snprintf (what, sizeof what, "%d photo%s, %d to a page", box_n, box_n == 1 ? "" : "s", per);
  if (per <= 0)
    snprintf (info, sizeof info, "%s \xD7 %s cm photos don't fit on this paper", w_text, h_text);
  else if (!colour)
    snprintf (info, sizeof info, "%s. Grey only: tick Colour in !Printers", what);
  else if (!paper[0])
    snprintf (info, sizeof info, "%s", what);
  else if (pages > 1 && per > 1)
    snprintf (info, sizeof info, "%s: %d pages of %s", what, pages, paper);
  else
    snprintf (info, sizeof info, "%s: %s paper", what, paper);
  redraw_icon (PB_INFO);
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
      snprintf (p->name, sizeof p->name, "%s", d.name[0] ? d.name : fsys_leaf (d.path));
      /* How big it will be on paper, so how many pixels it needs: the
         cells are all the same size. */
      memcpy (a, job_cells[0], sizeof a);
      (void) pw; (void) ph;
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

/* Where a JPEG of iw x ih pixels goes in cell c (millipoints): the part
   of our drawing (OS units, the JPEG plotted 1:1 at 0,0) to print, the
   matrix and where that part's bottom left goes on the paper. On paper
   an OS unit is 400 millipoints, so the scale is the printed size over
   (OS units * 400); turned, a quarter turn anticlockwise. Filling a cell,
   only the part inside it is printed. */
static void place_in (int iw, int ih, const int c[4], int xeig, int yeig, int rect[4], int m[4], int pos[2])
{
  int w, h, turned, ow = iw << xeig, oh = ih << yeig, f;
  double k, px0, py0, vx0, vx1, vy0, vy1, r0, r1, r2, r3;
  print_place (iw, ih, c, job_size_mode, job_w_cm, job_h_cm, job_rotate, &w, &h, &turned);
  k = (turned ? (double) h : (double) w) / ow;        /* millipoints of paper per OS unit of drawing */
  f = (int) lround (k / 400.0 * 65536);
  px0 = c[0] + ((c[2] - c[0]) - w) / 2.0;
  py0 = c[1] + ((c[3] - c[1]) - h) / 2.0;
  vx0 = px0 > c[0] ? px0 : c[0];
  vx1 = px0 + w < c[2] ? px0 + w : c[2];
  vy0 = py0 > c[1] ? py0 : c[1];
  vy1 = py0 + h < c[3] ? py0 + h : c[3];
  if (!turned)
    {
      r0 = (vx0 - px0) / k; r2 = (vx1 - px0) / k;
      r1 = (vy0 - py0) / k; r3 = (vy1 - py0) / k;
    }
  else
    {
      /* x' = Ox - k y, y' = Oy + k x, with O the placed picture's bottom right. */
      double ox = px0 + w, oy = py0;
      r0 = (vy0 - oy) / k; r2 = (vy1 - oy) / k;
      r1 = (ox - vx1) / k; r3 = (ox - vx0) / k;
    }
  rect[0] = (int) floor (r0 + 1e-6); rect[1] = (int) floor (r1 + 1e-6);
  rect[2] = (int) ceil (r2 - 1e-6);  rect[3] = (int) ceil (r3 - 1e-6);
  if (rect[0] < 0) rect[0] = 0;
  if (rect[1] < 0) rect[1] = 0;
  if (rect[2] > ow) rect[2] = ow;
  if (rect[3] > oh) rect[3] = oh;
  if (!turned)
    {
      m[0] = f; m[1] = 0; m[2] = 0; m[3] = f;
      pos[0] = (int) lround (px0 + k * rect[0]);
      pos[1] = (int) lround (py0 + k * rect[1]);
    }
  else
    {
      m[0] = 0; m[1] = f; m[2] = -f; m[3] = 0;
      pos[0] = (int) lround (px0 + w - k * rect[1]);
      pos[1] = (int) lround (py0 + k * rect[0]);
    }
}

/* A name under a photo: painted in the font at (0,0) of its own drawing
   (OS units, baseline at 0), at its real size, centred in the strip under
   the cell; cut off at the cell's width. */
#define NAME_ABOVE 26                  /* OS units of the drawing above the baseline, and below it */
#define NAME_BELOW 8
static void place_name (int font, const char *name, const int c[4], int rect[4], int m[4], int pos[2])
{
  int tw = 0, dummy = 0, cw = c[2] - c[0], wmp;
  if (_swix (Font_ScanString, _INR (0, 4) | _OUTR (3, 4), font, name, 1 << 8, 0x7FFFFFFF, 0x7FFFFFFF, &tw, &dummy))
    tw = 0;
  wmp = tw < cw ? tw : cw;             /* millipoints */
  rect[0] = 0; rect[1] = -NAME_BELOW; rect[2] = (wmp + 399) / 400; rect[3] = NAME_ABOVE;
  m[0] = 65536; m[1] = 0; m[2] = 0; m[3] = 65536;
  pos[0] = c[0] + (cw - wmp) / 2;
  pos[1] = c[1] - NAME_STRIP + (NAME_STRIP - (NAME_ABOVE + NAME_BELOW) * 400) / 2;
}

/* One page: the photos k0 .. k0 + n - 1 of the ready ones, each in its cell. */
static const _kernel_oserror *plot_sheet (page **items, int n, int xeig, int yeig, int font)
{
  static _kernel_oserror unreadable = { 0, "a prepared photo couldn't be read back (is the disc full?)" };
  unsigned char *jpeg[MAX_CELLS];
  size_t len[MAX_CELLS];
  const _kernel_oserror *e = NULL;
  int i, more = 0, id = 0, blk[4];
  memset (jpeg, 0, sizeof jpeg);
  for (i = 0; !e && i < n; i++)
    {
      int iw = 0, ih = 0, rect[4], m[4], pos[2];
      len[i] = 0;
      if (!(jpeg[i] = read_file (items[i]->jpeg, &len[i])))
        {
          e = &unreadable;
          break;
        }
      e = _swix (JPEG_Info, _INR (0, 2) | _OUTR (2, 3), 1, jpeg[i], len[i], &iw, &ih);
      if (!e && (iw <= 0 || ih <= 0))
        e = &unreadable;
      if (e)
        break;
      place_in (iw, ih, job_cells[i], xeig, yeig, rect, m, pos);
      e = _swix (PDriver_GiveRectangle, _INR (0, 4), 2 * i, rect, m, pos, 0xFFFFFF00);
      if (!e && font > 0 && items[i]->name[0])
        {
          place_name (font, items[i]->name, job_cells[i], rect, m, pos);
          e = _swix (PDriver_GiveRectangle, _INR (0, 4), 2 * i + 1, rect, m, pos, 0xFFFFFF00);
        }
    }
  if (!e)
    e = _swix (PDriver_DrawPage, _INR (0, 3) | _OUT (0) | _OUT (2), copies, blk, 0, 0, &more, &id);
  while (!e && more)
    {
      i = id / 2;
      if (i >= 0 && i < n)
        {
          if (id & 1)
            {
              _swix (ColourTrans_SetFontColours, _INR (0, 3), font, 0xFFFFFF00, 0x00000000, 14);
              e = _swix (Font_Paint, _INR (0, 4), font, items[i]->name, (1 << 4) | (1 << 8), 0, 0);
            }
          else
            e = _swix (JPEG_PlotScaled, _INR (0, 5), jpeg[i], 0, 0, 0, len[i], 0);
        }
      if (!e)
        e = _swix (PDriver_GetRectangle, _IN (1) | _OUT (0) | _OUT (2), blk, &more, &id);
    }
  for (i = 0; i < n; i++)
    free (jpeg[i]);
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
  int k, ready = 0, fh = 0, old = 0, a[4], pw, ph, printed = 0, sheets = 0, xeig = 1, yeig = 1;
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
      {
        static page *items[MAX_CELLS];
        int font = 0, per = job_ncells > 0 ? job_ncells : 1, got = 0;
        /* 9 point Homerton for the names (Font_FindFont: sizes in 1/16 point). */
        if (job_names && _swix (Font_FindFont, _INR (1, 5) | _OUT (0), "Homerton.Medium", 9 * 16, 9 * 16, 0, 0, &font))
          font = 0;
        for (k = 0; !e && k <= npages; k++)
          {
            if (k < npages && pages[k].ready)
              items[got++] = &pages[k];
            if (got == per || (k == npages && got))
              {
                e = plot_sheet (items, got, xeig, yeig, font);
                if (!e)
                  {
                    printed += got;
                    sheets++;
                  }
                got = 0;
              }
          }
        if (font > 0)
          _swix (Font_LoseFont, _IN (0), font);
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
    {
      char on[40] = "";
      if (sheets < printed)
        snprintf (on, sizeof on, ", %d to a page,", job_ncells);
      activity_log (ACT_EXPORT, "Printed %d photo%s%s on %s%s", printed, printed == 1 ? "" : "s", on, printer_name,
                    copies > 1 ? " (more than one copy each)" : "");
    }
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
  if (per_page == 0)
    job_size_mode = SIZE_GIVEN;          /* as many as fit: of the given size */
  if (job_size_mode == SIZE_GIVEN && (job_w_cm <= 0 || job_h_cm <= 0))
    {
      app_report ("Type the size the photos should be, in centimetres.");
      return;
    }
  job_names = names;
  job_ncells = cells_now (job_cells);
  if (job_ncells <= 0)
    {
      char m[200];
      snprintf (m, sizeof m, "%s \xD7 %s cm photos don't fit on this paper. Choose a smaller size, or fewer to a page.",
                w_text, h_text);
      app_report (m);
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
    case PB_FIT: case PB_FILL:
      size_mode = b[4] == PB_FIT ? SIZE_FIT : SIZE_FILL;
      if (per_page == 0)
        per_page = 1;                  /* as many as fit is of a given size */
      show_state ();
      update_info ();
      break;
    case PB_GIVEN: size_mode = SIZE_GIVEN; update_info (); break;
    case PB_W: case PB_H:
      if (size_mode != SIZE_GIVEN)
        {
          size_mode = SIZE_GIVEN;
          show_state ();
        }
      update_info ();
      break;
    case PB_P1: case PB_P2: case PB_P4: case PB_P6: case PB_P9: case PB_MANY:
      per_page = per_choices[b[4] - PB_P1];
      if (per_page == 0 && size_mode != SIZE_GIVEN)
        size_mode = SIZE_GIVEN;
      show_state ();
      update_info ();
      break;
    case PB_GAP:
      update_info ();
      break;
    case PB_ROTATE: case PB_SHARPEN: case PB_NAMES:   /* the Wimp ticks and unticks them */
      rotate = is_on (PB_ROTATE);
      sharpen = is_on (PB_SHARPEN);
      names = is_on (PB_NAMES);
      update_info ();
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
