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

struct rawjob
{
  int state, tag;
  char src[1024], image[300], details[300], err[300];
  img_info info;
  rawjob *next;                        /* in the queue */
};

static char helper[1024], scratch[1024];
static int have_helper;
static rawjob_runner runner;
static rawjob *queue, *running;
static int next_tag = 1;

static unsigned stamp;

/* Which plug-in made a raw's preview: its program's path, size and date
   (a new version of LumenRaw makes them again). */
unsigned rawjob_stamp (void) { return stamp; }

void rawjob_setup (const char *h, const char *s, rawjob_runner run)
{
  have_helper = h && *h;
  stamp = 0;
  if (have_helper)
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
      stamp = x ? x : 1;
    }
  snprintf (helper, sizeof helper, "%s", h ? h : "");
  snprintf (scratch, sizeof scratch, "%s", s ? s : "");
  runner = run;
  if (have_helper)
    {
      /* A fresh start: files left by jobs Lumen stopped waiting for. */
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

int rawjob_available (void) { return have_helper; }

static int switched_off;

void rawjob_set_switched_off (int off) { switched_off = off; }
int rawjob_switched_off (void) { return switched_off; }

const char *rawjob_missing_message (void)
{
  if (switched_off)
    return "This is a camera raw file, and the LibRaw plug-in that reads them is switched off. "
           "Tick it in Plug-ins on Lumen's icon bar menu.";
  return "This is a camera raw file. Lumen reads raw files with its LibRaw plug-in: "
         "install LumenRaw (from PackMan, or the LumenRaw zip from Lumen's release page), then open Lumen again.";
}

int rawjob_busy (void) { return queue || running; }

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
  if (j->info.format == 1)             /* develop */
    snprintf (cmd, sizeof cmd, "%s decode %s %s %s %d", helper, j->src, j->image, j->details, j->info.width);
  else
    snprintf (cmd, sizeof cmd, "%s thumb %s %s %s", helper, j->src, j->image, j->details);
  if (!runner || runner (cmd, j->tag))
    fail (j, "Lumen couldn't start its LibRaw plug-in.");
}

rawjob *rawjob_start (const char *src, int develop, int max_side, char *err, size_t n)
{
  rawjob *j, **p;
  if (!have_helper)
    {
      snprintf (err, n, "%s", rawjob_missing_message ());
      return NULL;
    }
  j = calloc (1, sizeof *j);
  if (!j)
    {
      snprintf (err, n, "Not enough memory");
      return NULL;
    }
  j->tag = next_tag++;
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
  in->format = IMG_RAW;
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
          snprintf (j->err, sizeof j->err, "The LibRaw plug-in couldn't read this raw file (%s).", v);
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
      else if (!strcmp (line, "taken")) snprintf (in->taken, sizeof in->taken, "%s", v);
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
      fail (j, "The LibRaw plug-in stopped without finishing (perhaps out of memory).");
      run_next ();
      return -1;
    }
  return 0;
}

void rawjob_ended (int tag)
{
  if (running && running->tag == tag)
    running->state = J_ENDED;
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
