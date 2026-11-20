/* Copyright (c) 2026 Andrew Youll
   Released under the MIT licence: see the LICENSE file.
   SPDX-License-Identifier: MIT */
/* stack: stacking frames of the night sky into one picture, a little at
   a time so the desktop goes on working.

   1. Darks: a master dark, each pixel the median of the dark frames'
      (taken with the lens cap on, at the same exposure, ISO and
      temperature as the lights), taken from every light before
      anything else: it removes the sensor's hot pixels and its glow.
   2. Stars: each light's stars, found in its brightness at half size
      (a colour mosaic read 2 x 2 to a pixel), and its sky's level in
      each colour.
   3. Lining up: the light with the most stars is the reference; every
      other is matched to it (astro.h). One that can't be is left out,
      and the report says why.
   4. Stacking: a band of rows at a time. Each light is read in full
      colour (demosaicked), moved onto the reference, and its sky brought
      to the reference's level (so a brightening sky doesn't count as a
      difference); each pixel is then the average of the lights' values
      with Winsorized sigma clipping, which leaves out satellites,
      planes, cosmic rays and hot pixels that moved with the frame.
   5. The result: a FITS file, 32-bit floating point (linear: Lumen
      stretches it for the screen), RGB for colour lights.

   Every light must be a FITS picture of the same size and kind; so must
   the darks. */
#ifndef LUMEN_STACK_H
#define LUMEN_STACK_H
#include <stddef.h>
#include "astro.h"

typedef struct
{
  char path[1024];
  int stars;                  /* stars found */
  int used;                   /* 1 if stacked */
  int reference;              /* 1 if the others were lined up to it */
  double dx, dy, angle, rms;  /* how it lined up: moved, turned (degrees), rms (pixels) */
  int pairs;                  /* stars that agreed */
  char why[160];              /* why it was left out, or "" */
} stack_frame;

typedef struct stack_job stack_job;

/* Checks the frames and starts. creator: the program, for the FITS
   header ("Lumen 0.0.40"). NULL with a message in err if the frames
   can't be stacked (not FITS, sizes differ, fewer than two...). */
stack_job *stack_start (const char *const *lights, int nlights, const char *const *darks, int ndarks, const char *out,
                        const char *creator, char *err, size_t errlen);
/* Works until now() reaches deadline. 0: more to do; 1: finished, the
   file written; -1: failed (stack_error says why; nothing is left
   behind). */
int stack_step (stack_job *j, unsigned (*now) (void), unsigned deadline);
const char *stack_error (const stack_job *j);
/* How far it has got (0-100) and what it is doing, for people. */
int stack_progress (const stack_job *j, char *what, size_t n);
/* The lights, as found so far (all of them once finished). */
int stack_frames (const stack_job *j, const stack_frame **frames);
/* Once finished: what happened, for people ("18 of 20 frames stacked,
   ..."). */
void stack_summary (const stack_job *j, char *out, size_t n);
const char *stack_output (const stack_job *j);
/* Stops (if not finished, the part-written file is removed) and frees. */
void stack_free (stack_job *j);

#endif
