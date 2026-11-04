/* Copyright (c) 2026 Andrew Youll
   Released under the MIT licence: see the LICENSE file.
   SPDX-License-Identifier: MIT */
/* plugins: what is in Lumen's Plugins directory, and which are switched on.

   Each plug-in is a directory in !Lumen.Plugins (PackMan puts them
   there; one can also be added by dragging its directory to Lumen). In
   it, a text file, Plugin, describes it, one "Key: value" a line:

     Name: Camera raw (LibRaw)
     Version: 1.0
     Kind: raw                    what it does for Lumen ("raw": reads camera raw files)
     Program: LumenRaw            the program in the directory Lumen runs
     Package: LumenRaw            the PackMan package it came in, if any
     Description: Reads the raw files of ...

   A plug-in directory with no Plugin file but a LumenRaw program in it is
   taken to be the LibRaw plug-in (as the first one was).

   The ones switched off are listed in a small text file in Lumen's
   Choices, one "off <directory name>" a line; everything else is on. */
#ifndef LUMEN_PLUGINS_H
#define LUMEN_PLUGINS_H
#include <stddef.h>

#define PLUGINS_MAX 32

typedef struct
{
  char dir[64];                        /* its directory's leaf name, which identifies it */
  char name[64];
  char version[24];
  char kind[16];
  char program[1024];                  /* full path of the program, "" if it has none or it isn't there */
  char package[48];
  char description[256];
  int enabled;
} plugin_info;

/* Fills out[] from plugins_dir, at most max, sorted by name; the
   number found. state is the Choices file (may not exist yet). */
int plugins_scan (const char *plugins_dir, const char *state, plugin_info *out, int max);

/* Reads one plug-in directory's description. 0 if it is a plug-in. */
int plugins_read (const char *path, plugin_info *p);

/* Whether the plug-in in directory dir is switched on. */
int plugins_enabled (const char *state, const char *dir);
/* Switches it on or off. 0 on success. */
int plugins_set_enabled (const char *state, const char *dir, int on);

/* The first switched-on plug-in of a kind; NULL if none. */
const plugin_info *plugins_find (const plugin_info *list, int n, const char *kind);

/* Copies the plug-in directory src into plugins_dir. If one of that name
   is there already it is replaced only if replace is set; otherwise 1 is
   returned and nothing changes. -1 and a message in err on failure. */
int plugins_add (const char *plugins_dir, const char *src, int replace, char *err, size_t n);

/* Deletes the plug-in directory dir from plugins_dir. 0 on success. */
int plugins_remove (const char *plugins_dir, const char *dir, char *err, size_t n);

#endif
