/* Copyright (c) 2026 Andrew Youll
   Released under the MIT licence: see the LICENSE file.
   SPDX-License-Identifier: MIT */
/* styles: Lumen's built-in looks, shown under Presets > Styles. Each is
   the look part of a recipe (recipe_parse reads it); applying one replaces
   the photo's look and keeps its crop and geometry, as a preset does. */
#ifndef LUMEN_STYLES_H
#define LUMEN_STYLES_H

typedef struct
{
  const char *name;
  const char *about;                   /* one line, for interactive help */
  const char *recipe;
} style;

int styles_count (void);
const style *styles_get (int k);

#endif
