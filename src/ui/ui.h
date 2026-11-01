/* Lumen's desktop front end: the declarations its parts share.
   Block layouts are from the PRMs (2026 Reader Edition); page numbers are
   given where a layout is used. */
#ifndef LUMEN_UI_H
#define LUMEN_UI_H
#include "catalog.h"
#include "library.h"

#define APP_NAME "Lumen"
#ifndef LUMEN_VERSION
#define LUMEN_VERSION "0.0.0"
#define LUMEN_DATE "-"
#endif

/* Wimp_Poll reason codes. */
enum
{
  Null_Reason = 0, Redraw_Window_Request = 1, Open_Window_Request = 2,
  Close_Window_Request = 3, Pointer_Leaving = 4, Pointer_Entering = 5,
  Mouse_Click = 6, User_Drag_Box = 7, Key_Pressed = 8, Menu_Selection = 9,
  Scroll_Request = 10, Lose_Caret = 11, Gain_Caret = 12,
  User_Message = 17, User_Message_Recorded = 18, User_Message_Acknowledge = 19
};

/* Messages. */
#define Message_Quit 0
#define Message_DataSave 1
#define Message_DataLoad 3
#define Message_DataLoadAck 4
#define Message_DataOpen 5
#define Message_ModeChange 0x400C1
#define Message_HelpRequest 0x502
#define Message_HelpReply 0x503

enum { Button_Adjust = 1, Button_Menu = 2, Button_Select = 4 };
#define TASK_WORD 0x4B534154          /* "TASK" */

/* Key codes (Wimp_Poll 8, +24). */
#define KEY_LEFT 0x18C
#define KEY_RIGHT 0x18D
#define KEY_DOWN 0x18E
#define KEY_UP 0x18F
#define KEY_PAGEDOWN 0x19E
#define KEY_PAGEUP 0x19F
#define KEY_HOME 0x1E
#define KEY_END 0x1AB
#define KEY_ESCAPE 0x1B
#define KEY_CTRL_A 0x01

/* --- app.c: the task, its icon bar icon, the poll dispatch */
int app_init (const char *library_path);
/* Handles one Wimp_Poll event. Returns 1 to quit. */
int app_event (int reason, int *block, int sender);
/* Background work waiting (null events wanted). */
int app_busy (void);
void app_quit (void);
void app_report (const char *msg);
unsigned app_now (void);               /* centiseconds */
library *app_library (void);
int app_task (void);

/* --- draw.c: plotting inside a redraw loop */
void draw_init_mode (void);            /* read eigen factors etc.: at start and after mode changes */
int draw_xeig (void);
int draw_yeig (void);
void draw_rect (int x0, int y0, int x1, int y1, unsigned rgb);   /* rgb is &BBGGRR00 */
void draw_frame (int x0, int y0, int x1, int y1, unsigned rgb);
void draw_star (int cx, int cy, int r, unsigned rgb);
void draw_text (int x, int y, const char *s, unsigned fg, unsigned bg);
int draw_text_width (const char *s);
/* A 32bpp sprite (see thumbcache.c) scaled to fit a box, centred across
   and sitting on its bottom edge. out gets the rectangle plotted. */
void draw_sprite_fit (const void *area, const void *sprite, int w, int h,
                      int bx0, int by0, int bx1, int by1, int out[4]);

/* --- thumbcache.c: thumbnails as 32bpp sprites, loaded in idle time */
typedef struct
{
  void *area;                          /* a sprite area holding one sprite */
  void *sprite;
  int w, h;
} thumb_sprite;
void thumbcache_init (size_t max_bytes);
/* The sprite, or NULL (then it is queued for loading). */
const thumb_sprite *thumbcache_get (cat_id version, int thumb_ready);
/* Forget a version (its thumbnail changed). */
void thumbcache_drop (cat_id version);
/* Loads queued thumbnails until now() >= deadline. For each one loaded
   calls loaded(version). Returns the number loaded. */
int thumbcache_work (unsigned (*now) (void), unsigned deadline, void (*loaded) (cat_id));
int thumbcache_pending (void);
void thumbcache_clear_queue (void);
/* Builds a sprite from RGB rows: exposed for tests. */
int thumbcache_make_sprite (const unsigned char *rgb, int w, int h, thumb_sprite *out);

/* --- browser.c: the library window */
int browser_create (void);
int browser_window (void);
void browser_open (int *block);        /* Open_Window_Request, or NULL to open centred */
void browser_close (void);
void browser_redraw (int *block);
void browser_click (int *block);
int browser_key (int key);             /* 1 if used */
void browser_reload (void);            /* the list from the catalogue */
void browser_thumb_ready (cat_id version);
void browser_thumb_loaded (cat_id version);
void browser_update_title (void);
void browser_mode_changed (void);

void browser_set_source (const cat_source *src);
const cat_source *browser_source (void);
/* The selected versions (malloc'd; free it). */
int browser_selection (cat_id **ids);
int browser_count (void);
void browser_select_all (int on);
void browser_set_size (int size);      /* 0 small, 1 medium, 2 large */
int browser_size (void);
void browser_after_change (void);      /* ratings etc. changed: reload and redraw */

/* --- menus.c */
void menus_init (void);
void menus_open_window_menu (int x, int y);
void menus_open_iconbar_menu (int x);
int menus_selection (const int *choice);   /* 1: quit */

#endif
