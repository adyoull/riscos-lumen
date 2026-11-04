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

   A format plug-in (Kind: format) reads photos in a format Lumen doesn't
   know itself, and says in its Plugin file which files are its own:

     Kind: format
     API: 1                       the version of the format plug-in API it follows
     Format: HEIF photo           what one of its files is called, in messages
     Extensions: heic heif hif    the names it claims ("/heic" or ".heic", any case)
     Signature: 4 ftypheic        and/or the bytes at an offset it claims (\xNN for
     Signature: 4 ftypmif1        any byte); up to PLUGIN_SIGS of them

   Lumen imports any file one of them claims, and runs its program for
   thumbnails and pictures as it runs LumenRaw: docs/plugin-api.md has the
   API. Nothing about the format is in Lumen itself.

   A plug-in directory with no Plugin file but a LumenRaw program in it is
   taken to be the LibRaw plug-in (as the first one was).

   The ones switched off are listed in a small text file in Lumen's
   Choices, one "off <directory name>" a line; everything else is on. */
#ifndef LUMEN_PLUGINS_H
#define LUMEN_PLUGINS_H
#include <stddef.h>

#define PLUGINS_MAX 32
#define PLUGIN_SIGS 8
#define PLUGIN_API 1                   /* the format plug-in API this Lumen follows */

typedef struct
{
  int offset, length;
  unsigned char bytes[24];
} plugin_sig;

typedef struct
{
  char dir[64];                        /* its directory's leaf name, which identifies it */
  char name[64];
  char title[64];                      /* for menus and messages: the package (LumenHEIF), else the name without a last "(...)" */
  char version[24];
  char kind[16];
  char program[1024];                  /* full path of the program, "" if it has none or it isn't there */
  char package[48];
  char description[256];
  int enabled;
  /* Kind: format */
  int api;                             /* 0 if it gives none */
  char format[48];                     /* "HEIF photo" */
  char extensions[160];                /* "heic heif hif", lower case */
  int nsigs;
  plugin_sig sigs[PLUGIN_SIGS];
} plugin_info;

/* 1 if the plug-in claims the file: its name's extension, or the first
   len bytes of it (head may be NULL). */
int plugins_claims (const plugin_info *p, const char *name, const unsigned char *head, size_t len);
/* Whether it claims the name alone (for choosing which files to look at). */
int plugins_claims_name (const plugin_info *p, const char *name);

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
