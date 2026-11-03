/* simd.c: see simd.h. */
#include <stdlib.h>
#include "fsys.h"
#include "simd.h"

static int neon = -1;

int simd_built (void)
{
#if defined(__arm__)
  return 1;
#else
  return 0;
#endif
}

int simd_neon (void)
{
  if (neon < 0)
    {
#ifdef __riscos
      const char *v = getenv ("Lumen$NEON");
#else
      const char *v = getenv ("LUMEN_NEON");
#endif
      if (!simd_built ())
        neon = 0;
      else if (v && (*v == '0' || *v == '1'))
        neon = *v == '1';
      else
        neon = fsys_neon ();
    }
  return neon;
}

void simd_force (int on)
{
  neon = on < 0 ? -1 : simd_built () ? on != 0 : 0;
}
