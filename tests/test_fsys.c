/* Copyright (c) 2026 Andrew Youll
   Released under the MIT licence: see the LICENSE file.
   SPDX-License-Identifier: MIT */
/* fsys_riscos.c against the Fake RISC OS: OS_GBPB 10 records (word
   aligned, read over several calls), filetypes and dates from load and
   exec addresses, directories, OS_File 8 to create a path. */
#include <stdio.h>
#include <string.h>
#include "fake.h"
#include "swis.h"
#include "../src/core/fsys.h"

typedef struct { const char *name; unsigned load, exec, len; int type; } obj;
static const obj dir[] = {
  { "IMG_0001", 0xFFFC8501u, 0x23456789u, 123456, 1 },   /* JPEG, typed and dated */
  { "Day1", 0, 0, 0, 2 },                                 /* a directory */
  { "!App", 0, 0, 0, 2 },                                 /* an application directory */
  { "Untyped", 0x00008000u, 0x00008000u, 77, 1 },         /* load/exec addresses: untyped */
  { "Photos/zip", 0xFFFA9101u, 0, 999, 3 },               /* an image file: a file to us */
};
#define NOBJ (int) (sizeof dir / sizeof dir[0])

static int gbpb_calls;
static int fsc[10], fsc_calls;
static char fsc_src[128], fsc_dst[128];

/* OS_FSControl 26 (copy) and 27 (wipe): what they were asked. */
static const _kernel_oserror *fscontrol (int n, int r[10])
{
  (void) n;
  memcpy (fsc, r, sizeof fsc);
  snprintf (fsc_src, sizeof fsc_src, "%s", (const char *) r[1]);
  snprintf (fsc_dst, sizeof fsc_dst, "%s", r[0] == 26 ? (const char *) r[2] : "");
  fsc_calls++;
  return NULL;
}
static char created[8][128];
static int ncreated;

static const _kernel_oserror *swi (int n, int r[10])
{
  static const _kernel_oserror not_found = { 0xD6, "Not found" };
  if (n == OS_GBPB)
    {
      unsigned char *buf = (unsigned char *) r[2];
      int want = r[3], start = r[4], k = 0, pos = 0;
      CHECK (r[0] == 10, "OS_GBPB 10");
      CHECK (strcmp ((const char *) r[1], "SDFS::Pi.$.Photos") == 0, "the directory asked for");
      gbpb_calls++;
      /* Two entries a call, to test continuation. */
      if (want > 2)
        want = 2;
      while (k < want && start + k < NOBJ)
        {
          const obj *o = &dir[start + k];
          int *w = (int *) (buf + pos);
          size_t l = strlen (o->name);
          w[0] = (int) o->load; w[1] = (int) o->exec; w[2] = (int) o->len; w[3] = 3; w[4] = o->type;
          memcpy (buf + pos + 20, o->name, l + 1);
          pos += (int) ((20 + l + 1 + 3) & ~3u);
          k++;
        }
      r[3] = k;
      r[4] = start + k >= NOBJ ? -1 : start + k;
      return NULL;
    }
  if (n == OS_File)
    {
      const char *p = (const char *) r[1];
      if (r[0] == 17)
        {
          int i;
          r[0] = 0;
          if (strcmp (p, "SDFS::Pi.$.Photos") == 0 || strcmp (p, "SDFS::Pi.$") == 0)
            r[0] = 2;
          for (i = 0; i < ncreated; i++)
            if (strcmp (p, created[i]) == 0)
              r[0] = 2;
          if (strcmp (p, "SDFS::Pi.$.Photos.IMG_0001") == 0)
            {
              r[0] = 1; r[2] = (int) dir[0].load; r[3] = (int) dir[0].exec; r[4] = (int) dir[0].len;
            }
          return NULL;
        }
      if (r[0] == 8)
        {
          if (ncreated < 8)
            snprintf (created[ncreated++], sizeof created[0], "%s", p);
          return NULL;
        }
      return &not_found;
    }
  return NULL;
}

int main (void)
{
  fsys_dir *d;
  fsys_entry e;
  int got = 0;
  char path[256];
  fake_reset ();
  fake_swi (OS_GBPB, "OS_GBPB", swi);
  fake_swi (OS_File, "OS_File", swi);
  fake_swi (OS_FSControl, "OS_FSControl", fscontrol);

  CHECK (fsys_opendir ("SDFS::Pi.$.Nowhere") == NULL, "a missing directory doesn't open");
  d = fsys_opendir ("SDFS::Pi.$.Photos");
  CHECK (d != NULL, "the directory opens");
  while (d && fsys_readdir (d, &e) == 1)
    {
      if (got == 0)
        {
          CHECK (strcmp (e.name, "IMG_0001") == 0 && !e.is_dir, "first: a file");
          CHECK (e.filetype == 0xC85, "typed JPEG");
          CHECK (e.size == 123456, "its length");
          CHECK (e.mtime == (((long long) 0x01 << 32) | 0x23456789u), "date from load and exec");
        }
      else if (got == 1)
        CHECK (e.is_dir && e.filetype == FT_DIR && strcmp (e.name, "Day1") == 0, "second: a directory");
      else if (got == 2)
        CHECK (e.is_dir && strcmp (e.name, "!App") == 0, "third: an application directory");
      else if (got == 3)
        CHECK (!e.is_dir && e.filetype == FT_NONE, "fourth: untyped");
      else if (got == 4)
        CHECK (!e.is_dir && e.filetype == 0xA91 && strcmp (e.name, "Photos/zip") == 0, "fifth: an image file is a file");
      got++;
    }
  fsys_closedir (d);
  CHECK (got == NOBJ, "every entry, over several calls");
  CHECK (gbpb_calls == 3, "two entries a call: three calls");

  CHECK (fsys_stat ("SDFS::Pi.$.Photos.IMG_0001", &e) == 0 && e.filetype == 0xC85
         && strcmp (e.name, "IMG_0001") == 0, "fsys_stat: a typed file");
  CHECK (fsys_stat ("SDFS::Pi.$.Missing", &e) == -1, "fsys_stat: not found");

  fsys_join (path, sizeof path, "SDFS::Pi.$.Photos", "Day1");
  CHECK (strcmp (path, "SDFS::Pi.$.Photos.Day1") == 0, "join with '.'");
  CHECK (strcmp (fsys_leaf ("SDFS::Pi.$.Photos.Day1"), "Day1") == 0, "leaf after the last '.'");
  CHECK (strcmp (fsys_leaf ("Lumen:Library"), "Library") == 0, "leaf after a path variable's ':'");

  CHECK (fsys_mkdirs ("SDFS::Pi.$.Lumen.Library.Thumbs") == 0, "mkdirs");
  CHECK (ncreated == 3 && strcmp (created[0], "SDFS::Pi.$.Lumen") == 0
         && strcmp (created[2], "SDFS::Pi.$.Lumen.Library.Thumbs") == 0, "creates each level after $, in order");

  /* Plug-ins are copied in and deleted as whole directories: *Copy and
     *Wipe with recurse and force, and nothing that would ask or print. */
  CHECK (fsys_copy_tree ("SDFS::Pi.$.Downloads.LibRaw", "SDFS::Pi.$.Apps.!Lumen.Plugins.LibRaw") == 0
         && fsc[0] == 26 && fsc[3] == 3 && strcmp (fsc_src, "SDFS::Pi.$.Downloads.LibRaw") == 0
         && strcmp (fsc_dst, "SDFS::Pi.$.Apps.!Lumen.Plugins.LibRaw") == 0,
         "copy tree: OS_FSControl 26, recurse and force");
  CHECK (fsc[4] == 0 && fsc[5] == 0 && fsc[6] == 0 && fsc[7] == 0 && fsc[8] == 0, "copy tree: no dates, no buffer");
  CHECK (fsys_remove_tree ("SDFS::Pi.$.Apps.!Lumen.Plugins.Old") == 0 && fsc[0] == 27 && fsc[3] == 3
         && strcmp (fsc_src, "SDFS::Pi.$.Apps.!Lumen.Plugins.Old") == 0, "remove tree: OS_FSControl 27, recurse and force");
  CHECK (fsc_calls == 2, "one call each");
  return fake_done ("test_fsys");
}
