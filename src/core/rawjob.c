/* Copyright (c) 2026 Andrew Youll
   Released under the MIT licence: see the LICENSE file.
   SPDX-License-Identifier: MIT */
/* rawjob.c: see rawjob.h. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "fsys.h"
#include "rawjob.h"

enum { J_QUEUED, J_RUNNING, J_ENDED, J_DONE, J_FAILED };

static int raw_noise;                  /* for raw jobs started from now on (rawjob_set_raw_noise) */

struct rawjob
{
  int state, tag, kind, noise;
  char src[1024], image[300], details[300], err[300];
  img_info info;
  rawjob *next;                        /* in the queue */
};

/* The plug-ins, one for each kind of file (RAWJOB_RAW, then format
   plug-ins); their jobs share one queue, one at a time (each can need a
   lot of memory). */
static char helper[RAWJOB_KINDS][1024], scratch[1024];
static int have_helper[RAWJOB_KINDS];
static rawjob_runner runner;
static rawjob *queue, *running;
static int next_tag = 1;

static unsigned stamp[RAWJOB_KINDS];
static plugin_info formats[RAWJOB_KINDS];
static int switched_off[RAWJOB_KINDS];   /* [0] unused; dir[0] == 0: free */

/* Which plug-in made a preview: its program's path, size and date (a new
   version of the plug-in makes them again). */
unsigned rawjob_stamp_kind (int kind) { return kind >= 0 && kind < RAWJOB_KINDS ? stamp[kind] : 0; }
unsigned rawjob_stamp (void) { return stamp[RAWJOB_RAW]; }

int rawjob_set_format (const plugin_info *p)
{
  int k, free_k = -1;
  for (k = 1; k < RAWJOB_KINDS; k++)
    {
      if (formats[k].dir[0] && !strcmp (formats[k].dir, p->dir))
        break;
      if (!formats[k].dir[0] && free_k < 0)
        free_k = k;
    }
  if (k == RAWJOB_KINDS)
    k = free_k;
  if (k > 0)
    formats[k] = *p;
  return k;
}

void rawjob_clear_format (int kind)
{
  if (kind > 0 && kind < RAWJOB_KINDS)
    {
      memset (&formats[kind], 0, sizeof formats[kind]);
      have_helper[kind] = 0;
      helper[kind][0] = '\0';
      stamp[kind] = 0;
    }
}

const plugin_info *rawjob_format (int kind)
{
  return kind > 0 && kind < RAWJOB_KINDS && formats[kind].dir[0] ? &formats[kind] : NULL;
}

int rawjob_claims_name (const char *path)
{
  int k;
  for (k = 1; k < RAWJOB_KINDS; k++)
    if (formats[k].dir[0] && plugins_claims_name (&formats[k], path))
      return 1;
  return 0;
}

int rawjob_claims_type (int filetype)
{
  int k, f;
  if (filetype < 0 || filetype >= 0xFFD)
    return 0;
  for (k = 1; k < RAWJOB_KINDS; k++)
    for (f = 0; formats[k].dir[0] && f < formats[k].nformats; f++)
      {
        const char *w = formats[k].formats[f].mime;
        while (*w)
          {
            char mime[64];
            size_t l = 0;
            while (*w == ' ')
              w++;
            while (w[l] && w[l] != ' ')
              l++;
            if (l && l < sizeof mime)
              {
                memcpy (mime, w, l);
                mime[l] = '\0';
                if (fsys_type_of_mime (mime) == filetype)
                  return k;
              }
            w += l;
          }
      }
  return 0;
}

int rawjob_claims_mime (const char *mime)
{
  int k;
  for (k = 1; k < RAWJOB_KINDS; k++)
    if (formats[k].dir[0] && plugins_claims_mime (&formats[k], mime))
      return k;
  return 0;
}

int rawjob_kind_for (int format, const char *path, const unsigned char *head, size_t len)
{
  unsigned char buf[4200];
  int k;
  if (format == IMG_RAW)
    return RAWJOB_RAW;
  for (k = 1; k < RAWJOB_KINDS; k++)
    if (formats[k].dir[0] && plugins_claims_name (&formats[k], path))
      return k;
  if (!head && path)
    {
      FILE *f = fopen (path, "rb");
      if (f)
        {
          len = fread (buf, 1, sizeof buf, f);
          fclose (f);
          head = buf;
        }
    }
  for (k = 1; k < RAWJOB_KINDS; k++)
    if (formats[k].dir[0] && plugins_claims (&formats[k], NULL, head, len))
      return k;
  return -1;
}

void rawjob_setup (const char *h, const char *s, rawjob_runner run)
{
  rawjob_setup_kind (RAWJOB_RAW, h, s, run);
}

void rawjob_setup_kind (int kind, const char *h, const char *s, rawjob_runner run)
{
  int clean = !rawjob_busy ();
  have_helper[kind] = h && *h;
  stamp[kind] = 0;
  if (have_helper[kind])
    {
      fsys_entry e;
      const char *c;
      unsigned x = 2166136261u;
      for (c = h; *c; c++)
        x = (x ^ (unsigned char) *c) * 16777619u;
      if (fsys_stat (h, &e) == 0)
        {
          x = (x ^ (unsigned) e.size) * 16777619u;
          x = (x ^ (unsigned) e.mtime) * 16777619u;
          x = (x ^ (unsigned) (e.mtime >> 32)) * 16777619u;
        }
      stamp[kind] = x ? x : 1;
    }
  snprintf (helper[kind], sizeof helper[kind], "%s", h ? h : "");
  if (s && *s)
    snprintf (scratch, sizeof scratch, "%s", s);
  if (run)
    runner = run;
  if (have_helper[kind] && clean)
    {
      /* A fresh start: files left by jobs Lumen stopped waiting for (not
         while a job of the other plug-in is using the directory). */
      fsys_dir *d;
      fsys_entry e;
      fsys_mkdirs (scratch);
      if ((d = fsys_opendir (scratch)))
        {
          while (fsys_readdir (d, &e) == 1)
            if (!e.is_dir && e.name[0] == 'R')
              {
                char path[1100];
                fsys_join (path, sizeof path, scratch, e.name);
                remove (path);
              }
          fsys_closedir (d);
        }
    }
}

int rawjob_available (void) { return have_helper[RAWJOB_RAW]; }
int rawjob_available_kind (int kind) { return kind >= 0 && kind < RAWJOB_KINDS && have_helper[kind]; }
int rawjob_switched_off_kind (int kind) { return kind >= 0 && kind < RAWJOB_KINDS && switched_off[kind]; }

void rawjob_set_switched_off (int off) { switched_off[RAWJOB_RAW] = off; }
int rawjob_switched_off (void) { return switched_off[RAWJOB_RAW]; }
void rawjob_set_switched_off_kind (int kind, int off)
{
  if (kind >= 0 && kind < RAWJOB_KINDS)
    switched_off[kind] = off;
}

/* The name of the format a file is in ("AVIF photo"): which of the
   plug-in's formats claims it (the first if the file can't be looked at). */
const char *rawjob_format_name (int kind, const char *path, const unsigned char *head, size_t len)
{
  const plugin_info *p = rawjob_format (kind);
  unsigned char buf[4200];
  int f;
  if (!p)
    return "";
  if (!head && path)
    {
      FILE *fp = fopen (path, "rb");
      if (fp)
        {
          len = fread (buf, 1, sizeof buf, fp);
          fclose (fp);
          head = buf;
        }
    }
  f = plugins_claims (p, path, head, head ? len : 0);
  return p->formats[f > 0 ? f - 1 : 0].name;
}

const char *rawjob_missing_message_kind (int kind)
{
  return rawjob_missing_message_for (kind, NULL);
}

const char *rawjob_missing_message_for (int kind, const char *path)
{
  static char m[400];
  const plugin_info *p = rawjob_format (kind);
  const char *fmt = p ? rawjob_format_name (kind, path, NULL, 0) : "";
  const char *an = strchr ("AEIOUaeiou", fmt[0]) && fmt[0] ? "an" : "a";
  if (kind == RAWJOB_RAW)
    {
      if (switched_off[kind])
        return "This is a camera raw file, and the LibRaw plug-in that reads them is switched off. "
               "Tick it in Plug-ins on Lumen's icon bar menu.";
      return "This is a camera raw file. Lumen reads raw files with its LibRaw plug-in: "
             "install LumenRaw (from PackMan, or the LumenRaw zip from Lumen's release page), then open Lumen again.";
    }
  if (!p)
    return "Lumen has no plug-in for this photo's format now (was it removed?). "
           "Install it again, then open Lumen again.";
  if (switched_off[kind])
    snprintf (m, sizeof m, "This is %s %s, and the %s plug-in that reads them is switched off. "
              "Tick it in Plug-ins on Lumen's icon bar menu.", an, fmt, p->title);
  else if (p->api > PLUGIN_API)
    snprintf (m, sizeof m, "This is %s %s. The %s plug-in that reads them needs a newer Lumen.", an, fmt, p->title);
  else
    snprintf (m, sizeof m, "This is %s %s. The %s plug-in that reads them is missing its program: "
              "install %s again.", an, fmt, p->title, p->package[0] ? p->package : p->name);
  return m;
}

const char *rawjob_missing_message (void) { return rawjob_missing_message_kind (RAWJOB_RAW); }

static const char *plugin_name (int kind)
{
  const plugin_info *p = rawjob_format (kind);
  return p ? p->title : "LibRaw";
}

int rawjob_busy (void) { return queue || running; }

void rawjob_set_raw_noise (int level) { raw_noise = level < 0 ? 0 : level > 2 ? 2 : level; }

const char *rawjob_busy_title (void)
{
  const rawjob *j = running ? running : queue;
  const plugin_info *p;
  if (!j || j->kind == RAWJOB_RAW)
    return "LumenRaw";
  p = rawjob_format (j->kind);
  return p && p->title[0] ? p->title : "a plug-in";
}

static void unlink_job (rawjob *j)
{
  rawjob **p;
  for (p = &queue; *p; p = &(*p)->next)
    if (*p == j)
      {
        *p = j->next;
        break;
      }
  if (running == j)
    running = NULL;
}

static void fail (rawjob *j, const char *msg)
{
  snprintf (j->err, sizeof j->err, "%s", msg);
  j->state = J_FAILED;
  unlink_job (j);
}

/* Starts the first job waiting, if none is running. */
static void run_next (void)
{
  rawjob *j = queue;
  char cmd[2400];
  if (running || !j)
    return;
  queue = j->next;
  j->next = NULL;
  running = j;
  j->state = J_RUNNING;
  remove (j->image);
  remove (j->details);
  if (j->info.format == 1 && j->noise > 0)
    /* develop, with the raw plug-in's noise reduction (API: a 6th argument) */
    snprintf (cmd, sizeof cmd, "%s decode %s %s %s %d %d", helper[j->kind], j->src, j->image, j->details, j->info.width,
              j->noise);
  else if (j->info.format == 1)        /* develop */
    snprintf (cmd, sizeof cmd, "%s decode %s %s %s %d", helper[j->kind], j->src, j->image, j->details, j->info.width);
  else
    snprintf (cmd, sizeof cmd, "%s thumb %s %s %s", helper[j->kind], j->src, j->image, j->details);
  if (!runner || runner (cmd, j->tag))
    {
      char m[100];
      snprintf (m, sizeof m, "Lumen couldn't start its %s plug-in.", plugin_name (j->kind));
      fail (j, m);
    }
}

rawjob *rawjob_start (const char *src, int develop, int max_side, char *err, size_t n)
{
  return rawjob_start_kind (RAWJOB_RAW, src, develop, max_side, err, n);
}

rawjob *rawjob_start_kind (int kind, const char *src, int develop, int max_side, char *err, size_t n)
{
  rawjob *j, **p;
  if (!rawjob_available_kind (kind))
    {
      snprintf (err, n, "%s", rawjob_missing_message_for (kind, src));
      return NULL;
    }
  j = calloc (1, sizeof *j);
  if (!j)
    {
      snprintf (err, n, "Not enough memory");
      return NULL;
    }
  j->tag = next_tag++;
  j->kind = kind;
  j->noise = kind == RAWJOB_RAW && develop ? raw_noise : 0;
  snprintf (j->src, sizeof j->src, "%s", src);
  {
    char leaf[24];
    snprintf (leaf, sizeof leaf, "R%dp", j->tag);
    fsys_join (j->image, sizeof j->image, scratch, leaf);
    snprintf (leaf, sizeof leaf, "R%di", j->tag);
    fsys_join (j->details, sizeof j->details, scratch, leaf);
  }
  /* Kept until it runs: what to do. */
  j->info.format = develop;
  j->info.width = max_side;
  j->state = J_QUEUED;
  for (p = &queue; *p; p = &(*p)->next)
    ;
  *p = j;
  run_next ();
  return j;
}

/* Reads the details file: 1 if complete with no error, -1 if it reports
   one (in j->err), 0 if not there. */
static int read_details (rawjob *j)
{
  FILE *f = fopen (j->details, "r");
  char line[300];
  img_info *in = &j->info;
  if (!f)
    return 0;
  memset (in, 0, sizeof *in);
  in->format = j->kind == RAWJOB_RAW ? IMG_RAW : IMG_PLUGIN;
  in->orientation = 1;
  while (fgets (line, sizeof line, f))
    {
      char *v = strchr (line, '='), *e;
      if (!v)
        continue;
      *v++ = '\0';
      if ((e = strchr (v, '\n')))
        *e = '\0';
      if (!strcmp (line, "error"))
        {
          if (j->kind == RAWJOB_RAW)
            snprintf (j->err, sizeof j->err, "The LibRaw plug-in couldn't read this raw file (%s).", v);
          else
            snprintf (j->err, sizeof j->err, "The %s plug-in couldn't read this photo (%s).", plugin_name (j->kind), v);
          fclose (f);
          return -1;
        }
      else if (!strcmp (line, "width")) in->width = atoi (v);
      else if (!strcmp (line, "height")) in->height = atoi (v);
      else if (!strcmp (line, "orientation")) in->orientation = atoi (v);
      else if (!strcmp (line, "make")) snprintf (in->make, sizeof in->make, "%s", v);
      else if (!strcmp (line, "model")) snprintf (in->model, sizeof in->model, "%s", v);
      else if (!strcmp (line, "iso")) in->iso = atoi (v);
      else if (!strcmp (line, "exposure")) in->exposure = atof (v);
      else if (!strcmp (line, "fnumber")) in->fnumber = atof (v);
      else if (!strcmp (line, "focal")) in->focal = atof (v);
      else if (!strcmp (line, "lens")) snprintf (in->lens, sizeof in->lens, "%s", v);
      else if (!strcmp (line, "lensspec")) sscanf (v, "%lf %lf %lf", &in->lens_fmin, &in->lens_fmax, &in->lens_amin);
      else if (!strcmp (line, "focal35")) in->focal35 = atof (v);
      else if (!strcmp (line, "taken")) snprintf (in->taken, sizeof in->taken, "%s", v);
      else if (!strcmp (line, "encoding")) snprintf (in->encoding, sizeof in->encoding, "%s", v);
      else if (!strcmp (line, "exif"))
        {
          long a = 0, b = 0;
          if (sscanf (v, "%ld %ld", &a, &b) == 2 && a >= 0 && b > 8)
            {
              in->exif_offset = a;
              in->exif_length = b;
            }
        }
    }
  fclose (f);
  if (in->orientation < 1 || in->orientation > 8)
    in->orientation = 1;
  return 1;
}

int rawjob_poll (rawjob *j)
{
  fsys_entry e;
  if (j->state == J_DONE)
    return 1;
  if (j->state == J_FAILED)
    return -1;
  run_next ();
  if (j->state == J_QUEUED)
    return 0;
  /* Running, or ended: the picture is there once it's complete (renamed
     into place after the details). */
  if (fsys_stat (j->image, &e) == 0)
    {
      if (read_details (j) == 1)
        {
          j->state = J_DONE;
          unlink_job (j);
          run_next ();
          return 1;
        }
    }
  if (read_details (j) == -1)
    {
      j->state = J_FAILED;             /* read_details put the message in j->err */
      unlink_job (j);
      run_next ();
      return -1;
    }
  if (j->state == J_ENDED)
    {
      char m[160];
      snprintf (m, sizeof m, "The %s plug-in stopped without finishing (perhaps out of memory).", plugin_name (j->kind));
      fail (j, m);
      run_next ();
      return -1;
    }
  return 0;
}

/* Its task has ended: the next job can start at once, without waiting
   for this one's owner to look at its result (the Viewer opening a photo
   holds thumbnails back, and so the polling of a thumbnail's job: its
   plug-in would otherwise never get the next job, the Viewer's own,
   found when a double-clicked photo opened as its thumbnail was made). */
void rawjob_ended (int tag)
{
  if (running && running->tag == tag)
    {
      running->state = J_ENDED;
      running = NULL;
      run_next ();
    }
}

const char *rawjob_error (const rawjob *j) { return j->err; }
const char *rawjob_image (const rawjob *j) { return j->image; }
void rawjob_details (const rawjob *j, img_info *info) { *info = j->info; }

void rawjob_hurry (rawjob *j)
{
  rawjob **p;
  if (!j || j->state != J_QUEUED || queue == j)
    return;
  for (p = &queue; *p; p = &(*p)->next)
    if (*p == j)
      {
        *p = j->next;
        j->next = queue;
        queue = j;
        break;
      }
}

void rawjob_free (rawjob *j)
{
  if (!j)
    return;
  if (j->state == J_RUNNING)
    running = NULL;                    /* forgotten: it finishes on its own, and rawjob_setup clears its files up next time */
  unlink_job (j);
  remove (j->image);
  remove (j->details);
  free (j);
  run_next ();
}
