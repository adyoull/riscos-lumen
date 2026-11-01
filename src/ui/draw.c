/* draw.c: plotting helpers for the redraw loop.
   ColourTrans_SetGCOL (PRM p78): R0 &BBGGRR00, R3 flags (0: foreground),
   R4 GCOL action (0: set). OS_Plot (p442): 4 move, 85 triangle fill,
   101 rectangle fill (both absolute, foreground). Wimp_TextOp 0/1/2
   (p1391-1393). OS_SpriteOp 52 (p514) with a translation table from
   ColourTrans_SelectTable (p76-77), which may say none is needed. */
#include <math.h>
#include <stdlib.h>
#include <string.h>
#include <kernel.h>
#include <swis.h>
#include "ui.h"

#define SPRITE_MODE_32BPP 0x301680B5   /* type 6 (32bpp), 90 x 90 dpi */

static int xeig = 1, yeig = 1;
static int plot_flags;                 /* OS_SpriteOp 52's R5: dither in modes of 256 colours or fewer */
static void *table;                    /* translation table for 32bpp sprites, or NULL */
static int table_known;

void draw_init_mode (void)
{
  int v = 1;
  if (_swix (OS_ReadModeVariable, _INR (0, 1) | _OUT (2), -1, 4, &v) == NULL)
    xeig = v;
  if (_swix (OS_ReadModeVariable, _INR (0, 1) | _OUT (2), -1, 5, &v) == NULL)
    yeig = v;
  /* Log2BPP (mode variable 9) 3 or less: 256 colours or fewer. Plotting a
     32bpp photo there without dithering bands badly, so ask SpriteExtend
     to dither (R5 bit 6). */
  plot_flags = 0;
  if (_swix (OS_ReadModeVariable, _INR (0, 1) | _OUT (2), -1, 9, &v) == NULL && v <= 3)
    plot_flags = 1 << 6;
  free (table);
  table = NULL;
  table_known = 0;
}

int draw_xeig (void) { return xeig; }
int draw_yeig (void) { return yeig; }

static void gcol (unsigned rgb)
{
  _swix (ColourTrans_SetGCOL, _IN (0) | _INR (3, 4), rgb, 0, 0);
}

void draw_rect (int x0, int y0, int x1, int y1, unsigned rgb)
{
  gcol (rgb);
  _swix (OS_Plot, _INR (0, 2), 4, x0, y0);
  _swix (OS_Plot, _INR (0, 2), 101, x1, y1);
}

void draw_frame (int x0, int y0, int x1, int y1, unsigned rgb)
{
  int t = 2 << xeig > 4 ? 4 : 2 << xeig;
  draw_rect (x0, y0, x1, y0 + t, rgb);
  draw_rect (x0, y1 - t, x1, y1, rgb);
  draw_rect (x0, y0, x0 + t, y1, rgb);
  draw_rect (x1 - t, y0, x1, y1, rgb);
}

/* A five-pointed star as ten triangles round its centre. */
void draw_star (int cx, int cy, int r, unsigned rgb)
{
  int px[11], py[11], i;
  gcol (rgb);
  for (i = 0; i <= 10; i++)
    {
      double a = M_PI / 2 + i * M_PI / 5;
      int rr = (i & 1) ? r * 2 / 5 : r;
      px[i] = cx + (int) lround (rr * cos (a));
      py[i] = cy + (int) lround (rr * sin (a));
    }
  for (i = 0; i < 10; i++)
    {
      _swix (OS_Plot, _INR (0, 2), 4, px[i], py[i]);
      _swix (OS_Plot, _INR (0, 2), 4, cx, cy);
      _swix (OS_Plot, _INR (0, 2), 85, px[i + 1], py[i + 1]);
    }
}

void draw_text (int x, int y, const char *s, unsigned fg, unsigned bg)
{
  _swix (Wimp_TextOp, _INR (0, 2), 0, fg, bg);
  /* bit 30: baseline as the system font's, so y is the text's bottom. */
  _swix (Wimp_TextOp, _INR (0, 5), 2 | (1u << 30), s, -1, -1, x, y);
}

int draw_text_width (const char *s)
{
  int w = 0;
  if (_swix (Wimp_TextOp, _INR (0, 2) | _OUT (0), 1, s, 0, &w))
    return (int) strlen (s) * 16;
  return w;
}

static void ensure_table (const void *area, const void *sprite)
{
  int size = 0;
  if (table_known)
    return;
  table_known = 1;
  /* R5 bit 0: R1 is a sprite pointer (R0 a sprite area). */
  if (_swix (ColourTrans_SelectTable, _INR (0, 5) | _OUT (4), area, sprite, -1, -1, 0, 1, &size)
      || size <= 0)
    return;                            /* none needed: true colour to true colour */
  table = malloc ((size_t) size);
  if (table && _swix (ColourTrans_SelectTable, _INR (0, 5), area, sprite, -1, -1, table, 1))
    {
      free (table);
      table = NULL;
    }
}

void draw_sprite_rect (const void *area, const void *sprite, int w, int h,
                       int bx0, int by0, int bx1, int by1, int out[4])
{
  /* Fit the sprite inside the box, keeping its shape, centred both ways.
     The scale is mul/div with mul in OS units and div in source pixels. */
  int bw = bx1 - bx0, bh = by1 - by0, mul, div, dw, dh, scale[4];
  out[0] = bx0; out[1] = by0; out[2] = bx1; out[3] = by1;
  if (w <= 0 || h <= 0 || bw <= 0 || bh <= 0)
    return;
  ensure_table (area, sprite);
  if ((long long) bw * h <= (long long) bh * w)
    {
      mul = bw; div = w;
    }
  else
    {
      mul = bh; div = h;
    }
  dw = (int) ((long long) w * mul / div);
  dh = (int) ((long long) h * mul / div);
  scale[0] = mul; scale[1] = mul;
  scale[2] = div << xeig; scale[3] = div << yeig;
  out[0] = bx0 + (bw - dw) / 2;
  out[1] = by0 + (bh - dh) / 2;
  out[2] = out[0] + dw;
  out[3] = out[1] + dh;
  _swix (OS_SpriteOp, _INR (0, 7), 52 + 512, area, sprite, out[0], out[1], plot_flags, scale, table);
}

void draw_sprite_fit (const void *area, const void *sprite, int w, int h,
                      int bx0, int by0, int bx1, int by1, int out[4])
{
  int box = (bx1 - bx0) < (by1 - by0) ? (bx1 - bx0) : (by1 - by0);
  int big = w > h ? w : h;
  int dw, dh, scale[4];
  out[0] = bx0; out[1] = by0; out[2] = bx1; out[3] = by1;
  if (big <= 0 || box <= 0)
    return;
  ensure_table (area, sprite);
  /* Scale factors multiply destination pixels: OS units = pixels << eig. */
  scale[0] = box;
  scale[1] = box;
  scale[2] = big << xeig;
  scale[3] = big << yeig;
  dw = w * box / big;
  dh = h * box / big;
  /* Centred across, sitting on the bottom of the box (on the photo's name). */
  out[0] = bx0 + ((bx1 - bx0) - dw) / 2;
  out[1] = by0;
  out[2] = out[0] + dw;
  out[3] = by0 + dh;
  _swix (OS_SpriteOp, _INR (0, 7), 52 + 512, area, sprite, out[0], out[1], plot_flags, scale, table);
}
