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

   A format plug-in (Kind: format) reads photos in formats Lumen doesn't
   know itself, and says in its Plugin file which files are its own, a
   Format line starting each format it reads:

     Kind: format
     API: 1                       the version of the format plug-in API it follows
     Format: HEIF photo           what one of its files is called, in messages
     Extensions: heic heif hif    the names it claims ("/heic" or ".heic", any case)
     Signature: 4 ftypheic        and/or the bytes at an offset it claims (\xNN for
     Signature: 4 ftypmif1        any byte); up to PLUGIN_SIGS a format
     Format: AVIF photo           the next format: up to PLUGIN_FORMATS
     Extensions: avif
     Signature: 4 ftypavif

   Lumen imports any file one of them claims, and runs its program for
   thumbnails and pictures as it runs LumenRaw: docs/plugin-api.md has the
   API. Nothing about the format is in Lumen itself.

   A data plug-in has no program, but a file of facts Lumen reads itself,
   named by a Data line. The lens one (Kind: lens) is the lens database
   lens.h reads:

     Kind: lens
     Data: Lenses                 the file in the directory

   A plug-in directory with no Plugin file but a LumenRaw program in it is
   taken to be the LibRaw plug-in (as the first one was).

   The ones switched off are listed in a small text file in Lumen's
   Choices, one "off <directory name>" a line; everything else is on. */
#ifndef LUMEN_PLUGINS_H
#define LUMEN_PLUGINS_H
#include <stddef.h>

#define PLUGINS_MAX 32
#define PLUGIN_SIGS 12                 /* a format */
#define PLUGIN_FORMATS 6               /* a plug-in */
#define PLUGIN_API 1                   /* the format plug-in API this Lumen follows */

typedef struct
{
  int offset, length;
  unsigned char bytes[24];
} plugin_sig;

/* One format a format plug-in reads. */
typedef struct
{
  char name[48];                       /* "HEIF photo" */
  char extensions[160];                /* "heic heif hif", lower case */
  char mime[160];                      /* "image/heic image/heif", lower case: MimeMap gives their filetypes and extensions */
  int nsigs;
  plugin_sig sigs[PLUGIN_SIGS];
} plugin_format;

typedef struct
{
  char dir[64];                        /* its directory's leaf name, which identifies it */
  char name[64];
  char title[64];                      /* for menus and messages: the package (LumenHEIF), else the name without a last "(...)" */
  char version[24];
  char kind[16];
  char program[1024];                  /* full path of the program, "" if it has none or it isn't there */
  char data[1024];                     /* full path of its Data file, "" if it has none or it isn't there */
  char package[48];
  char description[256];
  int enabled;
  /* Kind: format */
  int api;                             /* 0 if it gives none */
  int nformats;                        /* at least 1 for Kind: format */
  plugin_format formats[PLUGIN_FORMATS];
} plugin_info;

/* Whether the plug-in claims the file, by its name's extension or the
   first len bytes of it (head may be NULL): 1 + the format's index, 0 if
   not. */
int plugins_claims (const plugin_info *p, const char *name, const unsigned char *head, size_t len);
/* The same by the name alone (for choosing which files to look at). */
int plugins_claims_name (const plugin_info *p, const char *name);
/* The same by a MIME type ("image/avif"), from its MIME lines. */
int plugins_claims_mime (const plugin_info *p, const char *mime);

/* Fills out[] from plugins_dir, at most max, sorted by name; the
   number found. state is the Choices file (may not exist yet). */
int plugins_scan (const char *plugins_dir, const char *state, plugin_info *out, int max);

/* Reads one plug-in directory's description. 0 if it is a plug-in. */
int plugins_read (const char *path, plugin_info *p);

/* Whether the plug-in in directory dir is switched on. */
int plugins_enabled (const char *state, const char *dir);
/* Switches it on or off. 0 on success. */
int plugins_set_enabled (const char *state, const char *dir, int on);

/* The first switched-on plug-in of a kind (with its program or data
   file there); NULL if none. */
const plugin_info *plugins_find (const plugin_info *list, int n, const char *kind);

/* Copies the plug-in directory src into plugins_dir. If one of that name
   is there already it is replaced only if replace is set; otherwise 1 is
   returned and nothing changes. -1 and a message in err on failure. */
int plugins_add (const char *plugins_dir, const char *src, int replace, char *err, size_t n);

/* Deletes the plug-in directory dir from plugins_dir. 0 on success. */
int plugins_remove (const char *plugins_dir, const char *dir, char *err, size_t n);

#endif
