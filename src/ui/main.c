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

int main (void)
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
  switch (app_init (path))
    {
    case 0: break;
    case 2: return 0;                  /* handed over to the Lumen already running */
    default: return 1;
    }
  while (!quit)
    {
      int b[64], reason = 0, sender = 0;
      /* Mask Pointer_Leaving/Entering (bits 4, 5); nulls (bit 0) when idle. */
      int mask = 0x30 | (app_busy () ? 0 : 1);
      if (_swix (Wimp_Poll, _INR (0, 1) | _OUT (0) | _OUT (2), mask, b, &reason, &sender))
        continue;
      quit = app_event (reason, b, sender);
    }
  app_quit ();
  return 0;
}
