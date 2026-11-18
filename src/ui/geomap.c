/* Copyright (c) 2026 Andrew Youll
   Released under the MIT licence: see the LICENSE file.
   SPDX-License-Identifier: MIT */
/* geomap.c: photos on maps, by the GeoData messages (core/geodata.h),
   with RiscOSM (Sine Nomine) in mind.

   - The library's photos with a place are a dataset (DATASET) map
     programs can search: announced with DatasetDetails at start and when
     the area they cover changes (withdrawn when there are none, or
     Choices says not to), and described again on DescribeDatasets.
   - FetchData (a box, or a point and radius, words, pages) is answered
     with Status (searching), a TransmitData for each photo found, and
     Status (found, or none).
   - Show on map sends TransmitData for the photos chosen, asking for a
     map (starting RiscOSM if it isn't running and the Filer has seen it).
   - FetchFile is answered with DataLoad: a JPEG original as it is, any
     other photo (or an adjusted one) as a JPEG made for the map (up to
     MAP_SIDE pixels, the adjustments applied, its EXIF and place kept),
     in <Wimp$ScrapDir>.LumenMap until Lumen quits.
   - DisplayInfo (a pin Ctrl-clicked) opens that photo in the Viewer.
   The object id is the version's id; the replies to a search are sent a
   few at each poll so the Wimp's message queue isn't swamped. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <kernel.h>
#include <swis.h>
#include "geodata.h"
#include "export.h"
#include "fsys.h"
#include "prefs.h"
#include "recipe.h"
#include "ui.h"

#define DATASET 1                      /* the library: one dataset */
#define PER_POLL 12                    /* messages sent each poll */
#define MAP_SIDE 1024                  /* the JPEGs made for maps, longer side */
#define MAP_DIR "<Wimp$ScrapDir>.LumenMap"
#define FT_JPEG_TYPE 0xC85
#ifndef Wimp_TransferBlock
#define Wimp_TransferBlock 0x400F1
#endif
#define Msg_DataLoad 3

/* --- messages waiting to go: batches of photos, each with a status to end */
typedef struct batch
{
  struct batch *next;
  int task, your_ref, map, flags;
  cat_geo_hit *hits;
  int n, at;
  int status;                          /* send Status when done (a reply to FetchData) */
  int total;
  /* Show on map: waiting for the map to open (MapInfo, in reply to our
     OpenMap), to put the pins on it; until then, until it bounces or the
     time is up (then the first photo asks for a map itself). */
  int opening, open_ref;
  unsigned open_until;
} batch;
static batch *queue_head, *queue_tail;

/* Show on map waiting for RiscOSM to start. */
static cat_id *waiting;
static int nwaiting;
static unsigned wait_until, wait_seen;
static int map_task;                   /* RiscOSM's task handle, when last looked for (0: not running) */

/* What was announced (to tell when to again). */
static int announced, ann_n, ann_box[4];
static unsigned last_check;

/* JPEGs being made for maps (FetchFile): one at a time. */
typedef struct make
{
  struct make *next;
  cat_id version;
  int task, your_ref;
} make;
static make *makes;
static exporter *cur;
static char cur_dst[300];

static void send (int code, int *b, int to)
{
  if (_swix (Wimp_SendMessage, _INR (0, 2), code, b, to) && to == map_task)
    map_task = 0;                      /* it has gone */
}

static void free_batch (batch *b)
{
  free (b->hits);
  free (b);
}

static void enqueue (batch *b)
{
  b->next = NULL;
  if (queue_tail)
    queue_tail->next = b;
  else
    queue_head = b;
  queue_tail = b;
}

/* --- announcing the library */

static void announce (int force)
{
  int b[GEODATA_WORDS], n = 0, x0 = 0, y0 = 0, x1 = 0, y1 = 0;
  library *l = app_library ();
  if (l && app_prefs ()->map_share)
    n = cat_geo_bounds (lib_cat (l), &x0, &y0, &x1, &y1);
  if (n < 0)
    return;
  if (!force && ((n == 0 && !announced) || (n > 0 && announced && n == ann_n && x0 == ann_box[0] && y0 == ann_box[1]
                                            && x1 == ann_box[2] && y1 == ann_box[3])))
    return;
  if (n > 0)
    {
      char desc[160];
      snprintf (desc, sizeof desc, "%d photo%s with a place, in the Lumen library %s", n, n == 1 ? "" : "s",
                l ? lib_root (l) : "");
      geodata_details (b, DATASET, n, x0, y0, x1, y1, "Lumen photos", desc);
    }
  else
    geodata_details (b, DATASET, 0, 0, 0, 0, 0, "", "");
  b[3] = 0;
  send (17, b, 0);                     /* a broadcast */
  announced = n > 0;
  ann_n = n;
  ann_box[0] = x0; ann_box[1] = y0; ann_box[2] = x1; ann_box[3] = y1;
}

void geomap_start (void)
{
  last_check = app_now ();
  announce (0);
}

void geomap_library_changed (void)
{
  announce (0);
}

static int made_files;                  /* files were made in MAP_DIR */

static void tidy_files (void)
{
  char dir[300];
  if (made_files && fsys_canonical (MAP_DIR, dir, sizeof dir) == 0)
    fsys_remove_tree (dir);
  made_files = 0;
}

void geomap_stop (void)
{
  if (announced)
    {
      int b[GEODATA_WORDS];
      geodata_details (b, DATASET, 0, 0, 0, 0, 0, "", "");
      b[3] = 0;
      send (17, b, 0);
      announced = 0;
    }
  if (cur)
    export_free (cur);
  cur = NULL;
  tidy_files ();
}

/* --- answering */

static void describe (int *m)
{
  int b[GEODATA_WORDS], n, x0 = 0, y0 = 0, x1 = 0, y1 = 0, types = m[10] & GD_TYPES, inter = 0;
  library *l = app_library ();
  if (!l || !app_prefs ()->map_share)
    return;
  if ((m[5] & 2) && m[11] != DATASET)
    return;                            /* another dataset asked for */
  if (types && types != GD_TYPES && !(types & GD_PHOTOS))
    return;
  n = cat_geo_bounds (lib_cat (l), &x0, &y0, &x1, &y1);
  if (n <= 0)
    return;
  if (m[6] || m[7] || m[8] || m[9])
    {
      /* An area of interest: do our photos' box and it overlap? */
      inter = !(x1 < m[6] || x0 > m[8] || y1 < m[7] || y0 > m[9]);
      if (!inter && !(m[5] & 1))
        return;
    }
  {
    char desc[160];
    snprintf (desc, sizeof desc, "%d photo%s with a place, in the Lumen library %s", n, n == 1 ? "" : "s", lib_root (l));
    geodata_details (b, DATASET, n, x0, y0, x1, y1, "Lumen photos", desc);
  }
  if (inter)
    b[11] |= GD_INTERSECTS;
  b[3] = m[2];
  send (17, b, m[1]);
}

static void status (int code, int quantity, const char *msg, int task, int your_ref)
{
  int b[GEODATA_WORDS];
  geodata_status (b, code, quantity, msg);
  b[3] = your_ref;
  send (17, b, task);
}

static void fetch_data (int *m)
{
  geodata_fetch f;
  cat_geo_query q;
  char words[128] = "";
  batch *b;
  int total = 0;
  library *l = app_library ();
  geodata_read_fetch (m, &f);
  if (!l || !app_prefs ()->map_share || f.dataset != DATASET || (f.flags & GD_CANCEL_UPDATES))
    return;
  /* The search words are in the sender's memory. */
  if ((f.what & GD_TEXT) && f.text_len > 0)
    {
      int len = f.text_len < (int) sizeof words - 1 ? f.text_len : (int) sizeof words - 1;
      if (_swix (Wimp_TransferBlock, _INR (0, 4), m[1], f.text_at, app_task (), words, len))
        len = 0;
      words[len] = '\0';
    }
  status (GDS_SEARCHING, 0, "Searching the Lumen library", m[1], m[2]);
  if (geodata_query (&f, words, &q))
    {
      status (GDS_NO_RESULTS, 0, "No photos", m[1], m[2]);
      return;
    }
  b = calloc (1, sizeof *b);
  if (!b || cat_geo_find (lib_cat (l), &q, &b->hits, &b->n, &total))
    {
      free (b);
      status (GDS_FAILED, 0, "Lumen couldn't search its library", m[1], m[2]);
      return;
    }
  if (b->n == 0)
    {
      free_batch (b);
      status (GDS_NO_RESULTS, 0, "No photos found in Lumen", m[1], m[2]);
      return;
    }
  b->task = m[1];
  b->your_ref = m[2];
  b->map = f.map;
  b->flags = GD_FILE | GD_MORE_INFO;
  b->status = 1;
  b->total = total;
  enqueue (b);
}

/* A file for the map: the original if the map program can use it as it
   is, else a JPEG made for it. */
static void data_load (int task, int your_ref, const char *path)
{
  int b[GEODATA_WORDS];
  fsys_entry e;
  memset (b, 0, sizeof b);
  b[3] = your_ref;
  b[4] = Msg_DataLoad;
  b[9] = fsys_stat (path, &e) == 0 ? (int) e.size : 0;
  b[10] = FT_JPEG_TYPE;
  snprintf ((char *) &b[11], sizeof b - 44, "%s", path);
  b[0] = (int) ((44 + strlen ((char *) &b[11]) + 1 + 3) & ~3u);
  send (18, b, task);
}

static void fetch_file (int *m)
{
  cat_geo_query q;
  cat_geo_hit *h = NULL;
  int n = 0, k, jpeg = 0, any = 0;
  library *l = app_library ();
  if (!l || m[6] != DATASET || m[7] <= 0)
    return;
  for (k = 8; k < GEODATA_WORDS && m[k] != -1 && 4 * k < m[0]; k++)
    {
      any = 1;
      if (m[k] == FT_JPEG_TYPE)
        jpeg = 1;
    }
  if (any && !jpeg)
    return;                            /* it can't take a JPEG */
  memset (&q, 0, sizeof q);
  q.version = m[7];
  if (cat_geo_find (lib_cat (l), &q, &h, &n, NULL) || n < 1)
    {
      free (h);
      return;
    }
  if (h->format == IMG_JPEG && !h->edited)
    data_load (m[1], m[2], h->path);
  else
    {
      make *mk = calloc (1, sizeof *mk), **at = &makes;
      if (mk)
        {
          mk->version = m[7];
          mk->task = m[1];
          mk->your_ref = m[2];
          while (*at)
            at = &(*at)->next;
          *at = mk;
        }
    }
  free (h);
}

/* The map RiscOSM opened for Show on map: the pins go on it. */
static void map_info (int *m)
{
  batch *b;
  for (b = queue_head; b; b = b->next)
    if (b->opening && m[3] == b->open_ref)
      {
        b->opening = 0;
        b->map = m[5];
      }
}

void geomap_bounced (int *m)
{
  batch *b;
  if (m[4] != GeoData_OpenMap)
    return;
  /* Nothing opened a map: the first photo asks for one itself. */
  for (b = queue_head; b; b = b->next)
    if (b->opening && m[2] == b->open_ref)
      {
        b->opening = 0;
        b->flags |= GD_DRAW_MAP;
      }
}

int geomap_message (int *m)
{
  switch (m[4])
    {
    case GeoData_MapInfo:
      map_info (m);
      return 1;
    case GeoData_DescribeDatasets:
      describe (m);
      return 1;
    case GeoData_FetchData:
      fetch_data (m);
      return 1;
    case GeoData_FetchFile:
      fetch_file (m);
      return 1;
    case GeoData_DisplayInfo:
      if (m[6] == DATASET && m[7] > 0)
        {
          if (app_show_version (m[7]))
            app_report ("That photo is no longer in Lumen's library.");
        }
      return 1;
    }
  return 0;
}

/* --- Show on map */

/* RiscOSM, by its name in the Task Manager (looked for each time: it
   may have quit, or been started, since). */
static int find_map (void)
{
  return map_task = app_find_task ("RiscOSM");
}

static void show_now (const cat_id *v, int n, const char *title)
{
  batch *b = calloc (1, sizeof *b);
  int k, none = 0;
  library *l = app_library ();
  if (!b || !l || !(b->hits = malloc (sizeof *b->hits * (size_t) n)))
    {
      free (b);
      return;
    }
  for (k = 0; k < n; k++)
    {
      cat_geo_query q;
      cat_geo_hit *h = NULL;
      int got = 0;
      memset (&q, 0, sizeof q);
      q.version = v[k];
      if (cat_geo_find (lib_cat (l), &q, &h, &got, NULL) == 0 && got == 1)
        b->hits[b->n++] = h[0];
      else
        none++;
      free (h);
    }
  if (b->n == 0)
    {
      free_batch (b);
      app_report (n == 1 ? "This photo has no location: its camera didn't record where it was taken."
                         : "None of these photos has a location: their cameras didn't record where they were taken.");
      return;
    }
  b->task = map_task;
  b->flags = GD_FILE | GD_MORE_INFO;
  /* A map framing them all first (or centred on the one); the pins go
     on it when RiscOSM says which map it is. */
  {
    int m[GEODATA_WORDS];
    char t[120];
    if (title && title[0])
      snprintf (t, sizeof t, "%s", title);
    else if (b->n == 1)
      snprintf (t, sizeof t, "%s", b->hits[0].name);
    else
      snprintf (t, sizeof t, "%d photos from Lumen", b->n);
    geodata_open_map (m, b->hits, b->n, t);
    m[3] = 0;
    if (_swix (Wimp_SendMessage, _INR (0, 2), 18, m, b->task) == NULL)
      {
        b->opening = 1;
        b->open_ref = m[2];
        b->open_until = app_now () + 500;
      }
    else
      b->flags |= GD_DRAW_MAP;
  }
  enqueue (b);
  if (none)
    {
      char msg[160];
      snprintf (msg, sizeof msg, "%d of the %d photos %s no location, so %s shown.", none, n,
                none == 1 ? "has" : "have", none == 1 ? "it isn't" : "they aren't");
      app_inform (msg);
    }
}

void geomap_show (const cat_id *v, int n)
{
  geomap_show_named (v, n, NULL);
}

static char wait_title[120];

void geomap_show_named (const cat_id *v, int n, const char *title)
{
  char dir[256];
  if (n <= 0)
    return;
  if (find_map ())
    {
      show_now (v, n, title);
      return;
    }
  /* Not running: start it if the Filer has seen it, and send when it's up. */
  int len = 0;
  if (!_swix (OS_ReadVarVal, _INR (0, 4) | _OUT (2), "RiscOSM$Dir", dir, (int) sizeof dir - 1, 0, 3, &len) && len > 0)
    {
      free (waiting);
      waiting = malloc (sizeof *waiting * (size_t) n);
      if (!waiting)
        return;
      memcpy (waiting, v, sizeof *waiting * (size_t) n);
      nwaiting = n;
      snprintf (wait_title, sizeof wait_title, "%s", title ? title : "");
      wait_until = app_now () + 3000;
      wait_seen = 0;
      /* Filer_Run doesn't expand <RiscOSM$Dir> (the Filer opened a
         directory of that name): the path itself. */
      {
        char cmd[300];
        dir[len < (int) sizeof dir ? len : (int) sizeof dir - 1] = '\0';
        snprintf (cmd, sizeof cmd, "Filer_Run %s", dir);
        _swix (Wimp_StartTask, _IN (0), cmd);
      }
      return;
    }
  app_report ("Lumen shows photos on a map with RiscOSM, which isn't running. Start RiscOSM, then choose "
              "Show on map again.");
}

/* --- the poll */

int geomap_busy (void)
{
  return queue_head || nwaiting || makes || cur;
}

static void make_step (unsigned deadline)
{
  int first = 1;
  /* At least one step each time, however short the slice. */
  while (first || (int) (app_now () - deadline) < 0)
    {
      first = 0;
      if (!cur)
        {
          make *mk = makes;
          cat_details d;
          char text[RECIPE_TEXT_MAX], err[200], dir[260];
          recipe rc;
          export_opts o;
          if (!mk)
            return;
          if (cat_get_details (lib_cat (app_library ()), mk->version, &d)
              || cat_get_recipe (lib_cat (app_library ()), mk->version, text, sizeof text)
              || fsys_canonical (MAP_DIR, dir, sizeof dir) || fsys_mkdirs (dir))
            {
              makes = mk->next;
              free (mk);
              continue;
            }
          {
            char leaf[32];
            snprintf (leaf, sizeof leaf, "M%lld", (long long) mk->version);
            fsys_join (cur_dst, sizeof cur_dst, dir, leaf);
          }
          made_files = 1;
          recipe_parse (&rc, text);
          memset (&o, 0, sizeof o);
          o.format = EXPORT_JPEG;
          o.quality = 85;
          o.max_side = MAP_SIDE;
          o.out_sharpen = EXPORT_SHARP_LOW;
          snprintf (o.software, sizeof o.software, "Lumen");
          cur = export_start (d.path, d.width, d.height, d.orientation, &rc, cur_dst, &o, err, sizeof err);
          if (!cur)
            {
              makes = mk->next;
              free (mk);
              continue;
            }
        }
      {
        int r = export_step (cur, 32);
        if (r != 0)
          {
            make *mk = makes;
            export_free (cur);
            cur = NULL;
            if (mk)
              {
                if (r > 0)
                  data_load (mk->task, mk->your_ref, cur_dst);
                makes = mk->next;
                free (mk);
              }
          }
      }
    }
}

void geomap_idle (unsigned now, unsigned deadline)
{
  int sent = 0;
  /* Waiting for RiscOSM to start: send once it has been up a moment. */
  if (nwaiting)
    {
      if (!wait_seen && find_map ())
        wait_seen = now;
      if (wait_seen && (int) (now - wait_seen) > 200)
        {
          show_now (waiting, nwaiting, wait_title);
          free (waiting);
          waiting = NULL;
          nwaiting = 0;
        }
      else if ((int) (now - wait_until) > 0)
        {
          free (waiting);
          waiting = NULL;
          nwaiting = 0;
          app_report ("RiscOSM didn't start, so the photos couldn't be shown on a map.");
        }
    }
  while (queue_head && sent < PER_POLL)
    {
      batch *b = queue_head;
      if (b->opening)
        {
          if ((int) (now - b->open_until) < 0)
            break;                     /* still waiting for the map */
          b->opening = 0;
          b->flags |= GD_DRAW_MAP;     /* no answer: the first photo asks for a map */
        }
      if (b->at < b->n)
        {
          int m[GEODATA_WORDS];
          if (!b->task)
            b->task = find_map ();
          if (!b->task)
            b->at = b->n;              /* gone: drop them */
          else
            {
              geodata_transmit (m, b->map, DATASET, &b->hits[b->at], b->at == 0 ? b->flags : b->flags & ~GD_DRAW_MAP);
              m[3] = b->your_ref;
              send (17, m, b->task);
              b->at++;
              sent++;
            }
          continue;
        }
      if (b->status && b->task)
        {
          char msg[80];
          snprintf (msg, sizeof msg, "%d photo%s found in Lumen", b->total, b->total == 1 ? "" : "s");
          status (GDS_FOUND, b->total, msg, b->task, b->your_ref);
        }
      queue_head = b->next;
      if (!queue_head)
        queue_tail = NULL;
      free_batch (b);
    }
  if (makes || cur)
    make_step (deadline);
  /* The area the photos cover may have changed (imports, places read). */
  if ((int) (now - last_check) > 1000)
    {
      last_check = now;
      announce (0);
    }
}
