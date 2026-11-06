/* Copyright (c) 2026 Andrew Youll
   Released under the MIT licence: see the LICENSE file.
   SPDX-License-Identifier: MIT */
/* simd: whether Lumen's NEON routines are used.

   Lumen is built for ARMv7 with VFPv3, so it runs on every ARMv7
   processor; the routines that gain most from NEON (resample32,
   sharpen32, noise reduction, develop_rows and the layers in retouch.c)
   have NEON versions as well, chosen when Lumen starts if the
   processor has NEON (fsys_neon). Both give exactly the same pixels.
   Lumen$NEON (LUMEN_NEON on Linux) set to 0 or 1 overrides the choice. */
#ifndef LUMEN_SIMD_H
#define LUMEN_SIMD_H

/* 1 if the NEON routines are in this build (ARM). */
int simd_built (void);
/* 1 if they are used. */
int simd_neon (void);
/* Tests and LumenBench: use them (1), or not (0), or decide again (-1). */
void simd_force (int on);

#endif
