/* prefs: Lumen's Choices, a small text file of "key value" lines in
   <Choices$Write>.Lumen.Choices. Unknown keys are ignored and missing ones
   keep their defaults, so older and newer Lumens can share the file. */
#ifndef LUMEN_PREFS_H
#define LUMEN_PREFS_H

typedef struct
{
  char library[512];                   /* "" = the usual place; used from the next start */
  int export_format;                   /* EXPORT_JPEG or EXPORT_PNG */
  int export_quality;                  /* 1-100 */
  int export_size;                     /* longer side, 0 = full size */
  int thumb_size;                      /* 0 small, 1 medium, 2 large */
  int verify;                          /* check originals are still there at start */
  int watch_new;                       /* folders imported are watched for new photos */
  int albums_new;                      /* and get albums from the folders inside them */
  int watch_minutes;                   /* how often watched folders are looked through (0: only at start) */
} prefs;

void prefs_default (prefs *p);
/* Reads the file into *p (defaults first). 0 if it was there. */
int prefs_load (const char *path, prefs *p);
/* Writes it (making its directory if need be). 0 on success. */
int prefs_save (const char *path, const prefs *p);

#endif
