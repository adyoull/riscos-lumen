/* Copyright (c) 2026 Andrew Youll
   Released under the MIT licence: see the LICENSE file.
   SPDX-License-Identifier: MIT */
/* Lumen's poll loop. Null events are asked for only while there is
   background work, so an idle Lumen costs the desktop nothing. */
#include <stdio.h>
#include <stdlib.h>
#include <kernel.h>
#include <swis.h>
#include <unixlib/local.h>
#include "ui.h"

/* Pathnames are native RISC OS ones throughout: UnixLib passes them on
   unchanged (see fsys.h). */
int __riscosify_control = __RISCOSIFY_NO_PROCESS;

int main (int argc, char **argv)
{
  /* A copy, at once: UnixLib's getenv hands back a buffer that the next
     getenv may reuse (found on a Pi: Lumen$Dir, read later, left this
     empty and the library had nowhere to go). */
  static char path[1024];
  const char *v = getenv ("Lumen$Library");
  int quit = 0;
  /* The library: Lumen$Library if it is set, else the Choices window's,
     else the usual place (!Run's Lumen$DefaultLibrary). */
  snprintf (path, sizeof path, "%s", v ? v : "");
  if (!path[0])
    {
      app_load_prefs ();
      snprintf (path, sizeof path, "%s", app_prefs ()->library);
    }
  if (!path[0])
    {
      v = getenv ("Lumen$DefaultLibrary");
      snprintf (path, sizeof path, "%s", v && *v ? v : "<Choices$Write>.Lumen.Library");
    }
  /* A photo to open: !Run passes it on (a double-click, File types). */
  switch (app_init (path, argc > 1 ? argv[1] : NULL))
    {
    case 0: break;
    case 2: return 0;                  /* handed over to the Lumen already running */
    default: return 1;
    }
  while (!quit)
    {
      int b[64], reason = 0, sender = 0, busy = app_busy ();
      unsigned wake;
      /* Mask Pointer_Leaving/Entering (bits 4, 5); nulls (bit 0) when idle,
         unless a watched folder is due a look: then Wimp_PollIdle (PRM
         p741) wakes Lumen at that time. Thumbnails nobody is looking at,
         while another program is in front, are made only in the Wimp's
         spare time: Wimp_PollIdle between slices. */
      if (busy && app_background_wait (&wake))
        {
          if (_swix (Wimp_PollIdle, _INR (0, 2) | _OUT (0) | _OUT (2), 0x30, b, (int) wake, &reason, &sender))
            continue;
        }
      else if (!busy && app_next_wake (&wake))
        {
          if (_swix (Wimp_PollIdle, _INR (0, 2) | _OUT (0) | _OUT (2), 0x30, b, (int) wake, &reason, &sender))
            continue;
        }
      else if (_swix (Wimp_Poll, _INR (0, 1) | _OUT (0) | _OUT (2), 0x30 | (busy ? 0 : 1), b, &reason, &sender))
        continue;
      quit = app_event (reason, b, sender);
    }
  app_quit ();
  return 0;
}
