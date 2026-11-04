/* Copyright (c) 2026 Andrew Youll
   Released under the MIT licence: see the LICENSE file.
   SPDX-License-Identifier: MIT */
/* naming: export file names from a pattern, for exports of several photos.

   A pattern is text with tokens in curly brackets:
     {name}     the photo's name (its original's leafname, no extension)
     {n}        its number in the export: 1, 2, 3...
     {nn} {nnn} {nnnn}   the same, padded with noughts: 01, 001, 0001
     {date}     the date it was taken, 2026-08-14 (or "undated")
     {year} {month} {day}   parts of that date
     {time}     the time it was taken, 14-05-09
     {project}  the project it is in
     {camera}   the camera's model
     {rating}   its stars, 0-5
   Anything else is kept as typed. The result is made safe for a RISC OS
   filename: characters RISC OS doesn't allow become "_", a full stop
   becomes "/" (as the Filer shows a dot in a DOS name), and it is cut to
   NAMING_MAX - 1 characters. An empty pattern is {name}. */
#ifndef LUMEN_NAMING_H
#define LUMEN_NAMING_H
#include <stddef.h>

#define NAMING_MAX 200

typedef struct
{
  const char *name;                    /* the photo's name, without extension */
  int n;                               /* 1-based number in this export */
  const char *taken;                   /* EXIF "2026:08:14 14:05:09", or "" */
  const char *project;
  const char *camera;
  int rating;
} naming_info;

/* The name for one photo. out must hold NAMING_MAX. Returns its length. */
int naming_expand (const char *pattern, const naming_info *ni, char *out);
/* 1 if the pattern has a token that differs between photos ({name}, {n}...),
   so names won't all be the same before any "_2" is added. */
int naming_varies (const char *pattern);
/* The tokens, for the Export box's pop-up menu: "{name}", ... NULL-ended. */
extern const char *const naming_tokens[];

#endif
