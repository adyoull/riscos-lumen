/* Copyright (c) 2026 Andrew Youll
   Released under the MIT licence: see the LICENSE file.
   SPDX-License-Identifier: MIT */
/* lens: the lens database the LumenLens plug-in carries (its Lenses file,
   made from lensfun's by tools/lensdb.py, which describes its form), and
   finding a photo's lens in it.

   Lumen reads the database itself, a line at a time: what each camera
   and lens is called, and where each lens's measurements start in the
   file, which are read only for the lens a photo needs. A profile is a
   lens's measurements worked out for the photo (recipe_lens): distortion
   and lateral colour at its focal length, vignetting at its focal length
   and aperture, each between the two nearest measured; and the scales
   that put the photo's corners where they were on the camera the lens
   was measured on (a camera with a smaller sensor sees only the middle
   of what the lens draws).

   Finding the lens: the camera, from its make and model, gives the mount
   (the lenses that fit) and its sensor's size (crop factor). The lens is
   the one whose name is most like the one the photo gives (EXIF
   LensModel, or what LibRaw finds in the camera's own notes), in words
   and numbers, among those that fit; without a name, the one whose focal
   lengths and aperture are the ones the photo gives (EXIF LensSpecification).
   A compact camera's fixed lens is found from the camera alone.

   A camera that isn't in the database can be stood in for by one that
   is, with the same mount and sensor size (lens_db_similar): the
   profile is scaled for that one's sensor. Without one, the photo is
   taken to be from a camera like the one the lens was measured on. */
#ifndef LUMEN_LENS_H
#define LUMEN_LENS_H
#include "recipe.h"

typedef struct lens_db lens_db;

/* What the photo says about its camera and lens. */
typedef struct
{
  char make[32], model[48];            /* the camera */
  char lens[96];                       /* the lens's name, "" if none */
  double focal, fnumber;               /* as taken; 0 unknown */
  double fmin, fmax, amin;             /* the lens's focal lengths and widest aperture, 0 unknown */
  double crop;                         /* its sensor's crop factor, from the 35 mm focal length; 0 unknown */
  int camera;                          /* a camera of the database's chosen for it; -1: by make and model */
} lens_photo;

/* What a photo's file says (its EXIF): the camera, focal length,
   aperture and lens, into ph (what ph has already is kept where the file
   says nothing). 0 if the file could be read. */
int lens_photo_read (lens_photo *ph, const char *path);

/* Reads the database's file (the plug-in's Data file). NULL if it can't
   be read or isn't one. */
lens_db *lens_db_open (const char *path);
void lens_db_close (lens_db *db);

int lens_db_count (const lens_db *db);
/* Lens i's maker and name, as the database gives them first. */
const char *lens_db_maker (const lens_db *db, int i);
const char *lens_db_name (const lens_db *db, int i);
/* Lens i's shortest and longest focal length (from its name, or what was
   measured) and its widest aperture (0 if its name doesn't say). */
void lens_db_range (const lens_db *db, int i, double *fmin, double *fmax, double *amin);

/* The crop factor of the camera lens i was measured on. */
double lens_db_lens_crop (const lens_db *db, int i);

/* The camera: its index (-1 if it isn't in the database), mount and crop
   factor (1 and "" when it isn't). */
int lens_db_camera (const lens_db *db, const char *make, const char *model, const char **mount, double *crop);
int lens_db_cameras (const lens_db *db);
/* Camera i's maker and model (as the database gives them first), mount
   and crop factor. */
const char *lens_db_camera_maker (const lens_db *db, int i);
const char *lens_db_camera_model (const lens_db *db, int i);
void lens_db_camera_info (const lens_db *db, int i, const char **mount, double *crop);
/* The camera the photo is taken to be from (ph->camera, else by its make
   and model): its index or -1. */
int lens_db_photo_camera (const lens_db *db, const lens_photo *ph);
/* Up to n cameras lens i fits (any lens, if i < 0), likeliest first:
   those with a crop factor nearest crop (0: the one lens i was measured
   with, or any), then of its own mount, then of maker make (NULL: any).
   How many. */
int lens_db_similar (const lens_db *db, int i, double crop, const char *make, int *out, int n);
/* Whether lens i fits the camera's mount ("" or NULL: any camera). */
int lens_db_fits (const lens_db *db, int i, const char *mount);

/* The photo's lens: its index, or -1 if there's none it's likely to be. */
int lens_db_match (const lens_db *db, const lens_photo *ph);
/* Up to n lenses the photo might have been taken with, best first (those
   that fit and cover its focal length, the likeliest by name): how many. */
int lens_db_suggest (const lens_db *db, const lens_photo *ph, int *out, int n);

/* Lens i's profile for the photo. 0, or -1 if it can't be read. */
int lens_db_profile (const lens_db *db, int i, const lens_photo *ph, recipe_lens *out);

/* How alike two names are, 0..1 (words and numbers in common). */
double lens_name_score (const char *a, const char *b);

#endif
