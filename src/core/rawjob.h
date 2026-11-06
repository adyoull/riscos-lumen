/* Copyright (c) 2026 Andrew Youll
   Released under the MIT licence: see the LICENSE file.
   SPDX-License-Identifier: MIT */
/* rawjob: camera raw files through the LibRaw plug-in, and other formats
   through format plug-ins (docs/plugin-api.md), run the same way.

   The plug-in is a separate program (LumenRaw, in its own PackMan
   package) that Lumen runs once for each job. A job writes two files: the
   picture (a JPEG preview, or a developed PPM) and a few lines of details
   (size, orientation, camera...). The front end runs the program in the
   background (on RISC OS in a task window, so the desktop carries on) and
   says when it has ended; jobs run one at a time, each needing a lot of
   memory for a big raw file.

     rawjob_setup (helper, scratch_dir, runner);   once, if the plug-in is there
     j = rawjob_start (src, 0, 0, err, n);          the camera's preview (thumbnails)
     j = rawjob_start (src, 1, 1600, err, n);       developed, half size if that's enough
     while ((r = rawjob_poll (j)) == 0) ... back to the Wimp ...
     r == 1: rawjob_image (j), rawjob_details (j, &info, &orientation)
     rawjob_free (j);                                its files are deleted */
#ifndef LUMEN_RAWJOB_H
#define LUMEN_RAWJOB_H
#include <stddef.h>
#include "exif.h"
#include "plugins.h"

/* Starts the command line in the background; tag identifies it in
   rawjob_ended. 0 if started. */
typedef int (*rawjob_runner) (const char *cmdline, int tag);

/* Kinds of plug-in, each its own program, their jobs sharing the queue:
   RAWJOB_RAW is LumenRaw (camera raw files); the others are format
   plug-ins (plugins.h), each claiming the files it reads. The functions
   without _kind are LumenRaw's. */
#define RAWJOB_KINDS 8
enum { RAWJOB_RAW = 0 };
/* A format plug-in, as installed (switched on or off): its kind, kept for
   as long as the same plug-in directory is there. -1 if all are in use. */
int rawjob_set_format (const plugin_info *p);
/* Forgets a format plug-in's kind (it has been removed). */
void rawjob_clear_format (int kind);
/* Its Plugin file, NULL if kind isn't a format plug-in's. */
const plugin_info *rawjob_format (int kind);
/* The kind of plug-in that reads a file of format (IMG_RAW, IMG_PLUGIN,
   or IMG_UNKNOWN for one not yet looked at), by its name or first bytes
   (head may be NULL: then they are read from path). -1 if none claims it. */
int rawjob_kind_for (int format, const char *path, const unsigned char *head, size_t len);
/* Whether an installed format plug-in claims a file by its name. */
int rawjob_claims_name (const char *path);
/* The format plug-in (its kind) whose MIME lines have this MIME type, or 0. */
int rawjob_claims_mime (const char *mime);
/* The format plug-in whose MIME types MimeMap gives this filetype, or 0. */
int rawjob_claims_type (int filetype);
void rawjob_setup_kind (int kind, const char *helper, const char *scratch, rawjob_runner run);
int rawjob_available_kind (int kind);
unsigned rawjob_stamp_kind (int kind);
/* What to say when a file's plug-in can't read it: not installed, or
   switched off. kind -1: no plug-in claims the file. */
const char *rawjob_missing_message_kind (int kind);
/* The same for a file: names the format it is in ("This is an AVIF photo..."). */
const char *rawjob_missing_message_for (int kind, const char *path);
/* Which of the plug-in's formats a file is in, by name ("AVIF photo"); its
   first if the file can't be looked at. head may be NULL (read from path). */
const char *rawjob_format_name (int kind, const char *path, const unsigned char *head, size_t len);
void rawjob_set_switched_off_kind (int kind, int off);
int rawjob_switched_off_kind (int kind);

/* helper NULL: no plug-in installed (rawjob_start explains). */
void rawjob_setup (const char *helper, const char *scratch, rawjob_runner run);
int rawjob_available (void);
/* Identifies the plug-in in use (its program, size and date); 0 if none. */
unsigned rawjob_stamp (void);
/* The message for a raw file when the plug-in isn't installed. */
const char *rawjob_missing_message (void);
/* A raw plug-in is installed but switched off: the message says so. */
void rawjob_set_switched_off (int off);
int rawjob_switched_off (void);

typedef struct rawjob rawjob;
rawjob *rawjob_start (const char *src, int develop, int max_side, char *err, size_t n);
rawjob *rawjob_start_kind (int kind, const char *src, int develop, int max_side, char *err, size_t n);
/* 0 still going (or waiting its turn), 1 done, -1 failed (rawjob_error). */
int rawjob_poll (rawjob *j);
const char *rawjob_error (const rawjob *j);
const char *rawjob_image (const rawjob *j);
/* The picture's details from the plug-in: size (before turning), camera,
   exposure, date; and its EXIF orientation. */
void rawjob_details (const rawjob *j, img_info *info);
void rawjob_free (rawjob *j);
/* Waiting its turn: to the front of the queue (the photo being opened). */
void rawjob_hurry (rawjob *j);

/* From the front end: the program started with tag has ended. */
void rawjob_ended (int tag);
int rawjob_busy (void);
/* The raw plug-in's own noise reduction (before demosaicing) for raw
   files developed by jobs started from now on: 0 off, 1 light, 2
   strong. A photo's own (recipe P_NOISE_RAW): set it just before
   starting its job, and back to 0 after. */
void rawjob_set_raw_noise (int level);
/* The plug-in at work (or next): its title ("LumenRaw", "LumenHEIF"). */
const char *rawjob_busy_title (void);                /* any job waiting or running */

#endif
