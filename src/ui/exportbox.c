/* exportbox.c: the Export dialogue, a standard RISC OS save box, and the
   queue of exports it starts.

   The box (opened as a transient dialogue with Wimp_CreateMenu) has the
   file's icon to drag, its name, the format (JPEG or PNG) and JPEG
   quality, the size (Full, the longer side at 3840, 2048 or 1024 pixels,
   or fitting inside a box), the colour space (sRGB, Adobe RGB, Display
   P3), sharpening for the output, and the pattern that names the files
   when several photos are exported (naming.h). What was used last is
   kept in Choices. Drag the icon to a directory viewer, or to another program,
   or type a full pathname and click Save. Several photos at once go into
   a new directory of that name, one file each.

   Saving follows the data transfer protocol (PRM p1005-1008): at the end
   of the drag, Message_DataSave goes to the window under the pointer
   with the proposed leafname; its Message_DataSaveAck gives the full
   pathname. If the receiver is another program (the path is
   <Wimp$Scrap>, estimated size -1), Message_DataLoad goes back to it
   once the file is written.

   Icon flags and button types are from the classic PRM (the 2026 edition
   doesn't list them): bit 0 text, 1 sprite, 2 border, 3 centred across,
   4 centred up and down, 5 filled, 8 indirected, 12-15 button type (3
   click, 6 click/drag, 11 radio, 15 writable), 16-20 exclusive selection
   group, 21 selected, 24-27 foreground, 28-31 background colour. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <kernel.h>
#include <swis.h>
#include "colour.h"
#include "export.h"
#include "naming.h"
#include "fsys.h"
#include "ui.h"

#define Message_DataSave 1
#define Message_DataSaveAck 2

enum
{
  I_FILE, I_NAME, I_FORMAT_LABEL, I_JPEG, I_PNG, I_SIZE_LABEL, I_FULL, I_3840, I_2048, I_1024,
  I_QUALITY_LABEL, I_QUALITY, I_CANCEL, I_SAVE,
  I_FIT, I_BOX_W, I_BOX_X, I_BOX_H, I_BOX_PX,
  I_COLOUR_LABEL, I_SRGB, I_ADOBE, I_P3,
  I_SHARP_LABEL, I_SH_NONE, I_SH_LOW, I_SH_STD, I_SH_HIGH,
  I_NAMES_LABEL, I_NAMES, I_NAMES_HINT, I_COUNT
};
static const int size_choice[4] = { 0, 3840, 2048, 1024 };

#define BOX_W 880
#define BOX_H 724

static int win = -1, is_open;
static char name[256] = "Photo", quality[4] = "90", sprite_name[16] = "file_c85";
static char box_w_text[6] = "1920", box_h_text[6] = "1080", names[128] = "{name}";
static int fit_on;                     /* Size: Fit in the box */
static export_opts opts = { EXPORT_JPEG, 90, 0, APP_NAME " " LUMEN_VERSION, 0, 0, CS_SRGB, EXPORT_SHARP_NONE };
static cat_id *box_ids;                /* the photos the box was opened for */
static int box_n;
static int dragging, save_ref;

/* The queue */
typedef struct { cat_id id; char dst[1024]; } job;
static job *jobs;
static int njobs, ijob, failed;
static exporter *cur;
static export_opts job_opts;
static char first_error[200];
static int reply_task, reply_block[64], reply_pending;   /* scrap transfer to another program */

int exportbox_window (void) { return win; }
int exportbox_dragging (void) { return dragging; }
const export_opts *exportbox_options (void) { return &opts; }

/* --- the window */
static void icon_at (int *ic, int x0, int y0, int x1, int y1, unsigned flags,
                     const char *text, const char *valid, int size)
{
  ic[0] = x0; ic[1] = y0; ic[2] = x1; ic[3] = y1;
  ic[4] = (int) flags;
  ic[5] = (int) text; ic[6] = valid ? (int) valid : -1; ic[7] = size;
}

#define T_LABEL   0x17000311u          /* text, vcentred, indirected, right-justified; black on grey */
#define T_WRITE   0x0700F135u          /* text, border, vcentred, filled, indirected, writable; black on white */
#define T_RADIO   0x1700B113u          /* text+sprite, vcentred, indirected, radio; on the window's grey (colour 1), not white */
#define T_BUTTON  0x1700313Du          /* text, border, centred, vcentred, filled, indirected, click; on grey */

int exportbox_create (void)
{
  static const char v_radio[] = "Sradiooff,radioon", v_name[] = "Pptr_write", v_num[] = "A0-9;Pptr_write";
  static const char v_save[] = "R6,3", v_cancel[] = "R5,3";
  int b[22 + 8 * I_COUNT];
  int *ic = &b[22];
  memset (b, 0, sizeof b);
  b[0] = 0; b[1] = 0; b[2] = BOX_W; b[3] = BOX_H;
  b[6] = -1;
  b[7] = (int) 0x84000012;             /* moveable, title, new format */
  ((unsigned char *) b)[32] = 7;  ((unsigned char *) b)[33] = 2;
  ((unsigned char *) b)[34] = 7;  ((unsigned char *) b)[35] = 1;
  ((unsigned char *) b)[36] = 3;  ((unsigned char *) b)[37] = 1;
  ((unsigned char *) b)[38] = 12;
  b[10] = 0; b[11] = -BOX_H; b[12] = BOX_W; b[13] = 0;
  b[14] = 0x3D;                        /* title: text, border, centred, filled */
  b[15] = 0;
  b[16] = 1;
  memcpy (&b[18], "Export", 7);
  b[21] = I_COUNT;
  /* The file: a sprite from the Wimp's pool, dragged to save. */
  icon_at (ic + 8 * I_FILE, BOX_W / 2 - 60, -128, BOX_W / 2 + 60, -20, 0x0000611Au, sprite_name, (const char *) 1,
           (int) sizeof sprite_name);
  icon_at (ic + 8 * I_NAME, 24, -192, BOX_W - 24, -140, T_WRITE, name, v_name, (int) sizeof name);
  /* Format and quality */
  icon_at (ic + 8 * I_FORMAT_LABEL, 24, -252, 176, -208, T_LABEL, "Format", NULL, 7);
  icon_at (ic + 8 * I_JPEG, 196, -252, 330, -208, T_RADIO | (1u << 16) | (1u << 21), "JPEG", v_radio, 5);
  icon_at (ic + 8 * I_PNG, 344, -252, 470, -208, T_RADIO | (1u << 16), "PNG", v_radio, 4);
  icon_at (ic + 8 * I_QUALITY_LABEL, 480, -252, 620, -208, T_LABEL, "Quality", NULL, 8);
  icon_at (ic + 8 * I_QUALITY, 636, -252, 736, -208, T_WRITE, quality, v_num, (int) sizeof quality);
  /* Size: full, a longer side, or fit inside a box */
  icon_at (ic + 8 * I_SIZE_LABEL, 24, -312, 176, -268, T_LABEL, "Size", NULL, 5);
  icon_at (ic + 8 * I_FULL, 196, -312, 316, -268, T_RADIO | (2u << 16) | (1u << 21), "Full", v_radio, 5);
  icon_at (ic + 8 * I_3840, 330, -312, 450, -268, T_RADIO | (2u << 16), "3840", v_radio, 5);
  icon_at (ic + 8 * I_2048, 464, -312, 584, -268, T_RADIO | (2u << 16), "2048", v_radio, 5);
  icon_at (ic + 8 * I_1024, 598, -312, 718, -268, T_RADIO | (2u << 16), "1024", v_radio, 5);
  icon_at (ic + 8 * I_FIT, 196, -372, 330, -328, T_RADIO | (2u << 16), "Fit in", v_radio, 7);
  icon_at (ic + 8 * I_BOX_W, 344, -372, 454, -328, T_WRITE, box_w_text, v_num, (int) sizeof box_w_text);
  icon_at (ic + 8 * I_BOX_X, 454, -372, 494, -328, 0x17000119u, "\xD7", NULL, 2);   /* centred */
  icon_at (ic + 8 * I_BOX_H, 494, -372, 604, -328, T_WRITE, box_h_text, v_num, (int) sizeof box_h_text);
  icon_at (ic + 8 * I_BOX_PX, 612, -372, 760, -328, 0x17000111u, "pixels", NULL, 7);
  /* Colour space */
  icon_at (ic + 8 * I_COLOUR_LABEL, 24, -432, 176, -388, T_LABEL, "Colour", NULL, 7);
  icon_at (ic + 8 * I_SRGB, 196, -432, 330, -388, T_RADIO | (3u << 16) | (1u << 21), "sRGB", v_radio, 5);
  icon_at (ic + 8 * I_ADOBE, 330, -432, 570, -388, T_RADIO | (3u << 16), "Adobe RGB", v_radio, 10);
  icon_at (ic + 8 * I_P3, 584, -432, 830, -388, T_RADIO | (3u << 16), "Display P3", v_radio, 11);
  /* Sharpening for the output */
  icon_at (ic + 8 * I_SHARP_LABEL, 24, -492, 176, -448, T_LABEL, "Sharpen", NULL, 8);
  icon_at (ic + 8 * I_SH_NONE, 196, -492, 330, -448, T_RADIO | (4u << 16) | (1u << 21), "None", v_radio, 5);
  icon_at (ic + 8 * I_SH_LOW, 344, -492, 454, -448, T_RADIO | (4u << 16), "Low", v_radio, 4);
  icon_at (ic + 8 * I_SH_STD, 464, -492, 680, -448, T_RADIO | (4u << 16), "Standard", v_radio, 9);
  icon_at (ic + 8 * I_SH_HIGH, 694, -492, 830, -448, T_RADIO | (4u << 16), "High", v_radio, 5);
  /* Names, for several photos */
  icon_at (ic + 8 * I_NAMES_LABEL, 24, -552, 176, -508, T_LABEL, "Names", NULL, 6);
  icon_at (ic + 8 * I_NAMES, 196, -552, BOX_W - 24, -508, T_WRITE, names, v_name, (int) sizeof names);
  icon_at (ic + 8 * I_NAMES_HINT, 196, -592, BOX_W - 24, -556, 0x17000111u,
           "For example {date}_{name}_{nnn}", NULL, 32);
  icon_at (ic + 8 * I_CANCEL, BOX_W - 384, -700, BOX_W - 220, -644, T_BUTTON, "Cancel", v_cancel, 7);
  icon_at (ic + 8 * I_SAVE, BOX_W - 200, -704, BOX_W - 24, -640, T_BUTTON, "Save", v_save, 5);
  return _swix (Wimp_CreateWindow, _IN (1) | _OUT (0), b, &win) ? -1 : 0;
}

static void redraw_icon (int i)
{
  int s[4];
  s[0] = win; s[1] = i; s[2] = 0; s[3] = 0;   /* Wimp_SetIconState, no change: redraws it */
  _swix (Wimp_SetIconState, _IN (1), s);
}

static void update_sprite (void)
{
  snprintf (sprite_name, sizeof sprite_name, "%s",
            box_n > 1 ? "directory" : opts.format == EXPORT_PNG ? "file_b60" : "file_c85");
  if (win != -1)
    redraw_icon (I_FILE);
}

/* A leafname from a photo's name: no extension, nothing RISC OS
   wouldn't take in a filename. */
static void leaf_from (const char *photo, char *out, size_t n)
{
  size_t i, k = 0;
  const char *dot = strrchr (photo, '.');
  const char *slash = strrchr (photo, '/');
  const char *end = photo + strlen (photo);
  /* The original's extension goes ("/jpg", ".png", a raw file's "/NEF"...):
     the export is a new file of its own type. */
  const char *ext = dot && (!slash || dot > slash) ? dot : slash;
  if (ext && ext > photo && strlen (ext + 1) >= 2 && strlen (ext + 1) <= 4)
    {
      const char *c = ext + 1;
      while (*c && ((*c >= 'a' && *c <= 'z') || (*c >= 'A' && *c <= 'Z') || (*c >= '0' && *c <= '9')))
        c++;
      if (!*c)
        end = ext;
    }
  for (i = 0; photo + i < end && k + 1 < n; i++)
    {
      char c = photo[i];
      if (c == ' ' || c == '.' || c == ':' || c == '$' || c == '&' || c == '@' || c == '^'
          || c == '%' || c == '\\' || c == '#' || c == '*' || c == '"' || c == '|' || c < 33)
        c = c == '.' ? '/' : '_';
      out[k++] = c;
    }
  out[k] = '\0';
  if (!k)
    snprintf (out, n, "Photo");
}

void exportbox_open (int from_viewer, int x, int y)
{
  free (box_ids);
  box_ids = NULL;
  box_n = 0;
  if (from_viewer)
    {
      if (!viewer_item ())
        return;
      box_ids = malloc (sizeof *box_ids);
      if (!box_ids)
        return;
      box_ids[0] = viewer_item ();
      box_n = 1;
    }
  else
    box_n = browser_selection (&box_ids);
  if (box_n <= 0)
    return;
  if (box_n == 1)
    {
      cat_details d;
      if (cat_get_details (lib_cat (app_library ()), box_ids[0], &d) == 0)
        leaf_from (fsys_leaf (d.path), name, sizeof name);
      else
        snprintf (name, sizeof name, "Photo");
    }
  else
    snprintf (name, sizeof name, "Lumen_export");
  update_sprite ();
  is_open = 1;
  _swix (Wimp_CreateMenu, _INR (1, 3), win, x - 64, y);
  _swix (Wimp_SetCaretPosition, _INR (0, 5), win, I_NAME, 0, 0, -1, (int) strlen (name));
}

static void close_box (void)
{
  if (is_open)
    _swix (Wimp_CreateMenu, _IN (1), -1);
  is_open = 0;
}

/* --- the queue */
int exports_busy (void)
{
  return njobs > 0 || cur != NULL;     /* until all_done has reported and tidied up */
}

/* "Exporting 2 of 5" while busy, else "". */
void exports_status (char *out, size_t n)
{
  if (!exports_busy ())
    {
      out[0] = '\0';
      return;
    }
  /* The photo being exported (or about to be); how far the whole queue
     has got is the status area's donut (exports_percent). */
  snprintf (out, n, "Exporting %d of %d", ijob + 1 < njobs ? ijob + 1 : njobs, njobs);
}

static int exists (const char *path)
{
  fsys_entry e;
  return fsys_stat (path, &e) == 0;
}

int exports_percent (void)
{
  if (!exports_busy () || njobs <= 0)
    return 0;
  return (ijob * 100 + (cur ? export_progress (cur) : 0)) / njobs;
}

/* Queues the box's photos for dst: one file, or (several) a directory of them. */
static int queue (const char *dst)
{
  int k, start;
  job *more;
  if (box_n <= 0)
    return -1;
  more = realloc (jobs, sizeof *jobs * (size_t) (njobs + box_n));
  if (!more)
    return -1;
  jobs = more;
  start = njobs;
  job_opts = opts;
  if (box_n == 1)
    {
      jobs[njobs].id = box_ids[0];
      snprintf (jobs[njobs].dst, sizeof jobs[njobs].dst, "%s", dst);
      njobs++;
    }
  else
    {
      if (fsys_mkdirs (dst))
        {
          char m[300];
          snprintf (m, sizeof m, "Lumen couldn't make the directory %s", dst);
          app_report (m);
          return -1;
        }
      for (k = 0; k < box_n; k++)
        {
          cat_details d;
          char leaf[200], path[1024];
          int j, n = 1, clash;
          if (cat_get_details (lib_cat (app_library ()), box_ids[k], &d))
            continue;
          {
            /* The name from the pattern ({name} by default). */
            char base[200];
            naming_info ni;
            leaf_from (fsys_leaf (d.path), base, sizeof base);
            ni.name = base;
            ni.n = k + 1;
            ni.taken = d.taken;
            ni.project = d.project_name;
            ni.camera = d.model;
            ni.rating = d.rating;
            naming_expand (names, &ni, leaf);
          }
          do
            {
              char l2[220];
              if (n == 1)
                snprintf (l2, sizeof l2, "%s", leaf);
              else
                snprintf (l2, sizeof l2, "%s_%d", leaf, n);
#ifndef __riscos__
              strncat (l2, opts.format == EXPORT_PNG ? ".png" : ".jpg", sizeof l2 - strlen (l2) - 1);
#endif
              fsys_join (path, sizeof path, dst, l2);
              clash = exists (path);
              for (j = start; j < njobs && !clash; j++)
                clash = strcmp (jobs[j].dst, path) == 0;
              n++;
            }
          while (clash && n < 1000);
          jobs[njobs].id = box_ids[k];
          snprintf (jobs[njobs].dst, sizeof jobs[njobs].dst, "%s", path);
          njobs++;
        }
    }
  return 0;
}

/* Where the time went (centiseconds): the raw plug-in, reading the
   photos, and adjusting, sharpening and writing; for the history. */
static unsigned spent[3], phase_t;
static int phase = -1;

static void phase_now (int p)
{
  unsigned t = app_now ();
  if (phase >= 0)
    spent[phase] += t - phase_t;
  phase = p;
  phase_t = t;
}

static void secs (char *out, size_t n, unsigned cs)
{
  if (cs < 950)
    snprintf (out, n, "%u.%u s", (cs + 5) / 100, (cs + 5) / 10 % 10);
  else
    snprintf (out, n, "%u s", (cs + 50) / 100);
}

static void all_done (void)
{
  char how[200] = "";
  unsigned total = spent[0] + spent[1] + spent[2];
  if (total >= 100)
    {
      char a[24], b[24], c[24], t[24];
      secs (t, sizeof t, total);
      secs (a, sizeof a, spent[0]);
      secs (b, sizeof b, spent[1]);
      secs (c, sizeof c, spent[2]);
      if (spent[0])
        snprintf (how, sizeof how, " in %s (LumenRaw %s, its picture read %s, adjusted and written %s)", t, a, b, c);
      else
        snprintf (how, sizeof how, " in %s (read %s, adjusted and written %s)", t, b, c);
    }
  spent[0] = spent[1] = spent[2] = 0;
  phase = -1;

  /* Into the Activity history. */
  if (njobs > 0)
    {
      char dir[1024];
      snprintf (dir, sizeof dir, "%s", jobs[0].dst);
      if (fsys_leaf (dir) > dir)
        ((char *) fsys_leaf (dir))[-1] = '\0';
      if (failed)
        ;                              /* app_report below puts it in the history */
      else if (reply_pending)
        activity_log (ACT_EXPORT, "Exported a photo to another program%s", how);
      else if (njobs == 1)
        activity_log (ACT_EXPORT, "Exported %s%s to %s", fsys_leaf (jobs[0].dst), how, dir);
      else
        activity_log (ACT_EXPORT, "Exported %d photos%s to %s", njobs, how, dir);
    }
  if (failed)
    {
      char m[400];
      if (njobs == 1)
        snprintf (m, sizeof m, "The photo couldn't be exported: %s", first_error);
      else
        snprintf (m, sizeof m, "%d of %d photos couldn't be exported. The first: %s", failed, njobs, first_error);
      app_report (m);
    }
  else if (reply_pending)
    {
      /* Scrap transfer: tell the receiver the file is ready. */
      reply_block[3] = reply_block[2];   /* your_ref = the ack's my_ref */
      reply_block[4] = Message_DataLoad;
      _swix (Wimp_SendMessage, _INR (0, 2), User_Message, reply_block, reply_task);
    }
  reply_pending = 0;
  free (jobs);
  jobs = NULL;
  njobs = ijob = failed = 0;
  first_error[0] = '\0';
}

static void job_failed (const char *why)
{
  if (!failed++)
    snprintf (first_error, sizeof first_error, "%s", why);
}

void exports_idle (unsigned deadline)
{
  while ((int) (app_now () - deadline) < 0)
    {
      int r;
      if (!cur)
        {
          cat_details d;
          char text[RECIPE_TEXT_MAX], err[200];
          recipe rc;
          if (ijob >= njobs)
            {
              if (njobs)
                all_done ();
              return;
            }
          if (cat_get_details (lib_cat (app_library ()), jobs[ijob].id, &d)
              || cat_get_recipe (lib_cat (app_library ()), jobs[ijob].id, text, sizeof text))
            {
              job_failed ("the photo is no longer in the library");
              ijob++;
              continue;
            }
          recipe_parse (&rc, text);
          cur = export_start (d.path, d.width, d.height, d.orientation, &rc, jobs[ijob].dst, &job_opts, err, sizeof err);
          if (cur)
            {
              /* For originals with no EXIF of their own (a PNG, a CR3...). */
              img_info known;
              memset (&known, 0, sizeof known);
              snprintf (known.make, sizeof known.make, "%s", d.make);
              snprintf (known.model, sizeof known.model, "%s", d.model);
              snprintf (known.taken, sizeof known.taken, "%s", d.taken);
              known.exposure = d.exposure;
              known.fnumber = d.fnumber;
              known.focal = d.focal;
              known.iso = d.iso;
              export_set_known (cur, &known);
              {
                char cap[CAT_CAPTION_MAX], kw[CAT_KEYWORDS_MAX];
                if (cat_get_tags (lib_cat (app_library ()), jobs[ijob].id, cap, sizeof cap, kw, sizeof kw) == 0)
                  export_set_tags (cur, cap, kw);
              }
            }
          if (!cur)
            {
              job_failed (err);
              ijob++;
              continue;
            }
        }
      if (phase != export_phase (cur))
        phase_now (export_phase (cur));
      r = export_step (cur, 16);
      if (phase != export_phase (cur))
        phase_now (export_phase (cur));
      if (r != 0)
        {
          phase_now (-1);
          if (r < 0)
            job_failed (export_error (cur));
          export_free (cur);
          cur = NULL;
          ijob++;
        }
    }
}

/* --- saving */
static int is_full_path (const char *s)
{
#ifdef __riscos__
  return strchr (s, '.') != NULL || strchr (s, ':') != NULL || strchr (s, '<') != NULL;
#else
  return strchr (s, '/') != NULL;
#endif
}

static void save_as_prefs (void);

static void read_options (void)
{
  int q = atoi (quality), w = atoi (box_w_text), h = atoi (box_h_text);
  opts.quality = q < 1 ? 1 : q > 100 ? 100 : q;
  w = w < 16 ? 16 : w;
  h = h < 16 ? 16 : h;
  snprintf (box_w_text, sizeof box_w_text, "%d", w);
  snprintf (box_h_text, sizeof box_h_text, "%d", h);
  opts.box_w = fit_on ? w : 0;
  opts.box_h = fit_on ? h : 0;
  if (fit_on)
    opts.max_side = 0;
}

/* Save to a full pathname (typed, or from the receiver). */
int exportbox_save_to (const char *path)
{
  read_options ();
  if (queue (path))
    return -1;
  save_as_prefs ();                    /* the next export (and Choices) start from these */
  close_box ();
  return 0;
}

static void save_clicked (void)
{
  if (!is_full_path (name))
    {
      app_report ("To save, drag the icon to a directory display.");
      return;
    }
  exportbox_save_to (name);
}

static void show_options (void);

void exportbox_click (int *b)
{
  int i = b[4];
  if (b[2] == Button_Menu)
    return;
  switch (i)
    {
    case I_JPEG: opts.format = EXPORT_JPEG; update_sprite (); break;
    case I_PNG: opts.format = EXPORT_PNG; update_sprite (); break;
    case I_FULL: case I_3840: case I_2048: case I_1024:
      opts.max_side = size_choice[i - I_FULL];
      fit_on = 0;
      break;
    case I_FIT: fit_on = 1; opts.max_side = 0; break;
    case I_BOX_W: case I_BOX_H:        /* typing a size chooses Fit in */
      if (!fit_on)
        {
          fit_on = 1;
          opts.max_side = 0;
          show_options ();
        }
      break;
    case I_SRGB: opts.colour = CS_SRGB; break;
    case I_ADOBE: opts.colour = CS_ADOBE; break;
    case I_P3: opts.colour = CS_P3; break;
    case I_SH_NONE: case I_SH_LOW: case I_SH_STD: case I_SH_HIGH: opts.out_sharpen = i - I_SH_NONE; break;
    case I_CANCEL: close_box (); break;
    case I_SAVE: save_clicked (); break;
    case I_FILE:
      if (b[2] == 64 || b[2] == 16)    /* a drag with Select or Adjust */
        {
          int st[9], db[10];
          st[0] = win;
          if (_swix (Wimp_GetWindowState, _IN (1), st))
            return;
          /* Wimp_DragBox type 5: a fixed size box (the icon), anywhere on screen. */
          db[0] = win;
          db[1] = 5;
          db[2] = st[1] + BOX_W / 2 - 60; db[3] = st[4] - 128;
          db[4] = st[1] + BOX_W / 2 + 60; db[5] = st[4] - 20;
          db[6] = -0x10000000; db[7] = -0x10000000; db[8] = 0x10000000; db[9] = 0x10000000;
          if (_swix (Wimp_DragBox, _IN (1), db) == NULL)
            dragging = 1;
        }
      break;
    }
}

int exportbox_key (int key)
{
  if (key == KEY_RETURN)
    {
      save_clicked ();
      return 1;
    }
  if (key == KEY_ESCAPE)
    {
      close_box ();
      return 1;
    }
  return 0;
}

/* The drag ended: offer the file to whatever is under the pointer. */
void exportbox_drag_end (void)
{
  int ptr[5], m[64];
  const char *leaf;
  size_t len;
  dragging = 0;
  if (_swix (Wimp_GetPointerInfo, _IN (1), ptr))
    return;
  if (ptr[3] == win)
    return;                            /* dropped back on the box */
  read_options ();
  leaf = fsys_leaf (name);
  len = strlen (leaf);
  if (len > 200)
    len = 200;
  memset (m, 0, sizeof m);
  m[0] = (int) ((44 + len + 1 + 3) & ~3u);
  m[3] = 0;
  m[4] = Message_DataSave;
  m[5] = ptr[3]; m[6] = ptr[4]; m[7] = ptr[0]; m[8] = ptr[1];
  m[9] = 1 << 20;                      /* an estimate */
  m[10] = box_n > 1 ? FT_DIR : opts.format == EXPORT_PNG ? FT_PNG : FT_JPEG;
  memcpy ((char *) &m[11], leaf, len);
  ((char *) &m[11])[len] = '\0';
  if (_swix (Wimp_SendMessage, _INR (0, 3), User_Message, m, ptr[3], ptr[4]) == NULL)
    save_ref = m[2];
}

/* Message_DataSaveAck: where to write. */
void exportbox_save_ack (int *m)
{
  const char *path = (const char *) &m[11];
  if (!save_ref || m[3] != save_ref)
    return;
  save_ref = 0;
  if (m[9] == -1)
    {
      if (box_n > 1)
        {
          app_report ("Several photos can only be exported to a directory.");
          return;
        }
      memcpy (reply_block, m, sizeof reply_block);
      reply_task = m[1];
      reply_pending = 1;
    }
  exportbox_save_to (path);
}

/* The box's radio buttons show opts (Wimp_SetIconState: bit 21, selected). */
static void show_options (void)
{
  static const int radios[] = { I_JPEG, I_PNG, I_FULL, I_3840, I_2048, I_1024, I_FIT, I_SRGB, I_ADOBE, I_P3,
                                I_SH_NONE, I_SH_LOW, I_SH_STD, I_SH_HIGH };
  int k;
  for (k = 0; k < (int) (sizeof radios / sizeof radios[0]); k++)
    {
      int i = radios[k], on, st[4];
      if (i == I_JPEG || i == I_PNG)
        on = opts.format == (i == I_JPEG ? EXPORT_JPEG : EXPORT_PNG);
      else if (i == I_FIT)
        on = fit_on;
      else if (i >= I_FULL && i <= I_1024)
        on = !fit_on && opts.max_side == size_choice[i - I_FULL];
      else if (i >= I_SRGB && i <= I_P3)
        on = opts.colour == (i == I_SRGB ? CS_SRGB : i == I_ADOBE ? CS_ADOBE : CS_P3);
      else
        on = opts.out_sharpen == i - I_SH_NONE;
      st[0] = win; st[1] = i;
      st[2] = on ? 1 << 21 : 0;
      st[3] = 1 << 21;
      _swix (Wimp_SetIconState, _IN (1), st);
    }
}

/* The Choices' starting settings (what was used last). */
void exportbox_use_prefs (const prefs *p)
{
  opts.format = p->export_format == EXPORT_PNG ? EXPORT_PNG : EXPORT_JPEG;
  fit_on = p->export_size == -1;
  opts.max_side = fit_on ? 0 : p->export_size;
  opts.quality = p->export_quality;
  opts.colour = p->export_colour == CS_ADOBE || p->export_colour == CS_P3 ? p->export_colour : CS_SRGB;
  opts.out_sharpen = p->export_sharpen >= 0 && p->export_sharpen <= 3 ? p->export_sharpen : 0;
  snprintf (quality, sizeof quality, "%d", p->export_quality);
  snprintf (box_w_text, sizeof box_w_text, "%d", p->export_box_w > 0 ? p->export_box_w : 1920);
  snprintf (box_h_text, sizeof box_h_text, "%d", p->export_box_h > 0 ? p->export_box_h : 1080);
  snprintf (names, sizeof names, "%s", p->export_names[0] ? p->export_names : "{name}");
  if (win != -1)
    {
      int k;
      show_options ();
      for (k = I_QUALITY; k <= I_QUALITY; k++)
        redraw_icon (k);
      redraw_icon (I_BOX_W);
      redraw_icon (I_BOX_H);
      redraw_icon (I_NAMES);
    }
  update_sprite ();
}

/* What was used goes into Choices, quietly (only if it changed). */
static void save_as_prefs (void)
{
  prefs p = *app_prefs ();
  p.export_format = opts.format;
  p.export_size = fit_on ? -1 : opts.max_side;
  p.export_quality = opts.quality;
  p.export_box_w = atoi (box_w_text);
  p.export_box_h = atoi (box_h_text);
  p.export_colour = opts.colour;
  p.export_sharpen = opts.out_sharpen;
  snprintf (p.export_names, sizeof p.export_names, "%s", names);
  if (memcmp (&p, app_prefs (), sizeof p))
    app_keep_prefs (&p);
}

/* For the tests: the name pattern and the box's size icons. */
void exportbox_set_names (const char *s)
{
  snprintf (names, sizeof names, "%s", s);
}

void exportbox_set_box (int w, int h)
{
  snprintf (box_w_text, sizeof box_w_text, "%d", w);
  snprintf (box_h_text, sizeof box_h_text, "%d", h);
}

/* For the tests: what the name icon holds. */
void exportbox_set_name (const char *s)
{
  snprintf (name, sizeof name, "%s", s);
}

const char *exportbox_name (void)
{
  return name;
}
