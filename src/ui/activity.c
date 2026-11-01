/* activity.c: what Lumen is doing, and what it has done.

   The status area in the main window's bottom bar shows the most
   important job running (importing, exporting, a raw photo being read,
   thumbnails, checking the originals) with a progress donut: a ring that
   fills as the job goes on, or a turning segment when how far it has got
   can't be known. Clicking it opens the Activity window: every job running
   now, each with its donut and percentage, and then a history of what has
   been done, newest first.

   The history is kept between sessions in <Choices$Write>.Lumen.Activity,
   one line per entry ("time kind text"), so it is a plain text file the
   user can read too. Lumen keeps the last ACT_KEEP entries.

   Jobs are seen starting and finishing by comparing what the library and
   the export queue say with what they said last time (activity_tick, after
   every event): the library itself needn't know about any of this. */
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <kernel.h>
#include <swis.h>
#include "ui.h"
#include "rawjob.h"
#include "fsys.h"

#define ACT_KEEP 200
#define ACT_TRIM 1000                  /* lines in the file before it is cut back to ACT_KEEP */

typedef struct
{
  long when;
  int kind;
  char text[200];
} entry;

static entry hist[ACT_KEEP];
static int nhist, first;               /* a ring: the oldest is hist[first] */
static char log_path[700];

/* What was seen last time. */
static int was_importing, was_thumbs, was_verifying, was_exporting;
static int thumbs_made, thumbs_failed_seen;
static unsigned import_t0, thumbs_t0, verify_t0;
static long last_watch_look;           /* when watched folders were last looked through (time()) */
static int last_watch_new;
static char shown[200];                /* the status area's text and donut, as last drawn */
static int shown_pct = -2, shown_spin = -1;

/* --- the history */
static const entry *nth (int k)        /* 0 = newest */
{
  return &hist[(first + nhist - 1 - k + ACT_KEEP) % ACT_KEEP];
}

static void add (long when, int kind, const char *text)
{
  entry *e;
  if (nhist < ACT_KEEP)
    e = &hist[(first + nhist++) % ACT_KEEP];
  else
    {
      e = &hist[first];
      first = (first + 1) % ACT_KEEP;
    }
  e->when = when;
  e->kind = kind;
  snprintf (e->text, sizeof e->text, "%s", text);
  /* One line each: no line ends inside. */
  {
    char *c;
    for (c = e->text; *c; c++)
      if (*c == '\n' || *c == '\r')
        *c = ' ';
  }
}

int activity_history_count (void) { return nhist; }

const char *activity_history (int k, long *when, int *kind)
{
  const entry *e;
  if (k < 0 || k >= nhist)
    return NULL;
  e = nth (k);
  if (when)
    *when = e->when;
  if (kind)
    *kind = e->kind;
  return e->text;
}

static void save_all (void)
{
  FILE *f;
  int k;
  if (!log_path[0] || !(f = fopen (log_path, "w")))
    return;
  for (k = nhist - 1; k >= 0; k--)
    fprintf (f, "%ld %d %s\n", nth (k)->when, nth (k)->kind, nth (k)->text);
  fclose (f);
  fsys_settype (log_path, 0xFFF);
}

void activity_log (int kind, const char *fmt, ...)
{
  char t[200];
  va_list ap;
  long now = (long) time (NULL);
  va_start (ap, fmt);
  vsnprintf (t, sizeof t, fmt, ap);
  va_end (ap);
  add (now, kind, t);
  if (log_path[0])
    {
      fsys_entry e;
      int is_new = fsys_stat (log_path, &e) != 0;
      FILE *f = fopen (log_path, "a");
      if (f)
        {
          fprintf (f, "%ld %d %s\n", now, kind, hist[(first + nhist - 1) % ACT_KEEP].text);
          fclose (f);
          if (is_new)
            fsys_settype (log_path, 0xFFF);
        }
    }
  activitywin_changed ();
}

void activity_clear (void)
{
  nhist = first = 0;
  save_all ();
  activitywin_changed ();
}

void activity_init (void)
{
  const char *v = getenv ("Lumen$ActivityFile");   /* the tests' own */
  char dir[600], line[300];
  FILE *f;
  int lines = 0;
  nhist = first = 0;
  if (v && *v)
    snprintf (log_path, sizeof log_path, "%s", v);
  else if (fsys_canonical ("<Choices$Write>.Lumen", dir, sizeof dir) == 0)
    {
      fsys_mkdirs (dir);
      fsys_join (log_path, sizeof log_path, dir, "Activity");
    }
  else
    log_path[0] = '\0';
  if (!log_path[0] || !(f = fopen (log_path, "r")))
    return;
  while (fgets (line, sizeof line, f))
    {
      char *end, *text;
      long when = strtol (line, &end, 10);
      int kind;
      size_t l = strlen (line);
      while (l && (line[l - 1] == '\n' || line[l - 1] == '\r'))
        line[--l] = '\0';
      if (end == line || *end != ' ')
        continue;
      kind = (int) strtol (end + 1, &text, 10);
      if (text == end + 1 || *text != ' ')
        continue;
      add (when, kind, text + 1);
      lines++;
    }
  fclose (f);
  if (lines > ACT_TRIM)
    save_all ();                       /* cut back to the last ACT_KEEP */
}

/* --- what is running */
void activity_thumb_made (void)
{
  thumbs_made++;
}

static void plural (char *out, size_t n, int count, const char *one, const char *many)
{
  snprintf (out, n, "%d %s", count, count == 1 ? one : many);
}

static void took (char *out, size_t n, unsigned cs)
{
  unsigned s = (cs + 50) / 100;
  if (s < 2)
    out[0] = '\0';
  else if (s < 120)
    snprintf (out, n, " in %u seconds", s);
  else
    snprintf (out, n, " in %u minutes", (s + 30) / 60);
}

int activity_now (act_job *jobs, int max)
{
  library *lib = app_library ();
  lib_progress p;
  int n = 0;
  if (!lib)
    return 0;
  lib_get_progress (lib, &p);
  if (p.importing && n < max)
    {
      if (p.watched_only)
        snprintf (jobs[n].text, sizeof jobs[n].text, "Looking through watched folders: %d new", p.added + p.relinked);
      else
        snprintf (jobs[n].text, sizeof jobs[n].text, "Importing %s: %d found, %d new", p.what, p.found, p.added);
      jobs[n++].pct = -1;
    }
  if (exports_busy () && n < max)
    {
      exports_status (jobs[n].text, sizeof jobs[n].text);
      jobs[n++].pct = exports_percent ();
    }
  if (rawjob_busy () && n < max)
    {
      snprintf (jobs[n].text, sizeof jobs[n].text, "Reading a raw photo with LumenRaw");
      jobs[n++].pct = -1;
    }
  if (p.thumbs_pending > 0 && n < max)
    {
      int done = thumbs_made, total = done + p.thumbs_pending;
      snprintf (jobs[n].text, sizeof jobs[n].text, "Making thumbnails: %d to go%s", p.thumbs_pending,
                app_has_focus () || p.importing ? "" : ", in spare time");
      jobs[n++].pct = total > 0 ? done * 100 / total : 0;
    }
  if (lib_verifying (lib) && n < max)
    {
      int checked, gone, back;
      lib_verify_counts (lib, &checked, &gone, &back);
      snprintf (jobs[n].text, sizeof jobs[n].text, "Checking the originals are still there: %d checked", checked);
      jobs[n++].pct = -1;
    }
  return n;
}

int activity_watch_info (char *out, size_t n)
{
  unsigned wake;
  struct tm *tm;
  time_t t;
  char at[16];
  if (!last_watch_look || !app_next_wake (&wake))
    return 0;
  t = (time_t) last_watch_look;
  tm = localtime (&t);
  strftime (at, sizeof at, "%H:%M", tm);
  if (last_watch_new)
    snprintf (out, n, "Watched folders: last looked through at %s, every %d minutes", at, app_prefs ()->watch_minutes);
  else
    snprintf (out, n, "Watched folders: nothing new at %s; Lumen looks every %d minutes", at, app_prefs ()->watch_minutes);
  return 1;
}

/* The text and donut for the status area. busy: something is running. */
void activity_status (char *text, size_t n, int *pct, int *busy)
{
  act_job j[6];
  int k = activity_now (j, 6);
  if (k)
    {
      if (k > 1)
        snprintf (text, n, "%s (and %d more)", j[0].text, k - 1);
      else
        snprintf (text, n, "%s", j[0].text);
      *pct = j[0].pct;
      *busy = 1;
      return;
    }
  *busy = 0;
  *pct = -1;
  /* Just finished: say what, for a minute. */
  if (nhist && (long) time (NULL) - nth (0)->when < 60)
    snprintf (text, n, "%s", nth (0)->text);
  else
    snprintf (text, n, "Activity");
}

/* The donut's turning segment: an eighth of a turn every 12 cs. */
int activity_spin (void)
{
  return (int) (app_now () / 12) % 8;
}

void activity_tick (void)
{
  library *lib = app_library ();
  lib_progress p;
  int verifying, exporting;
  if (!lib)
    return;
  lib_get_progress (lib, &p);
  verifying = lib_verifying (lib);
  exporting = exports_busy ();
  if (p.importing && !was_importing)
    import_t0 = app_now ();
  if (!p.importing && was_importing)
    {
      char t[40], a[40], b[60] = "", c[60] = "";
      took (t, sizeof t, app_now () - import_t0);
      if (p.watched_only)
        {
          last_watch_look = (long) time (NULL);
          last_watch_new = p.added + p.relinked;
          if (p.added || p.relinked)
            {
              plural (a, sizeof a, p.added, "new photo", "new photos");
              if (p.relinked)
                snprintf (b, sizeof b, ", %d found again", p.relinked);
              activity_log (ACT_WATCH, "Watched folders: %s%s", a, b);
            }
        }
      else if (p.found == 0 && p.relinked == 0)
        activity_log (ACT_PROBLEM, "Imported %s: no photos found in it", p.what);
      else
        {
          plural (a, sizeof a, p.added, "new photo", "new photos");
          if (p.skipped)
            snprintf (b, sizeof b, ", %d already in the library", p.skipped);
          if (p.relinked)
            snprintf (c, sizeof c, ", %d found again", p.relinked);
          activity_log (ACT_IMPORT, "Imported %s: %s%s%s%s", p.what, a, b, c, t);
        }
    }
  if (p.thumbs_pending > 0 && !was_thumbs)
    {
      thumbs_made = 0;
      thumbs_t0 = app_now ();
    }
  if (p.thumbs_pending <= 0 && was_thumbs)
    {
      if (thumbs_made > 0)
        {
          char a[40], t[40];
          plural (a, sizeof a, thumbs_made, "thumbnail", "thumbnails");
          took (t, sizeof t, app_now () - thumbs_t0);
          activity_log (ACT_THUMBS, "Made %s%s", a, t);
        }
      thumbs_made = 0;
    }
  if (verifying && !was_verifying)
    verify_t0 = app_now ();
  if (!verifying && was_verifying)
    {
      int checked, gone, back;
      lib_verify_counts (lib, &checked, &gone, &back);
      if (gone || back)
        activity_log (gone ? ACT_PROBLEM : ACT_CHECK, "Checked %d originals: %d missing, %d back again", checked, gone,
                      back);
      else if (checked)
        activity_log (ACT_CHECK, "Checked %d originals: all there", checked);
    }
  was_importing = p.importing;
  was_thumbs = p.thumbs_pending > 0;
  was_verifying = verifying;
  was_exporting = exporting;
  (void) thumbs_failed_seen;
  /* Redraw the status area when what it shows has changed. */
  {
    char t[200];
    int pct, busy, spin;
    activity_status (t, sizeof t, &pct, &busy);
    spin = busy && pct < 0 ? activity_spin () : -1;
    if (strcmp (t, shown) || pct != shown_pct || spin != shown_spin)
      {
        snprintf (shown, sizeof shown, "%s", t);
        shown_pct = pct;
        shown_spin = spin;
        mainwin_status_changed ();
        activitywin_now_changed ();
      }
  }
}

int activity_busy (void)
{
  act_job j[1];
  return activity_now (j, 1) > 0;
}

/* --- the Activity window
   A window we draw: the jobs running now, then the history, newest first.
   Flags (PRM p734's neighbour, new format): moveable (1); back, close,
   title, toggle size, vertical scroll bar, adjust size (bits 24-29). */
#define AW_W 1200
#define AW_H 700
#define ROW_NOW 64
#define ROW_HIST 44
#define HEAD 56
#define COL_BG 0xFFFFFF00u
#define COL_TEXT 0x1A1A1A00u
#define COL_QUIET 0x70707000u
#define COL_HEAD 0x99440000u
#define COL_RULE 0xDDDDDD00u
#define COL_TRACK 0xDDDDDD00u
#define COL_DONUT 0xCC770000u          /* #0077CC */
#define COL_BAD 0x2020C000u            /* #C02020 */
#define COL_GOOD 0x40A04000u           /* #40A040 */

static int awin = -1, aw_open, njobs_drawn = -1;
static char aw_title[] = "Activity";

int activitywin_window (void) { return awin; }

int activitywin_create (void)
{
  int b[23];
  memset (b, 0, sizeof b);
  b[0] = 0; b[1] = 0; b[2] = AW_W; b[3] = AW_H;
  b[6] = -1;
  b[7] = (int) 0xBF000002u;
  ((unsigned char *) b)[32] = 7;  ((unsigned char *) b)[33] = 2;
  ((unsigned char *) b)[34] = 7;  ((unsigned char *) b)[35] = 0;
  ((unsigned char *) b)[36] = 3;  ((unsigned char *) b)[37] = 1;
  ((unsigned char *) b)[38] = 12;
  b[10] = 0; b[11] = -AW_H; b[12] = AW_W * 2; b[13] = 0;
  b[14] = 0x13D;                       /* text, border, centred, filled, indirected */
  b[15] = 3 << 12;                     /* work area: click */
  b[16] = 1;
  b[18] = (int) aw_title; b[19] = -1; b[20] = sizeof aw_title;
  return _swix (Wimp_CreateWindow, _IN (1) | _OUT (0), b, &awin) ? -1 : 0;
}

int activitywin_is_open (void) { return aw_open; }

/* Where things are, in work area coordinates (y down from 0). */
static int now_rows (void)
{
  act_job j[6];
  int n = activity_now (j, 6);
  return n ? n : 1;
}

static int now_has_watch (void)
{
  char t[160];
  return activity_watch_info (t, sizeof t);
}

static int hist_top (void)
{
  return -(HEAD + now_rows () * ROW_NOW + (now_has_watch () ? ROW_HIST : 0) + 24 + HEAD);
}

static void clear_rect (int r[4], int vis_w)
{
  r[2] = vis_w - 24;
  r[0] = r[2] - 220;
  r[3] = hist_top () + HEAD - 8;
  r[1] = r[3] - 44;
}

static void set_extent (void)
{
  int e[4], h = -hist_top () + (nhist ? nhist : 1) * ROW_HIST + 24;
  if (awin == -1)
    return;
  e[0] = 0; e[1] = -(h > AW_H ? h : AW_H); e[2] = AW_W * 2; e[3] = 0;
  _swix (Wimp_SetExtent, _INR (0, 1), awin, e);
}

void activitywin_open (void)
{
  int s[9], xe = 1, ye = 1, sw = 0, sh = 0;
  if (awin == -1)
    return;
  set_extent ();
  _swix (OS_ReadModeVariable, _INR (0, 1) | _OUT (2), -1, 4, &xe);
  _swix (OS_ReadModeVariable, _INR (0, 1) | _OUT (2), -1, 5, &ye);
  _swix (OS_ReadModeVariable, _INR (0, 1) | _OUT (2), -1, 11, &sw);
  _swix (OS_ReadModeVariable, _INR (0, 1) | _OUT (2), -1, 12, &sh);
  s[0] = awin;
  if (_swix (Wimp_GetWindowState, _IN (1), s))
    return;
  if (!(s[8] & (1 << 16)))
    {
      int cx = ((sw + 1) << xe) / 2, cy = ((sh + 1) << ye) / 2;
      s[1] = cx - AW_W / 2; s[2] = cy - AW_H / 2; s[3] = cx + AW_W / 2; s[4] = cy + AW_H / 2;
      s[5] = 0; s[6] = 0;
    }
  s[7] = -1;
  _swix (Wimp_OpenWindow, _IN (1), s);
  aw_open = 1;
}

void activitywin_closed (void)
{
  aw_open = 0;
}

/* The whole window: the history has changed (or the jobs' layout). */
void activitywin_changed (void)
{
  if (awin == -1 || !aw_open)
    return;
  set_extent ();
  _swix (Wimp_ForceRedraw, _INR (0, 4), awin, 0, -0x10000000, 0x10000000, 0);
}

/* Only the jobs running: their text and donuts. */
void activitywin_now_changed (void)
{
  act_job j[6];
  int n;
  if (awin == -1 || !aw_open)
    return;
  n = activity_now (j, 6);
  if (n != njobs_drawn)
    {
      njobs_drawn = n;
      activitywin_changed ();          /* the history moves */
      return;
    }
  _swix (Wimp_ForceRedraw, _INR (0, 4), awin, 0, -(HEAD + now_rows () * ROW_NOW), 0x10000000, -HEAD);
}

/* Text cut to fit width, with "..." if it had to be. */
void activity_fit (const char *s, int width, char *out, size_t n)
{
  size_t l;
  snprintf (out, n, "%s", s);
  if (draw_text_width (out) <= width)
    return;
  l = strlen (out);
  while (l > 0)
    {
      out[--l] = '\0';
      if (l + 4 < n)
        {
          char t[260];
          snprintf (t, sizeof t, "%s...", out);
          if (draw_text_width (t) <= width)
            {
              snprintf (out, n, "%s", t);
              return;
            }
        }
    }
}

static void when_text (long when, char *out, size_t n)
{
  time_t t = (time_t) when, now = time (NULL);
  struct tm a, b;
  a = *localtime (&t);
  b = *localtime (&now);
  if (a.tm_year == b.tm_year && a.tm_yday == b.tm_yday)
    strftime (out, n, "%H:%M", &a);
  else if (a.tm_year == b.tm_year)
    strftime (out, n, "%d %b %H:%M", &a);
  else
    strftime (out, n, "%d %b %Y", &a);
}

static unsigned kind_colour (int kind)
{
  switch (kind)
    {
    case ACT_PROBLEM: return COL_BAD;
    case ACT_EXPORT: return COL_GOOD;
    case ACT_IMPORT:
    case ACT_WATCH: return COL_DONUT;
    default: return COL_QUIET;
    }
}

static void draw_heading (int ox, int y, const char *t, int vis_w)
{
  draw_text (ox + 24, y - 40, t, COL_HEAD, COL_BG);
  draw_rect (ox + 24, y - 52, ox + vis_w - 24, y - 50, COL_RULE);
}

void activitywin_redraw (int *b)
{
  int more = 0;
  _swix (Wimp_RedrawWindow, _IN (1) | _OUT (0), b, &more);
  while (more)
    {
      int ox = b[1] - b[5], oy = b[4] - b[6], vis_w = b[3] - b[1], k, y;
      act_job j[6];
      int n = activity_now (j, 6);
      char t[260], line[160];
      njobs_drawn = n;
      draw_rect (b[7], b[8], b[9], b[10], COL_BG);
      draw_heading (ox, oy, "Now", vis_w);
      y = oy - HEAD;
      if (!n)
        {
          draw_text (ox + 24, y - 42, "Nothing running.", COL_QUIET, COL_BG);
          y -= ROW_NOW;
        }
      for (k = 0; k < n; k++, y -= ROW_NOW)
        {
          int cx = ox + 24 + 22, cy = y - ROW_NOW / 2, room = vis_w - 80 - 24 - 120;
          draw_donut (cx, cy, 22, 12, j[k].pct, j[k].pct < 0 ? activity_spin () : 0, COL_DONUT, COL_TRACK, COL_BG);
          activity_fit (j[k].text, room, t, sizeof t);
          draw_text (ox + 80, cy - 10, t, COL_TEXT, COL_BG);
          if (j[k].pct >= 0)
            {
              snprintf (line, sizeof line, "%d%%", j[k].pct);
              draw_text (ox + vis_w - 24 - draw_text_width (line), cy - 10, line, COL_TEXT, COL_BG);
            }
        }
      if (activity_watch_info (line, sizeof line))
        {
          activity_fit (line, vis_w - 48, t, sizeof t);
          draw_text (ox + 24, y - 32, t, COL_QUIET, COL_BG);
          y -= ROW_HIST;
        }
      y -= 24;
      draw_heading (ox, y, "Earlier", vis_w);
      if (nhist)
        {
          int r[4];
          clear_rect (r, vis_w);
          draw_rect (ox + r[0], oy + r[1], ox + r[2], oy + r[3], 0xEEEEEE00u);
          draw_frame (ox + r[0], oy + r[1], ox + r[2], oy + r[3], 0xAAAAAA00u);
          draw_text (ox + (r[0] + r[2] - draw_text_width ("Clear history")) / 2, oy + r[1] + 10, "Clear history",
                     COL_TEXT, 0xEEEEEE00u);
        }
      y -= HEAD;
      if (!nhist)
        draw_text (ox + 24, y - 32, "Nothing yet.", COL_QUIET, COL_BG);
      for (k = 0; k < nhist; k++, y -= ROW_HIST)
        {
          const entry *e = nth (k);
          if (y < b[8] - ROW_HIST)
            break;                     /* below what is being redrawn */
          if (y - ROW_HIST > b[10])
            continue;                  /* above it */
          when_text (e->when, line, sizeof line);
          draw_text (ox + 24, y - 32, line, COL_QUIET, COL_BG);
          draw_rect (ox + 250, y - 28, ox + 262, y - 16, kind_colour (e->kind));
          activity_fit (e->text, vis_w - 290 - 24, t, sizeof t);
          draw_text (ox + 290, y - 32, t, e->kind == ACT_PROBLEM ? COL_BAD : COL_TEXT, COL_BG);
        }
      _swix (Wimp_GetRectangle, _IN (1) | _OUT (0), b, &more);
    }
}

void activitywin_click (int *b)
{
  int st[9], wx, wy, r[4];
  st[0] = awin;
  if (b[2] == Button_Menu || _swix (Wimp_GetWindowState, _IN (1), st))
    return;
  wx = b[0] - (st[1] - st[5]);
  wy = b[1] - (st[4] - st[6]);
  clear_rect (r, st[3] - st[1]);
  if (nhist && wx >= r[0] && wx <= r[2] && wy >= r[1] && wy <= r[3])
    activity_clear ();
}

/* For interactive help: 1 over the Clear history button. */
int activitywin_part_at (int x, int y)
{
  int st[9], r[4];
  st[0] = awin;
  if (_swix (Wimp_GetWindowState, _IN (1), st))
    return 0;
  clear_rect (r, st[3] - st[1]);
  x -= st[1] - st[5];
  y -= st[4] - st[6];
  return nhist && x >= r[0] && x <= r[2] && y >= r[1] && y <= r[3];
}
