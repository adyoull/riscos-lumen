/* Copyright (c) 2026 Andrew Youll
   Released under the MIT licence: see the LICENSE file.
   SPDX-License-Identifier: MIT */
/* A small SQLite VFS on POSIX calls (UnixLib on RISC OS, glibc in tests).
   SQLite is built with SQLITE_OS_OTHER=1, so this is its only VFS.

   Why: SQLite's own unix VFS rewrites pathnames (prepends getcwd, splits at
   '/', opens directories to fsync them), which breaks native RISC OS
   pathnames. Here a pathname is used exactly as given.

   Locking is a no-op: RISC OS has no advisory locks. Lumen opens a library
   once (library.c refuses a second copy), and the catalogue uses
   exclusive locking mode, so one connection is the only writer. */
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include <sys/stat.h>
#include "sqlite3.h"

typedef struct
{
  sqlite3_file base;
  int fd;
  char *delete_on_close;
} lfile;

static int lclose (sqlite3_file *f)
{
  lfile *p = (lfile *) f;
  close (p->fd);
  if (p->delete_on_close)
    {
      unlink (p->delete_on_close);
      free (p->delete_on_close);
    }
  return SQLITE_OK;
}

static int lread (sqlite3_file *f, void *buf, int amt, sqlite3_int64 off)
{
  lfile *p = (lfile *) f;
  int got = 0;
  if (lseek (p->fd, (off_t) off, SEEK_SET) != (off_t) off)
    return SQLITE_IOERR_READ;
  while (got < amt)
    {
      ssize_t n = read (p->fd, (char *) buf + got, (size_t) (amt - got));
      if (n < 0)
        {
          if (errno == EINTR)
            continue;
          return SQLITE_IOERR_READ;
        }
      if (n == 0)
        break;
      got += (int) n;
    }
  if (got < amt)
    {
      memset ((char *) buf + got, 0, (size_t) (amt - got));
      return SQLITE_IOERR_SHORT_READ;
    }
  return SQLITE_OK;
}

static int lwrite (sqlite3_file *f, const void *buf, int amt, sqlite3_int64 off)
{
  lfile *p = (lfile *) f;
  int done = 0;
  if (lseek (p->fd, (off_t) off, SEEK_SET) != (off_t) off)
    return SQLITE_IOERR_WRITE;
  while (done < amt)
    {
      ssize_t n = write (p->fd, (const char *) buf + done, (size_t) (amt - done));
      if (n < 0)
        {
          if (errno == EINTR)
            continue;
          return errno == ENOSPC ? SQLITE_FULL : SQLITE_IOERR_WRITE;
        }
      done += (int) n;
    }
  return SQLITE_OK;
}

static int ltruncate (sqlite3_file *f, sqlite3_int64 size)
{
  return ftruncate (((lfile *) f)->fd, (off_t) size) ? SQLITE_IOERR_TRUNCATE : SQLITE_OK;
}

static int lsync (sqlite3_file *f, int flags)
{
  (void) flags;
  return fsync (((lfile *) f)->fd) ? SQLITE_IOERR_FSYNC : SQLITE_OK;
}

static int lsize (sqlite3_file *f, sqlite3_int64 *size)
{
  struct stat st;
  if (fstat (((lfile *) f)->fd, &st))
    return SQLITE_IOERR_FSTAT;
  *size = (sqlite3_int64) st.st_size;
  return SQLITE_OK;
}

static int llock (sqlite3_file *f, int level) { (void) f; (void) level; return SQLITE_OK; }
static int lunlock (sqlite3_file *f, int level) { (void) f; (void) level; return SQLITE_OK; }
static int lreserved (sqlite3_file *f, int *out) { (void) f; *out = 0; return SQLITE_OK; }
static int lcontrol (sqlite3_file *f, int op, void *arg) { (void) f; (void) op; (void) arg; return SQLITE_NOTFOUND; }
static int lsector (sqlite3_file *f) { (void) f; return 512; }
static int ldevice (sqlite3_file *f) { (void) f; return 0; }

static const sqlite3_io_methods io = {
  1, lclose, lread, lwrite, ltruncate, lsync, lsize, llock, lunlock,
  lreserved, lcontrol, lsector, ldevice, 0, 0, 0, 0, 0, 0
};

static int vopen (sqlite3_vfs *vfs, sqlite3_filename name, sqlite3_file *f, int flags, int *out)
{
  lfile *p = (lfile *) f;
  int oflags = 0;
  static int temp_count;
  char temp[64];
  (void) vfs;
  memset (p, 0, sizeof *p);
  if (!name)                           /* a temporary file: SQLite wants a name of its choice */
    {
      snprintf (temp, sizeof temp, "%s%d", getenv ("LUMEN_TEMP") ? getenv ("LUMEN_TEMP") : "LumenTmp", ++temp_count);
      name = temp;
      flags |= SQLITE_OPEN_DELETEONCLOSE | SQLITE_OPEN_CREATE;
    }
  if (flags & SQLITE_OPEN_READWRITE)
    oflags |= O_RDWR;
  else
    oflags |= O_RDONLY;
  if (flags & SQLITE_OPEN_CREATE)
    oflags |= O_CREAT;
  if (flags & SQLITE_OPEN_EXCLUSIVE)
    oflags |= O_EXCL;
  p->fd = open (name, oflags, 0644);
  if (p->fd < 0)
    return SQLITE_CANTOPEN;
  if (flags & SQLITE_OPEN_DELETEONCLOSE)
    p->delete_on_close = strdup (name);
  if (out)
    *out = flags;
  p->base.pMethods = &io;
  return SQLITE_OK;
}

static int vdelete (sqlite3_vfs *vfs, const char *name, int sync)
{
  (void) vfs; (void) sync;
  if (unlink (name) && errno != ENOENT)
    return SQLITE_IOERR_DELETE;
  return SQLITE_OK;
}

static int vaccess (sqlite3_vfs *vfs, const char *name, int flags, int *out)
{
  struct stat st;
  (void) vfs; (void) flags;
  *out = stat (name, &st) == 0;
  return SQLITE_OK;
}

static int vfull (sqlite3_vfs *vfs, const char *name, int n, char *out)
{
  (void) vfs;
  if ((int) strlen (name) >= n)
    return SQLITE_CANTOPEN;
  strcpy (out, name);
  return SQLITE_OK;
}

static int vrandom (sqlite3_vfs *vfs, int n, char *out)
{
  static unsigned s;
  int i;
  (void) vfs;
  s ^= (unsigned) time (NULL) ^ (unsigned) clock ();
  for (i = 0; i < n; i++)
    {
      s = s * 1103515245u + 12345u;
      out[i] = (char) (s >> 16);
    }
  return n;
}

static int vsleep (sqlite3_vfs *vfs, int us) { (void) vfs; return us; }

static int vtime64 (sqlite3_vfs *vfs, sqlite3_int64 *t)
{
  (void) vfs;
  /* Julian day in milliseconds: the Unix epoch is day 2440587.5. */
  *t = (sqlite3_int64) time (NULL) * 1000 + 210866760000000LL;
  return SQLITE_OK;
}

static int vtime (sqlite3_vfs *vfs, double *t)
{
  sqlite3_int64 i;
  vtime64 (vfs, &i);
  *t = (double) i / 86400000.0;
  return SQLITE_OK;
}

static int verror (sqlite3_vfs *vfs, int n, char *out) { (void) vfs; if (n) out[0] = 0; return 0; }

static sqlite3_vfs lumen_vfs = {
  2, sizeof (lfile), 1024, NULL, "lumen", NULL,
  vopen, vdelete, vaccess, vfull, NULL, NULL, NULL, NULL,
  vrandom, vsleep, vtime, verror, vtime64, NULL, NULL, NULL
};

int sqlite3_os_init (void)
{
  return sqlite3_vfs_register (&lumen_vfs, 1);
}

int sqlite3_os_end (void)
{
  return SQLITE_OK;
}
