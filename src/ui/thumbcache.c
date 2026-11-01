/* thumbcache.c: thumbnails as 32bpp sprites, each in its own little sprite
   area (Format Of Sprite Area, PRM p1178; Format Of Sprite, p1177): a
   16-byte area header, a 44-byte sprite header, then rows of
   &00BBGGRR words, top row first. Mode word &301680B5: sprite type 6
   (32bpp), 90 dpi both ways.

   The browser asks for sprites while redrawing; missing ones are queued
   (most recent request first, since that is what is on screen) and loaded
   in idle time, so a redraw never waits for a disc. Least recently used
   sprites are dropped when the cache passes its size. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "thumbgen.h"
#include "ui.h"

#define MAX_QUEUE 512
#define SPRITE_MODE_32BPP 0x301680B5

typedef struct entry
{
  cat_id id;
  thumb_sprite s;
  size_t bytes;
  unsigned long used;                  /* for LRU */
  struct entry *next;                  /* hash chain */
} entry;

#define BUCKETS 509
static entry *table[BUCKETS];
static size_t total, limit = 48u << 20;
static unsigned long tick;
static cat_id queue[MAX_QUEUE];
static int nqueue;

void thumbcache_init (size_t max_bytes)
{
  limit = max_bytes;
}

static entry **slot (cat_id id)
{
  entry **e = &table[(unsigned long long) id % BUCKETS];
  while (*e && (*e)->id != id)
    e = &(*e)->next;
  return e;
}

int thumbcache_make_sprite (const unsigned char *rgb, int w, int h, thumb_sprite *out)
{
  size_t bytes = 16 + 44 + (size_t) w * h * 4;
  unsigned *a = malloc (bytes), *sp, *px;
  int i, n = w * h;
  if (!a)
    return -1;
  a[0] = (unsigned) bytes;             /* area size */
  a[1] = 1;                            /* sprites */
  a[2] = 16;                           /* first sprite */
  a[3] = (unsigned) bytes;             /* first free word */
  sp = a + 4;
  sp[0] = (unsigned) (44 + (size_t) w * h * 4);   /* offset to next */
  memset (sp + 1, 0, 12);
  memcpy (sp + 1, "thumb", 5);
  sp[4] = (unsigned) (w - 1);          /* width in words - 1: one word a pixel */
  sp[5] = (unsigned) (h - 1);
  sp[6] = 0;                           /* first bit used */
  sp[7] = 31;                          /* last bit used */
  sp[8] = 44;                          /* image */
  sp[9] = 44;                          /* no mask: same as the image */
  sp[10] = SPRITE_MODE_32BPP;
  px = sp + 11;
  for (i = 0; i < n; i++)
    px[i] = (unsigned) rgb[3 * i] | (unsigned) rgb[3 * i + 1] << 8 | (unsigned) rgb[3 * i + 2] << 16;
  out->area = a;
  out->sprite = sp;
  out->w = w;
  out->h = h;
  return 0;
}

static void evict (void)
{
  while (total > limit)
    {
      entry **oldest = NULL;
      int b;
      for (b = 0; b < BUCKETS; b++)
        {
          entry **e;
          for (e = &table[b]; *e; e = &(*e)->next)
            if (!oldest || (*e)->used < (*oldest)->used)
              oldest = e;
        }
      if (!oldest)
        return;
      {
        entry *gone = *oldest;
        *oldest = gone->next;
        total -= gone->bytes;
        free (gone->s.area);
        free (gone);
      }
    }
}

const thumb_sprite *thumbcache_get (cat_id id, int thumb_ready)
{
  entry *e = *slot (id);
  int i;
  if (e)
    {
      e->used = ++tick;
      return &e->s;
    }
  if (!thumb_ready)
    return NULL;
  /* Queue it, most recent first; move it up if already queued. */
  for (i = 0; i < nqueue; i++)
    if (queue[i] == id)
      break;
  if (i == nqueue)                     /* not queued: take a new slot, or the oldest's */
    {
      if (nqueue < MAX_QUEUE)
        nqueue++;
      i = nqueue - 1;
    }
  memmove (queue + 1, queue, sizeof *queue * (size_t) i);
  queue[0] = id;
  return NULL;
}

void thumbcache_drop (cat_id id)
{
  entry **s = slot (id), *e = *s;
  if (!e)
    return;
  *s = e->next;
  total -= e->bytes;
  free (e->s.area);
  free (e);
}

int thumbcache_pending (void)
{
  return nqueue;
}

void thumbcache_clear_queue (void)
{
  nqueue = 0;
}

int thumbcache_work (unsigned (*now) (void), unsigned deadline, void (*loaded) (cat_id))
{
  int n = 0;
  while (nqueue > 0 && (n == 0 || (int) (now () - deadline) < 0))
    {
      cat_id id = queue[0];
      char path[1100], err[200];
      rgb_image img;
      entry *e;
      memmove (queue, queue + 1, sizeof *queue * (size_t) --nqueue);
      if (*slot (id))
        continue;
      lib_thumb_path (app_library (), id, path, sizeof path);
      if (img_load_jpeg (path, &img, err, sizeof err))
        continue;                      /* the browser draws a placeholder */
      e = calloc (1, sizeof *e);
      if (!e || thumbcache_make_sprite (img.rgb, img.w, img.h, &e->s))
        {
          free (e);
          free (img.rgb);
          continue;
        }
      free (img.rgb);
      e->id = id;
      e->bytes = 60 + (size_t) img.w * img.h * 4;
      e->used = ++tick;
      e->next = NULL;
      *slot (id) = e;
      total += e->bytes;
      evict ();
      n++;
      if (loaded)
        loaded (id);
    }
  return n;
}
