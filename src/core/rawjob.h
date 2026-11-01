/* rawjob: camera raw files, through the LibRaw plug-in.

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

/* Starts the command line in the background; tag identifies it in
   rawjob_ended. 0 if started. */
typedef int (*rawjob_runner) (const char *cmdline, int tag);

/* helper NULL: no plug-in installed (rawjob_start explains). */
void rawjob_setup (const char *helper, const char *scratch, rawjob_runner run);
int rawjob_available (void);
/* The message for a raw file when the plug-in isn't installed. */
const char *rawjob_missing_message (void);
/* A raw plug-in is installed but switched off: the message says so. */
void rawjob_set_switched_off (int off);

typedef struct rawjob rawjob;
rawjob *rawjob_start (const char *src, int develop, int max_side, char *err, size_t n);
/* 0 still going (or waiting its turn), 1 done, -1 failed (rawjob_error). */
int rawjob_poll (rawjob *j);
const char *rawjob_error (const rawjob *j);
const char *rawjob_image (const rawjob *j);
/* The picture's details from the plug-in: size (before turning), camera,
   exposure, date; and its EXIF orientation. */
void rawjob_details (const rawjob *j, img_info *info);
void rawjob_free (rawjob *j);

/* From the front end: the program started with tag has ended. */
void rawjob_ended (int tag);
int rawjob_busy (void);                /* any job waiting or running */

#endif
