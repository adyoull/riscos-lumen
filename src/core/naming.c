/* Copyright (c) 2026 Andrew Youll
   Released under the MIT licence: see the LICENSE file.
   SPDX-License-Identifier: MIT */
/* naming.c: see naming.h. */
#include <stdio.h>
#include <string.h>
#include "naming.h"

const char *const naming_tokens[] = { "{name}", "{n}", "{nnn}", "{date}", "{year}", "{month}", "{day}", "{time}",
                                      "{project}", "{camera}", "{rating}", NULL };

static void put (char *out, int *k, const char *s)
{
  while (*s && *k < NAMING_MAX - 1)
    {
      unsigned char c = (unsigned char) *s++;
      if (c == '.')
        c = '/';
      else if (c < 33 || c == 127 || c == ':' || c == '$' || c == '&' || c == '@' || c == '^' || c == '%'
               || c == '\\' || c == '#' || c == '*' || c == '"' || c == '|' || c == '<' || c == '>')
        c = '_';
      out[(*k)++] = (char) c;
    }
}

/* Part of an EXIF date "YYYY:MM:DD HH:MM:SS": from, count characters. */
static int date_part (const char *taken, int from, int count, char *out)
{
  int i;
  if (!taken || (int) strlen (taken) < from + count)
    return 0;
  for (i = 0; i < count; i++)
    {
      char c = taken[from + i];
      if (c < '0' || c > '9')
        return 0;
      out[i] = c;
    }
  out[count] = '\0';
  return 1;
}

int naming_expand (const char *pattern, const naming_info *ni, char *out)
{
  int k = 0;
  const char *p = pattern && *pattern ? pattern : "{name}";
  while (*p && k < NAMING_MAX - 1)
    {
      const char *close;
      char tok[16], v[64];
      size_t len;
      if (*p != '{' || !(close = strchr (p, '}')) || (len = (size_t) (close - p - 1)) >= sizeof tok)
        {
          char one[2] = { *p++, '\0' };
          put (out, &k, one);
          continue;
        }
      memcpy (tok, p + 1, len);
      tok[len] = '\0';
      v[0] = '\0';
      if (!strcmp (tok, "name"))
        snprintf (v, sizeof v, "%s", ni->name && *ni->name ? ni->name : "Photo");
      else if (tok[0] == 'n' && strspn (tok, "n") == len)
        snprintf (v, sizeof v, "%0*d", (int) len, ni->n);
      else if (!strcmp (tok, "date"))
        {
          char y[5], m[3], d[3];
          if (date_part (ni->taken, 0, 4, y) && date_part (ni->taken, 5, 2, m) && date_part (ni->taken, 8, 2, d))
            snprintf (v, sizeof v, "%s-%s-%s", y, m, d);
          else
            snprintf (v, sizeof v, "undated");
        }
      else if (!strcmp (tok, "year") || !strcmp (tok, "month") || !strcmp (tok, "day"))
        {
          int from = tok[0] == 'y' ? 0 : tok[0] == 'm' ? 5 : 8, count = tok[0] == 'y' ? 4 : 2;
          if (!date_part (ni->taken, from, count, v))
            snprintf (v, sizeof v, "%s", count == 4 ? "0000" : "00");
        }
      else if (!strcmp (tok, "time"))
        {
          char h[3], m[3], s[3];
          if (date_part (ni->taken, 11, 2, h) && date_part (ni->taken, 14, 2, m) && date_part (ni->taken, 17, 2, s))
            snprintf (v, sizeof v, "%s-%s-%s", h, m, s);
          else
            snprintf (v, sizeof v, "notime");
        }
      else if (!strcmp (tok, "project"))
        snprintf (v, sizeof v, "%s", ni->project && *ni->project ? ni->project : "Photos");
      else if (!strcmp (tok, "camera"))
        snprintf (v, sizeof v, "%s", ni->camera && *ni->camera ? ni->camera : "camera");
      else if (!strcmp (tok, "rating"))
        snprintf (v, sizeof v, "%d", ni->rating < 0 ? 0 : ni->rating > 5 ? 5 : ni->rating);
      else
        {
          char one[2] = { *p++, '\0' };      /* not a token: the bracket as typed */
          put (out, &k, one);
          continue;
        }
      put (out, &k, v);
      p = close + 1;
    }
  out[k] = '\0';
  if (!k)
    {
      snprintf (out, NAMING_MAX, "Photo");
      k = 5;
    }
  return k;
}

int naming_varies (const char *pattern)
{
  static const char *const varying[] = { "{name}", "{n", "{time}", NULL };
  int i;
  if (!pattern || !*pattern)
    return 1;
  for (i = 0; varying[i]; i++)
    if (strstr (pattern, varying[i]))
      return 1;
  return 0;
}
