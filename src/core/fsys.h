/* Copyright (c) 2026 Andrew Youll
   Released under the MIT licence: see the LICENSE file.
   SPDX-License-Identifier: MIT */
/* fsys: the little Lumen needs from a filing system, the same on RISC OS
   (fsys_riscos.c: OS_GBPB 10, OS_File) and Linux (fsys_posix.c, for tests).

   On RISC OS every path is a native RISC OS pathname
   ("SDFS::RISCOSpi.$.Photos.IMG_0001"); main.c sets
   __riscosify_control = __RISCOSIFY_NO_PROCESS so UnixLib passes them to
   the filing system unchanged. On Linux paths are ordinary POSIX ones. */
#ifndef LUMEN_FSYS_H
#define LUMEN_FSYS_H
#include <stddef.h>

/* RISC OS filetypes Lumen knows. */
#define FT_JPEG   0xC85
#define FT_PNG    0xB60
#define FT_TIFF   0xFF0
#define FT_DATA   0xFFD
#define FT_DIR    0x1000             /* as OS_File 23 reports directories */
#define FT_APP    0x2000             /* and application directories */
#define FT_NONE   (-1)               /* untyped */

typedef struct
{
  char name[256];                    /* leaf name */
  int is_dir;                        /* directory or application directory */
  int filetype;                      /* FT_* or another RISC OS type, FT_NONE */
  long long size;
  long long mtime;                   /* centiseconds since 1900 (RISC OS), seconds*100 since 1970 + offset (Linux) */
} fsys_entry;

typedef struct fsys_dir fsys_dir;

fsys_dir *fsys_opendir (const char *path);
/* 1: *e filled; 0: no more entries; -1: error. */
int fsys_readdir (fsys_dir *d, fsys_entry *e);
void fsys_closedir (fsys_dir *d);

/* 0 and *e filled if path exists (e->name is its leaf), -1 if not. */
int fsys_stat (const char *path, fsys_entry *e);

/* dir + separator + leaf, truncated safely to n bytes. */
void fsys_join (char *out, size_t n, const char *dir, const char *leaf);
/* The leaf of a path (points into path). */
const char *fsys_leaf (const char *path);
/* Create path and any missing parents. 0 on success. */
int fsys_mkdirs (const char *path);
/* Sets a file's RISC OS filetype (OS_File 18, PRM p339); nothing on Linux. */
int fsys_settype (const char *path, int filetype);
/* Expand to a canonical full pathname (system variables, paths).
   On Linux it copies. 0 on success. */
int fsys_canonical (const char *path, char *out, size_t n);

/* Copies a directory and everything in it to dst (which mustn't be
   there yet), and deletes one the same way. 0 on success. On RISC OS
   these are OS_FSControl 26 and 27 (*Copy and *Wipe). */
int fsys_copy_tree (const char *src, const char *dst);
int fsys_remove_tree (const char *path);

/* 1 if the processor has NEON (Advanced SIMD). On RISC OS VFPSupport
   says (VFPSupport_Features 0, SWI &58EC8: MVFR0 bits 0-3 = 2, 32
   doubleword registers, and MVFR1 bits 8-11, the integer instructions);
   on ARM Linux the kernel's HWCAP_NEON; elsewhere 0. */
int fsys_neon (void);

#endif
