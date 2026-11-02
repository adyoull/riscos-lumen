/* xmp.c: see xmp.h. */
#include <stdio.h>
#include <string.h>
#include "xmp.h"
#include "catalog.h"

typedef struct { char *out; size_t cap, len; int full; } sink;

static void put (sink *s, const char *t)
{
  size_t n = strlen (t);
  if (s->len + n + 1 > s->cap)
    {
      s->full = 1;
      return;
    }
  memcpy (s->out + s->len, t, n + 1);
  s->len += n;
}

/* Text with XML's special characters as entities. */
static void put_text (sink *s, const char *t)
{
  char one[2] = { 0, 0 };
  for (; *t; t++)
    switch (*t)
      {
      case '&': put (s, "&amp;"); break;
      case '<': put (s, "&lt;"); break;
      case '>': put (s, "&gt;"); break;
      case '"': put (s, "&quot;"); break;
      default:
        if ((unsigned char) *t >= ' ')
          {
            one[0] = *t;
            put (s, one);
          }
      }
}

static void put_keyword (void *ctx, const char *kw)
{
  sink *s = ctx;
  put (s, "     <rdf:li>");
  put_text (s, kw);
  put (s, "</rdf:li>\n");
}

size_t xmp_packet (const char *caption, const char *keywords, char *out, size_t cap)
{
  sink s = { out, cap, 0, 0 };
  int has_kw = 0;
  const char *k;
  for (k = keywords ? keywords : ""; *k; k++)
    if (*k != ' ' && *k != ',' && *k != ';')
      has_kw = 1;
  if ((!caption || !*caption) && !has_kw)
    return 0;
  if (cap)
    out[0] = '\0';
  put (&s, "<?xpacket begin=\"\xEF\xBB\xBF\" id=\"W5M0MpCehiHzreSzNTczkc9d\"?>\n"
           "<x:xmpmeta xmlns:x=\"adobe:ns:meta/\">\n"
           " <rdf:RDF xmlns:rdf=\"http://www.w3.org/1999/02/22-rdf-syntax-ns#\">\n"
           "  <rdf:Description rdf:about=\"\" xmlns:dc=\"http://purl.org/dc/elements/1.1/\">\n");
  if (caption && *caption)
    {
      put (&s, "   <dc:description>\n    <rdf:Alt>\n     <rdf:li xml:lang=\"x-default\">");
      put_text (&s, caption);
      put (&s, "</rdf:li>\n    </rdf:Alt>\n   </dc:description>\n");
    }
  if (has_kw)
    {
      put (&s, "   <dc:subject>\n    <rdf:Bag>\n");
      cat_keyword_split (keywords, put_keyword, &s);
      put (&s, "    </rdf:Bag>\n   </dc:subject>\n");
    }
  put (&s, "  </rdf:Description>\n </rdf:RDF>\n</x:xmpmeta>\n<?xpacket end=\"w\"?>");
  return s.full ? 0 : s.len;
}

/* The first occurrence of text in [p, end), or NULL. */
static const char *find (const char *p, const char *end, const char *text)
{
  size_t n = strlen (text);
  for (; p && p + n <= end; p++)
    if (*p == *text && !memcmp (p, text, n))
      return p;
  return NULL;
}

/* Copies XML text [p, end) into out, entities decoded, white space tidied. */
static void take_text (const char *p, const char *end, char *out, size_t n)
{
  size_t len = 0;
  int space = 0;
  while (p < end && n)
    {
      char ch = *p++;
      if (ch == '&')
        {
          static const struct { const char *e; char c; } ents[] = {
            { "amp;", '&' }, { "lt;", '<' }, { "gt;", '>' }, { "quot;", '"' }, { "apos;", '\'' } };
          size_t k;
          for (k = 0; k < sizeof ents / sizeof ents[0]; k++)
            if ((size_t) (end - p) >= strlen (ents[k].e) && !memcmp (p, ents[k].e, strlen (ents[k].e)))
              {
                ch = ents[k].c;
                p += strlen (ents[k].e);
                break;
              }
        }
      if (ch == '\n' || ch == '\r' || ch == '\t' || ch == ' ')
        {
          space = len > 0;
          continue;
        }
      if (space && len + 1 < n)
        out[len++] = ' ';
      space = 0;
      if (len + 1 < n)
        out[len++] = ch;
    }
  if (n)
    out[len < n ? len : n - 1] = '\0';
}

/* The rdf:li items inside the element named tag (e.g. "dc:subject"): calls
   back with each one's text. */
static int each_item (const char *p, const char *end, const char *tag,
                      int (*fn) (void *ctx, const char *text), void *ctx)
{
  char open[40], close[40];
  const char *a, *b;
  snprintf (open, sizeof open, "<%s", tag);
  snprintf (close, sizeof close, "</%s>", tag);
  a = find (p, end, open);
  b = a ? find (a, end, close) : NULL;
  if (!a || !b)
    return 0;
  for (p = a; (p = find (p, b, "<rdf:li")) != NULL;)
    {
      const char *gt = find (p, b, ">"), *stop;
      char text[256];
      if (!gt)
        break;
      if (gt[-1] == '/')               /* <rdf:li/> */
        {
          p = gt + 1;
          continue;
        }
      stop = find (gt + 1, b, "</rdf:li>");
      if (!stop)
        break;
      take_text (gt + 1, stop, text, sizeof text);
      if (text[0] && fn (ctx, text))
        return 1;
      p = stop + 9;
    }
  return 0;
}

typedef struct { char *out; size_t n; } dest;

static int first_text (void *ctx, const char *text)
{
  dest *d = ctx;
  snprintf (d->out, d->n, "%s", text);
  return 1;
}

static int add_keyword (void *ctx, const char *text)
{
  dest *d = ctx;
  size_t len = strlen (d->out);
  char kw[64];
  char *c;
  /* A keyword with a comma in it would split in two: the comma goes. */
  snprintf (kw, sizeof kw, "%s", text);
  for (c = kw; *c; c++)
    if (*c == ',' || *c == ';')
      *c = ' ';
  if (len + strlen (kw) + 3 < d->n)
    snprintf (d->out + len, d->n - len, "%s%s", len ? ", " : "", kw);
  return 0;
}

int xmp_read (const unsigned char *buf, size_t len, char *caption, size_t cn, char *keywords, size_t kn)
{
  const char *p = (const char *) buf, *end = p + len, *a, *b;
  dest d;
  if (cn)
    caption[0] = '\0';
  if (kn)
    keywords[0] = '\0';
  a = find (p, end, "<x:xmpmeta");
  b = a ? find (a, end, "</x:xmpmeta>") : NULL;
  if (!a || !b)
    return 0;
  d.out = caption;
  d.n = cn;
  /* dc:description may also be an attribute-less simple element in old files. */
  if (!each_item (a, b, "dc:description", first_text, &d) && cn)
    {
      const char *s = find (a, b, "<dc:description>"), *e = s ? find (s, b, "</dc:description>") : NULL;
      if (s && e && !find (s, e, "<rdf:"))
        take_text (s + 16, e, caption, cn);
    }
  d.out = keywords;
  d.n = kn;
  each_item (a, b, "dc:subject", add_keyword, &d);
  return 1;
}
