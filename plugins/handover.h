/* Copyright (c) 2026 Andrew Youll
   Released under the MIT licence: see the LICENSE file.
   SPDX-License-Identifier: MIT */
/* handover.h: how a plug-in hands its developed picture to Lumen (shared
   by LumenRaw and LumenHEIF; static functions, included once by each).

   As a file: a binary PPM (P6, 8 bits) written under another name and
   renamed, so Lumen never sees half of it.

   In memory (plug-in API: decode's MEMORY argument, Lumen 0.0.38.7 and
   later): on RISC OS the same PPM is put in a dynamic area of its own,
   named "Lumen picture", and handover_info() gives the INFO line
   "memory=AREA LENGTH" that tells Lumen where; Lumen reads the picture
   straight from it and removes the area. Nothing goes through the disc:
   with the photos on an SD card, the 36 MB of a 12 MP photo isn't
   written and read back. Where there are no dynamic areas (or one can't
   be made) the file is written as before. */
#ifndef LUMEN_HANDOVER_H
#define LUMEN_HANDOVER_H
#include <stdio.h>
#include <string.h>
#ifdef __riscos
#include <kernel.h>
#endif

#define HANDOVER_AREA_NAME "Lumen picture"

/* The area made, and how much of it is the picture (0: none, a file). */
static int handover_area = -1;
static size_t handover_len;

static int handover_file (const char *path, const unsigned char *rgb, int w, int h)
{
  char tmp[1100];
  FILE *f;
  snprintf (tmp, sizeof tmp, "%s_t", path);
  f = fopen (tmp, "wb");
  if (!f)
    return -1;
  fprintf (f, "P6\n%d %d\n255\n", w, h);
  if (fwrite (rgb, (size_t) w * 3, (size_t) h, f) != (size_t) h)
    {
      fclose (f);
      return -1;
    }
  if (fclose (f))
    return -1;
  remove (path);
  return rename (tmp, path);
}

/* A dynamic area big enough for len bytes: its base, NULL if none. */
static unsigned char *handover_new_area (size_t len)
{
#ifdef __riscos
  _kernel_swi_regs r;
  memset (&r, 0, sizeof r);
  r.r[0] = 0;                          /* OS_DynamicArea 0: create */
  r.r[1] = -1;                         /* any number */
  r.r[2] = (int) len;                  /* its size now */
  r.r[3] = -1;                         /* anywhere */
  r.r[4] = 1 << 7;                     /* user read/write; not draggable in the Task manager */
  r.r[5] = (int) len;                  /* and at most */
  r.r[6] = 0;
  r.r[7] = 0;
  r.r[8] = (int) HANDOVER_AREA_NAME;
  if (_kernel_swi (0x66, &r, &r))      /* OS_DynamicArea */
    return NULL;
  handover_area = r.r[1];
  return (unsigned char *) r.r[3];
#else
  (void) len;
  return NULL;
#endif
}

/* The picture to Lumen: in memory if asked for and it can be, else as
   the file path. 0 on success. */
static int handover_write (const char *path, const unsigned char *rgb, int w, int h, int memory)
{
  if (memory)
    {
      char head[40];
      int hl = snprintf (head, sizeof head, "P6\n%d %d\n255\n", w, h);
      size_t len = (size_t) hl + (size_t) w * h * 3;
      unsigned char *a = handover_new_area (len);
      if (a)
        {
          memcpy (a, head, (size_t) hl);
          memcpy (a + hl, rgb, (size_t) w * h * 3);
          handover_len = len;
          return 0;
        }
    }
  return handover_file (path, rgb, w, h);
}

/* The INFO line for a picture in memory ("" for a file). */
static const char *handover_info (void)
{
  static char line[48];
  if (handover_area < 0)
    return "";
  snprintf (line, sizeof line, "memory=%d %lu\n", handover_area, (unsigned long) handover_len);
  return line;
}

/* The area given up again (the INFO couldn't be written: Lumen won't
   know of it). */
static void handover_drop (void)
{
#ifdef __riscos
  if (handover_area >= 0)
    {
      _kernel_swi_regs r;
      r.r[0] = 1;
      r.r[1] = handover_area;
      _kernel_swi (0x66, &r, &r);
    }
#endif
  handover_area = -1;
}
#endif
