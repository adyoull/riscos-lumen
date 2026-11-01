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
  snprintf (path, sizeof path, "%s", v && *v ? v : "<Choices$Write>.Lumen.Library");
  if (app_init (path))
    return 1;
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
