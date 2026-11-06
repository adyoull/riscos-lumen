/* Copyright (c) 2026 Andrew Youll
   Released under the MIT licence: see the LICENSE file.
   SPDX-License-Identifier: MIT */
/* presets: the user's presets, one Text file each, named after the
   preset, in a directory of their own (Lumen's: <Choices$Write>.Lumen.
   Presets). Every library uses them, and sharing one is copying its
   file: into another computer's Presets directory, or dropped on Lumen.

   A preset file is plain text: a first line saying what it is, the
   preset's name (which counts, whatever the file is called), then the
   look as a recipe writes it (recipe.h: tones and colours, no crop,
   retouching or brush strokes):

     Lumen preset 1
     Name: Show day
     exposure ev=0.35
     ...

   (Before 0.0.38 presets were kept in each library's catalogue;
   presets_take_from moves them here.) */
#ifndef LUMEN_PRESETS_H
#define LUMEN_PRESETS_H
#include <stddef.h>
#include "catalog.h"

#define PRESET_FIRST_LINE "Lumen preset 1"

/* Where they are (made when a preset is first saved). */
void presets_set_dir (const char *dir);
const char *presets_dir (void);

/* Every preset there, sorted by name (any case); ids are 1, 2...in that
   order, for the menus. 0, or -1 if the directory can't be read (none
   yet is 0 and a count of 0). Free *out. */
int presets_list (cat_preset **out, int *count);
/* Saves a preset, replacing one of the same name (any case). 0, or -1. */
int presets_save (const char *name, const char *look);
/* Deletes the preset of that name. 0, or -1 if there's none. */
int presets_delete (const char *name);

/* A leaf name for a preset's file (what the filing system can't have
   becomes _; on RISC OS spaces are hard spaces). */
void presets_leaf (const char *name, char *out, size_t n);

/* A preset file anywhere: written (Text), and read (0 if path is one:
   its name and look). */
int presets_write_file (const char *path, const char *name, const char *look);
int presets_read_file (const char *path, char *name, size_t namelen, char *look, size_t looklen);
/* A preset file from elsewhere (dropped on Lumen): saved here, by its
   name. 0, or -1 if it isn't a preset file. */
int presets_add_file (const char *path);

/* Moves a library's presets (from before 0.0.38, or brought in with an
   imported library) into the directory: those of a name not there yet
   are saved, and all are taken out of the catalogue. How many were new. */
int presets_take_from (cat *c);

#endif
