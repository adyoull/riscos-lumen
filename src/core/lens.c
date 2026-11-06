/* Copyright (c) 2026 Andrew Youll
   Released under the MIT licence: see the LICENSE file.
   SPDX-License-Identifier: MIT */
/* lens.c: see lens.h. */
#include <ctype.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "exif.h"
#include "lens.h"

/* Names are kept in one pool, each as its spellings one after another
   (each ends with a nul, the last with two), found by offset. */
typedef struct
{
  size_t maker, model, mounts;
  float crop, aspect, fmin, fmax, amin;
  char type, named;                    /* named: its focal lengths are its name's */
  long at;                             /* where its first measurement line starts */
} lens_entry;

typedef struct { size_t maker, model, mount; float crop; } cam_entry;
typedef struct { size_t name, compat; } mount_entry;

struct lens_db
{
  char path[1024];
  char *pool;
  size_t used, room;
  lens_entry *L;
  int nl, roomL;
  cam_entry *C;
  int nc, roomC;
  mount_entry *M;
  int nm, roomM;
};

#define STR(db, off) ((db)->pool + (off))

static int grow (void **p, int *room, int n, size_t size)
{
  void *q;
  if (n < *room)
    return 0;
  q = realloc (*p, (size_t) (*room ? *room * 2 : 256) * size);
  if (!q)
    return -1;
  *p = q;
  *room = *room ? *room * 2 : 256;
  return 0;
}

/* Adds s (len bytes) as a multi-string: each of seps starts the next spelling. */
static int add_names (lens_db *db, const char *s, size_t len, char sep, size_t *off)
{
  size_t i;
  if (db->used + len + 2 > db->room)
    {
      size_t room = (db->room ? db->room * 2 : 65536) + len + 2;
      char *q = realloc (db->pool, room);
      if (!q)
        return -1;
      db->pool = q;
      db->room = room;
    }
  *off = db->used;
  for (i = 0; i < len; i++)
    db->pool[db->used++] = s[i] == sep ? '\0' : s[i];
  db->pool[db->used++] = '\0';
  db->pool[db->used++] = '\0';
  return 0;
}

/* The next spelling after s, or NULL. */
static const char *next_name (const char *s)
{
  s += strlen (s) + 1;
  return *s ? s : NULL;
}

/* Splits line at '|' into up to n fields (in place). How many. */
static int fields (char *line, char **f, int n)
{
  int k = 0;
  f[k++] = line;
  while (k < n && (line = strchr (line, '|')) != NULL)
    {
      *line++ = '\0';
      f[k++] = line;
    }
  return k;
}

/* The focal lengths ("16-35mm", "50 mm") and widest aperture ("f/2.8",
   "1:2.8") a lens's name gives; 0 where it doesn't. */
static void name_range (const char *s, double *fmin, double *fmax, double *amin)
{
  const char *p;
  *fmin = *fmax = *amin = 0;
  for (p = s; *p; p++)
    {
      if (isdigit ((unsigned char) *p) && (p == s || !isalnum ((unsigned char) p[-1]) || isalpha ((unsigned char) p[-1])))
        {
          char *end;
          double a = strtod (p, &end), b = a;
          const char *q = end;
          if (*q == '-' && isdigit ((unsigned char) q[1]))
            {
              b = strtod (q + 1, &end);
              q = end;
            }
          while (*q == ' ')
            q++;
          if ((q[0] == 'm' || q[0] == 'M') && (q[1] == 'm' || q[1] == 'M') && !*fmin && a > 0)
            {
              *fmin = a;
              *fmax = b >= a ? b : a;
            }
          p = end - 1;
          continue;
        }
      if (!*amin && ((p[0] == 'f' || p[0] == 'F') && p[1] == '/' && isdigit ((unsigned char) p[2])))
        *amin = strtod (p + 2, NULL);
      else if (!*amin && p[0] == '1' && p[1] == ':' && isdigit ((unsigned char) p[2]) && (p == s || !isdigit ((unsigned char) p[-1])))
        *amin = strtod (p + 2, NULL);
    }
}

lens_db *lens_db_open (const char *path)
{
  FILE *f = fopen (path, "r");
  char line[2048];
  lens_db *db;
  lens_entry *cur = NULL;
  int measured = 0;
  if (!f)
    return NULL;
  if (!fgets (line, sizeof line, f) || strncmp (line, "LumenLens 1", 11) != 0 || !(db = calloc (1, sizeof *db)))
    {
      fclose (f);
      return NULL;
    }
  snprintf (db->path, sizeof db->path, "%s", path);
  while (fgets (line, sizeof line, f))
    {
      size_t l = strlen (line);
      char *fl[8];
      int n;
      while (l && (line[l - 1] == '\n' || line[l - 1] == '\r'))
        line[--l] = '\0';
      if (l < 2 || line[1] != ' ')
        continue;
      if (line[0] == 'D' || line[0] == 'T' || line[0] == 'V')
        {
          double fo = strtod (line + 2, NULL);
          measured++;
          if (cur && fo > 0 && !cur->named)
            {
              if (!cur->fmin || fo < cur->fmin)
                cur->fmin = (float) fo;
              if (fo > cur->fmax)
                cur->fmax = (float) fo;
            }
          continue;
        }
      n = fields (line + 2, fl, 8);
      if (line[0] == 'M' && n >= 1)
        {
          char *bar;
          mount_entry *m;
          if (grow ((void **) &db->M, &db->roomM, db->nm, sizeof *db->M))
            break;
          m = &db->M[db->nm];
          /* the rest of the fields, '|' again between them */
          for (bar = fl[0] + strlen (fl[0]); bar < line + l; bar++)
            if (!*bar)
              *bar = '|';
          bar = strchr (fl[0], '|');
          if (bar)
            *bar++ = '\0';
          if (add_names (db, fl[0], strlen (fl[0]), '|', &m->name)
              || add_names (db, bar ? bar : "", bar ? strlen (bar) : 0, '|', &m->compat))
            break;
          db->nm++;
        }
      else if (line[0] == 'C' && n >= 4)
        {
          cam_entry *c;
          if (grow ((void **) &db->C, &db->roomC, db->nc, sizeof *db->C))
            break;
          c = &db->C[db->nc];
          if (add_names (db, fl[0], strlen (fl[0]), '~', &c->maker) || add_names (db, fl[1], strlen (fl[1]), '~', &c->model)
              || add_names (db, fl[2], strlen (fl[2]), '\0', &c->mount))
            break;
          c->crop = (float) strtod (fl[3], NULL);
          if (!(c->crop > 0.05f))
            c->crop = 1;
          db->nc++;
        }
      else if (line[0] == 'L' && n >= 6)
        {
          double a, b, ap;
          if (cur && !measured)
            db->nl--;                  /* nothing measured: no use */
          measured = 0;
          if (grow ((void **) &db->L, &db->roomL, db->nl, sizeof *db->L))
            break;
          cur = &db->L[db->nl];
          memset (cur, 0, sizeof *cur);
          if (add_names (db, fl[0], strlen (fl[0]), '~', &cur->maker) || add_names (db, fl[1], strlen (fl[1]), '~', &cur->model)
              || add_names (db, fl[2], strlen (fl[2]), ';', &cur->mounts))
            break;
          cur->crop = (float) strtod (fl[3], NULL);
          cur->aspect = (float) strtod (fl[4], NULL);
          if (!(cur->crop > 0.05f))
            cur->crop = 1;
          if (!(cur->aspect >= 1))
            cur->aspect = 1.5f;
          cur->type = fl[5][0] ? fl[5][0] : 'r';
          cur->at = ftell (f);
          name_range (STR (db, cur->model), &a, &b, &ap);
          cur->amin = (float) ap;
          if (a > 0)
            {
              /* the name's, not what was measured */
              cur->fmin = (float) a;
              cur->fmax = (float) b;
              cur->named = 1;
            }
          db->nl++;
        }
    }
  fclose (f);
  if (cur && !measured)
    db->nl--;
  if (!db->nl)
    {
      lens_db_close (db);
      return NULL;
    }
  return db;
}

void lens_db_close (lens_db *db)
{
  if (!db)
    return;
  free (db->pool);
  free (db->L);
  free (db->C);
  free (db->M);
  free (db);
}

int lens_db_count (const lens_db *db) { return db ? db->nl : 0; }
const char *lens_db_maker (const lens_db *db, int i) { return i >= 0 && i < db->nl ? STR (db, db->L[i].maker) : ""; }
const char *lens_db_name (const lens_db *db, int i) { return i >= 0 && i < db->nl ? STR (db, db->L[i].model) : ""; }
int lens_db_cameras (const lens_db *db) { return db ? db->nc : 0; }
const char *lens_db_camera_maker (const lens_db *db, int i) { return i >= 0 && i < db->nc ? STR (db, db->C[i].maker) : ""; }
const char *lens_db_camera_model (const lens_db *db, int i) { return i >= 0 && i < db->nc ? STR (db, db->C[i].model) : ""; }

void lens_db_range (const lens_db *db, int i, double *fmin, double *fmax, double *amin)
{
  *fmin = *fmax = *amin = 0;
  if (i < 0 || i >= db->nl)
    return;
  *fmin = db->L[i].fmin;
  *fmax = db->L[i].fmax;
  *amin = db->L[i].amin;
}

double lens_db_lens_crop (const lens_db *db, int i) { return i >= 0 && i < db->nl ? db->L[i].crop : 1; }

void lens_db_camera_info (const lens_db *db, int i, const char **mount, double *crop)
{
  *mount = i >= 0 && i < db->nc ? STR (db, db->C[i].mount) : "";
  *crop = i >= 0 && i < db->nc ? db->C[i].crop : 1;
}

/* --- names */

/* Letters and digits only, lower case. */
static void squash (const char *s, char *out, size_t n)
{
  size_t k = 0;
  for (; *s && k + 1 < n; s++)
    if (isalnum ((unsigned char) *s))
      out[k++] = (char) tolower ((unsigned char) *s);
  out[k] = '\0';
}

static int same_name (const char *a, const char *b)
{
  char x[128], y[128];
  squash (a, x, sizeof x);
  squash (b, y, sizeof y);
  return x[0] && !strcmp (x, y);
}

/* First word, squashed ("NIKON CORPORATION" -> "nikon"). */
static void first_word (const char *s, char *out, size_t n)
{
  char w[64];
  size_t k = 0;
  while (*s && !isalnum ((unsigned char) *s))
    s++;
  while (*s && (isalnum ((unsigned char) *s) || *s == '-') && k + 1 < sizeof w)
    w[k++] = *s++;
  w[k] = '\0';
  squash (w, out, n);
}

static int maker_matches (const lens_db *db, size_t maker, const char *make)
{
  const char *m;
  char a[64], b[64];
  first_word (make, a, sizeof a);
  for (m = STR (db, maker); m; m = next_name (m))
    {
      if (same_name (m, make))
        return 1;
      first_word (m, b, sizeof b);
      if (a[0] && !strcmp (a, b))
        return 1;
    }
  return 0;
}

int lens_db_camera (const lens_db *db, const char *make, const char *model, const char **mount, double *crop)
{
  int i;
  char word[64], rest[128];
  *mount = "";
  *crop = 1;
  if (!db || !model || !model[0])
    return -1;
  /* the model without its maker's first word ("NIKON D750" -> "D750") */
  first_word (make ? make : "", word, sizeof word);
  squash (model, rest, sizeof rest);
  if (word[0] && !strncmp (rest, word, strlen (word)))
    memmove (rest, rest + strlen (word), strlen (rest + strlen (word)) + 1);
  for (i = 0; i < db->nc; i++)
    {
      const char *m;
      if (make && make[0] && !maker_matches (db, db->C[i].maker, make))
        continue;
      for (m = STR (db, db->C[i].model); m; m = next_name (m))
        {
          char sq[128];
          squash (m, sq, sizeof sq);
          if (same_name (m, model) || (rest[0] && !strcmp (sq, rest)))
            {
              *mount = STR (db, db->C[i].mount);
              *crop = db->C[i].crop;
              return i;
            }
        }
    }
  return -1;
}

int lens_db_photo_camera (const lens_db *db, const lens_photo *ph)
{
  const char *mount;
  double crop;
  if (!db)
    return -1;
  if (ph->camera >= 0 && ph->camera < db->nc)
    return ph->camera;
  return lens_db_camera (db, ph->make, ph->model, &mount, &crop);
}

static const mount_entry *find_mount (const lens_db *db, const char *name)
{
  int i;
  for (i = 0; i < db->nm; i++)
    if (!strcmp (STR (db, db->M[i].name), name))
      return &db->M[i];
  return NULL;
}

int lens_db_fits (const lens_db *db, int i, const char *mount)
{
  const char *lm;
  const mount_entry *m;
  if (!mount || !mount[0])
    return 1;
  if (i < 0 || i >= db->nl)
    return 0;
  m = find_mount (db, mount);
  for (lm = STR (db, db->L[i].mounts); lm; lm = next_name (lm))
    {
      const char *c;
      if (!strcmp (lm, mount))
        return 1;
      if (m && m->compat)
        for (c = STR (db, m->compat); c && *c; c = next_name (c))
          if (strcmp (c, "Generic") && !strcmp (c, lm))
            return 1;
    }
  return 0;
}

/* A camera with a lens of its own (its mount is no system's). */
static int fixed_mount (const lens_db *db, const char *mount)
{
  return mount && mount[0] && !find_mount (db, mount);
}

/* Words and numbers: letters and digits apart ("EF24-105mm" -> ef 24
   105), numbers without ".0", and "mm" and "f" left out. */
#define TOKENS 32
static int tokens (const char *s, char t[TOKENS][16])
{
  int n = 0;
  while (*s && n < TOKENS)
    {
      size_t k = 0;
      if (isdigit ((unsigned char) *s))
        {
          while ((isdigit ((unsigned char) *s) || (*s == '.' && isdigit ((unsigned char) s[1]))) && k + 1 < 16)
            t[n][k++] = *s++;
          t[n][k] = '\0';
          if (strchr (t[n], '.'))
            {
              while (k && t[n][k - 1] == '0')
                t[n][--k] = '\0';
              if (k && t[n][k - 1] == '.')
                t[n][--k] = '\0';
            }
          n++;
        }
      else if (isalpha ((unsigned char) *s))
        {
          while (isalpha ((unsigned char) *s) && k + 1 < 16)
            t[n][k++] = (char) tolower ((unsigned char) *s++);
          t[n][k] = '\0';
          if (strcmp (t[n], "mm") && strcmp (t[n], "f"))
            n++;
        }
      else
        s++;
    }
  return n;
}

double lens_name_score (const char *a, const char *b)
{
  char ta[TOKENS][16], tb[TOKENS][16];
  int na = tokens (a, ta), nb = tokens (b, tb), i, j, common = 0;
  char used[TOKENS] = { 0 };
  double dice;
  if (!na || !nb)
    return 0;
  for (i = 0; i < na; i++)
    for (j = 0; j < nb; j++)
      if (!used[j] && !strcmp (ta[i], tb[j]))
        {
          used[j] = 1;
          common++;
          break;
        }
  dice = 2.0 * common / (na + nb);
  /* A short name all of which is in the other ("17-50mm F2.8", as some
     makers' lenses give it) is as good as most of it. */
  if (common == na && na >= 3 && dice < 0.7)
    dice = 0.7;
  return dice;
}

static int near (double a, double b, double tol)
{
  return a > 0 && b > 0 && fabs (a - b) <= tol * (a > b ? a : b);
}

/* How likely the photo is to have been taken with lens i (0..1 and a bit). */
static double score (const lens_db *db, int i, const lens_photo *ph, const char *mount, const char *cam_maker, double ccrop)
{
  const lens_entry *e = &db->L[i];
  double s = 0, fmin, fmax, amin;
  const char *m;
  int named = ph->lens[0] && !same_name (ph->lens, "fixed lens");
  if (fixed_mount (db, mount))
    return lens_db_fits (db, i, mount) ? 1 : 0;
  if (named)
    {
      for (m = STR (db, e->model); m; m = next_name (m))
        {
          double v = lens_name_score (ph->lens, m);
          if (v > s)
            s = v;
        }
      /* Numbers that differ ("17-55mm" against "17-50mm") count against it. */
      name_range (ph->lens, &fmin, &fmax, &amin);
      if (fmin > 0 && e->fmin > 0 && (!near (fmin, e->fmin, 0.03) || !near (fmax, e->fmax, 0.03)))
        s *= 0.3;
      if (amin > 0 && e->amin > 0 && !near (amin, e->amin, 0.06))
        s *= 0.6;
    }
  else if (ph->fmin > 0 && e->fmin > 0)
    {
      if (near (ph->fmin, e->fmin, 0.03) && near (ph->fmax > 0 ? ph->fmax : ph->fmin, e->fmax, 0.03))
        s = ph->amin > 0 && e->amin > 0 ? (near (ph->amin, e->amin, 0.06) ? 0.75 : 0.3) : 0.65;
    }
  else
    s = 0.2;
  if (ph->focal > 0 && e->fmin > 0 && (ph->focal < e->fmin * 0.97 || ph->focal > e->fmax * 1.03))
    s *= 0.3;
  if (cam_maker && maker_matches (db, e->maker, cam_maker))
    s += 0.02;
  /* The same lens measured on more than one camera: the nearest in size. */
  if (s > 0 && ccrop > 0)
    s -= 0.005 * fabs (log (e->crop / ccrop));
  return s;
}

/* The camera's mount and maker (NULL if not known). */
static int photo_mount (const lens_db *db, const lens_photo *ph, const char **mount, const char **maker, double *crop)
{
  int c = lens_db_photo_camera (db, ph);
  *crop = c >= 0 ? db->C[c].crop : 0;
  *mount = c >= 0 ? STR (db, db->C[c].mount) : NULL;
  *maker = c >= 0 ? STR (db, db->C[c].maker) : (ph->make[0] ? ph->make : NULL);
  return c;
}

int lens_db_match (const lens_db *db, const lens_photo *ph)
{
  const char *mount, *maker;
  int i, best = -1, other = -1;
  double bs = 0, second = 0, ccrop;
  if (!db)
    return -1;
  photo_mount (db, ph, &mount, &maker, &ccrop);
  for (i = 0; i < db->nl; i++)
    if (lens_db_fits (db, i, mount))
      {
        double s = score (db, i, ph, mount, maker, ccrop);
        if (s > bs)
          {
            if (best >= 0 && strcmp (STR (db, db->L[best].model), STR (db, db->L[i].model)))
              {
                second = bs;
                other = best;
              }
            bs = s;
            best = i;
          }
        else if (s > second && best >= 0 && strcmp (STR (db, db->L[best].model), STR (db, db->L[i].model)))
          {
            second = s;
            other = i;
          }
      }
  if (bs < 0.6)
    return -1;
  /* Two lenses as likely as each other: it could be either, unless
     they're the same maker's (versions of one lens, much alike). */
  if (other >= 0 && bs - second < 0.015 && strcmp (STR (db, db->L[best].maker), STR (db, db->L[other].maker)))
    return -1;
  return best;
}

typedef struct { int i; double s; } ranked;

static int by_score (const void *a, const void *b)
{
  const ranked *x = a, *y = b;
  return x->s < y->s ? 1 : x->s > y->s ? -1 : x->i - y->i;
}

int lens_db_suggest (const lens_db *db, const lens_photo *ph, int *out, int n)
{
  const char *mount, *maker;
  ranked *r;
  int i, k = 0, m, j;
  double ccrop;
  if (!db || n <= 0 || !(r = malloc ((size_t) db->nl * sizeof *r)))
    return 0;
  photo_mount (db, ph, &mount, &maker, &ccrop);
  for (i = 0; i < db->nl; i++)
    if (lens_db_fits (db, i, mount))
      {
        double s = score (db, i, ph, mount, maker, ccrop);
        if (s >= 0.15)
          {
            r[k].i = i;
            r[k].s = s;
            k++;
          }
      }
  qsort (r, (size_t) k, sizeof *r, by_score);
  /* each name once (the likeliest of a lens measured more than once) */
  for (i = m = 0; i < k && m < n; i++)
    {
      for (j = 0; j < m; j++)
        if (!strcmp (STR (db, db->L[out[j]].model), STR (db, db->L[r[i].i].model)))
          break;
      if (j == m)
        out[m++] = r[i].i;
    }
  free (r);
  return m;
}

int lens_db_similar (const lens_db *db, int lens, double crop, const char *make, int *out, int n)
{
  ranked *r;
  int i, k = 0, m;
  if (!db || n <= 0 || !(r = malloc ((size_t) (db->nc ? db->nc : 1) * sizeof *r)))
    return 0;
  if (crop <= 0 && lens >= 0 && lens < db->nl)
    crop = db->L[lens].crop;           /* not known: like the camera it was measured on */
  for (i = 0; i < db->nc; i++)
    {
      const char *mount = STR (db, db->C[i].mount), *lm;
      double s = 0;
      if (lens >= 0 && !lens_db_fits (db, lens, mount))
        continue;
      if (crop > 0)
        s -= 10 * fabs (log (db->C[i].crop / crop));   /* the size first */
      /* then the lens's own mount (not with an adapter), and the photo's maker */
      for (lm = lens >= 0 ? STR (db, db->L[lens].mounts) : NULL; lm; lm = next_name (lm))
        if (!strcmp (lm, mount))
          s += 0.5;
      if (make && make[0] && maker_matches (db, db->C[i].maker, make))
        s += 0.3;
      r[k].i = i;
      r[k].s = s;
      k++;
    }
  qsort (r, (size_t) k, sizeof *r, by_score);
  m = k < n ? k : n;
  for (i = 0; i < m; i++)
    out[i] = r[i].i;
  free (r);
  return m;
}

/* --- profiles */

#define MEAS 1024
typedef struct { float f, a; int model; float v[6]; } meas;

static int by_focal (const void *a, const void *b)
{
  const meas *x = a, *y = b;
  if (x->f != y->f)
    return x->f < y->f ? -1 : 1;
  return x->a < y->a ? -1 : x->a > y->a ? 1 : 0;
}

/* Between the two measured nearest f (models alike) or the nearest. */
static const meas *between (const meas *m, int n, double f, float *v, int nv)
{
  int i, k;
  if (!n)
    return NULL;
  if (f <= 0 || f <= m[0].f)
    {
      memcpy (v, m[0].v, (size_t) nv * sizeof *v);
      return &m[0];
    }
  if (f >= m[n - 1].f)
    {
      memcpy (v, m[n - 1].v, (size_t) nv * sizeof *v);
      return &m[n - 1];
    }
  for (i = 0; i + 1 < n && m[i + 1].f < f; i++)
    ;
  if (m[i].model != m[i + 1].model || m[i + 1].f == m[i].f)
    {
      const meas *c = f - m[i].f < m[i + 1].f - f ? &m[i] : &m[i + 1];
      memcpy (v, c->v, (size_t) nv * sizeof *v);
      return c;
    }
  {
    double t = (f - m[i].f) / (m[i + 1].f - m[i].f);
    for (k = 0; k < nv; k++)
      v[k] = (float) (m[i].v[k] + (m[i + 1].v[k] - m[i].v[k]) * t);
  }
  return &m[i];
}

/* Vignetting at one measured focal length (from first, count of them):
   between the two apertures nearest a, in 1/a. */
static void vig_at (const meas *m, int count, double a, float *v)
{
  int i, k;
  if (a <= 0)
    a = m[count / 2].a;                /* not known: one from the middle */
  if (a <= m[0].a || count == 1)
    {
      memcpy (v, m[0].v, 3 * sizeof *v);
      return;
    }
  if (a >= m[count - 1].a)
    {
      memcpy (v, m[count - 1].v, 3 * sizeof *v);
      return;
    }
  for (i = 0; i + 1 < count && m[i + 1].a < a; i++)
    ;
  {
    double t = (1 / a - 1 / m[i].a) / (1 / m[i + 1].a - 1 / m[i].a);
    for (k = 0; k < 3; k++)
      v[k] = (float) (m[i].v[k] + (m[i + 1].v[k] - m[i].v[k]) * t);
  }
}

static void vig_profile (const meas *m, int n, double f, double a, float *v)
{
  int i = 0, j, lo = -1, hi = -1;
  /* runs of the same focal length */
  while (i < n)
    {
      for (j = i; j < n && m[j].f == m[i].f; j++)
        ;
      if (f > 0 && m[i].f <= f)
        lo = i;
      if (hi < 0 && f > 0 && m[i].f >= f)
        hi = i;
      if (f <= 0 && lo < 0)
        lo = i;
      i = j;
    }
  if (lo < 0)
    lo = 0;
  if (hi < 0)
    hi = lo;
  {
    float a1[3], a2[3];
    int c1 = 0, c2 = 0, k;
    while (lo + c1 < n && m[lo + c1].f == m[lo].f)
      c1++;
    while (hi + c2 < n && m[hi + c2].f == m[hi].f)
      c2++;
    vig_at (m + lo, c1, a, a1);
    vig_at (m + hi, c2, a, a2);
    if (m[hi].f == m[lo].f)
      memcpy (v, a1, sizeof a1);
    else
      {
        double t = (f - m[lo].f) / (m[hi].f - m[lo].f);
        for (k = 0; k < 3; k++)
          v[k] = (float) (a1[k] + (a2[k] - a1[k]) * t);
      }
  }
}

int lens_db_profile (const lens_db *db, int i, const lens_photo *ph, recipe_lens *out)
{
  static meas D[MEAS], T[MEAS], V[MEAS];
  int nd = 0, nt = 0, nv = 0, cam;
  char line[512];
  const lens_entry *e;
  double ccrop;
  FILE *f;
  memset (out, 0, sizeof *out);
  if (!db || i < 0 || i >= db->nl || !(f = fopen (db->path, "r")))
    return -1;
  e = &db->L[i];
  if (fseek (f, e->at, SEEK_SET) != 0)
    {
      fclose (f);
      return -1;
    }
  while (fgets (line, sizeof line, f) && (line[0] == 'D' || line[0] == 'T' || line[0] == 'V') && line[1] == ' ')
    {
      char *p = line + 2, *end, model;
      meas mm;
      int k;
      memset (&mm, 0, sizeof mm);
      mm.f = strtof (p, &end);
      p = end;
      if (line[0] == 'V')
        {
          mm.a = strtof (p, &end);
          p = end;
          for (k = 0; k < 3; k++)
            {
              mm.v[k] = strtof (p, &end);
              p = end;
            }
          if (nv < MEAS)
            V[nv++] = mm;
          continue;
        }
      while (*p == ' ')
        p++;
      model = *p++;
      for (k = 0; k < 6; k++)
        {
          mm.v[k] = strtof (p, &end);
          if (end == p)
            break;
          p = end;
        }
      if (line[0] == 'D')
        {
          mm.model = model == 'p' ? LENS_D_PTLENS : model == '3' ? LENS_D_POLY3 : model == '5' ? LENS_D_POLY5 : 0;
          if (mm.model && nd < MEAS)
            D[nd++] = mm;
        }
      else
        {
          mm.model = model == 'l' ? LENS_T_LINEAR : model == '3' ? LENS_T_POLY3 : 0;
          if (mm.model && nt < MEAS)
            T[nt++] = mm;
        }
    }
  fclose (f);
  qsort (D, (size_t) nd, sizeof *D, by_focal);
  qsort (T, (size_t) nt, sizeof *T, by_focal);
  qsort (V, (size_t) nv, sizeof *V, by_focal);
  out->on = 1;
  snprintf (out->name, sizeof out->name, "%s", STR (db, e->model));
  out->type = e->type;
  out->focal = (float) ph->focal;
  out->aperture = (float) ph->fnumber;
  {
    const meas *c = between (D, nd, ph->focal, out->d, 3);
    if (c)
      out->dist = c->model;
    c = between (T, nt, ph->focal, out->t, 6);
    if (c)
      out->tca = c->model;
    if (nv)
      {
        vig_profile (V, nv, ph->focal, ph->fnumber, out->k);
        out->vig = 1;
      }
  }
  /* The photo's corner, where it was on the camera measured with. */
  cam = lens_db_photo_camera (db, ph);
  ccrop = cam >= 0 ? db->C[cam].crop : e->crop;
  out->rv = (float) (e->crop / ccrop);
  out->rh = (float) (e->crop / ccrop * sqrt (1 + (double) e->aspect * e->aspect));
  if (ph->camera >= 0 && ph->camera < db->nc)
    {
      const char *mk = STR (db, db->C[ph->camera].maker), *md = STR (db, db->C[ph->camera].model);
      char w[64], m[64];
      first_word (mk, w, sizeof w);
      first_word (md, m, sizeof m);
      if (!strcmp (w, m))
        snprintf (out->camera, sizeof out->camera, "%s", md);
      else
        snprintf (out->camera, sizeof out->camera, "%.20s %.40s", mk, md);
    }
  return 0;
}

/* --- the photo */

#define HEAD 262144
int lens_photo_read (lens_photo *ph, const char *path)
{
  unsigned char *buf = malloc (HEAD);
  FILE *f = path && path[0] ? fopen (path, "rb") : NULL;
  size_t got;
  img_info in;
  int fmt;
  if (!buf || !f)
    {
      free (buf);
      if (f)
        fclose (f);
      return -1;
    }
  got = fread (buf, 1, HEAD, f);
  fclose (f);
  memset (&in, 0, sizeof in);
  fmt = img_probe_named (buf, got, path);
  if (fmt == IMG_RAW)
    img_read_raw_info (buf, got, &in);
  else if (fmt == IMG_JPEG || fmt == IMG_PNG || fmt == IMG_TIFF)
    img_read_info (buf, got, &in);
  free (buf);
  if (in.make[0] && !ph->make[0])
    snprintf (ph->make, sizeof ph->make, "%s", in.make);
  if (in.model[0] && !ph->model[0])
    snprintf (ph->model, sizeof ph->model, "%s", in.model);
  if (in.lens[0])
    snprintf (ph->lens, sizeof ph->lens, "%s", in.lens);
  if (in.focal > 0 && ph->focal <= 0)
    ph->focal = in.focal;
  if (in.fnumber > 0 && ph->fnumber <= 0)
    ph->fnumber = in.fnumber;
  if (in.focal35 > 0 && in.focal > 0)
    ph->crop = in.focal35 / in.focal;
  if (in.lens_fmin > 0)
    {
      ph->fmin = in.lens_fmin;
      ph->fmax = in.lens_fmax;
      ph->amin = in.lens_amin;
    }
  return 0;
}
