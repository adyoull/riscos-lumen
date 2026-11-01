/* Lumen's desktop front end: the declarations its parts share.
   Block layouts are from the PRMs (2026 Reader Edition); page numbers are
   given where a layout is used. */
#ifndef LUMEN_UI_H
#define LUMEN_UI_H
#include "catalog.h"
#include "library.h"
#include "recipe.h"

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
#define KEY_RETURN 0x0D

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
void draw_triangle (int x0, int y0, int x1, int y1, int x2, int y2, unsigned rgb);
void draw_text (int x, int y, const char *s, unsigned fg, unsigned bg);
int draw_text_width (const char *s);
/* A 32bpp sprite (see thumbcache.c) scaled to fit a box, centred across
   and sitting on its bottom edge. out gets the rectangle plotted. */
void draw_sprite_fit (const void *area, const void *sprite, int w, int h,
                      int bx0, int by0, int bx1, int by1, int out[4]);
/* Any shape of box: fitted inside, centred both ways. */
void draw_sprite_rect (const void *area, const void *sprite, int w, int h,
                       int bx0, int by0, int bx1, int by1, int out[4]);
/* Both plot a smoothed copy at the size shown (made on first use, kept
   until draw_forget). Off: OS_SpriteOp's own (pixel-dropping) scaling,
   for pictures that change on every frame, such as while dragging. */
void draw_set_smoothing (int on);
/* One pixel a pixel, bottom left at (x, y). */
void draw_sprite_at (const void *area, const void *sprite, int x, int y);
void draw_sprite_masked (const void *area, const void *sprite, int x, int y);   /* using its mask */
void draw_circle (int cx, int cy, int r, unsigned rgb);
void draw_line (int x0, int y0, int x1, int y1, unsigned rgb);                          /* an outline */
/* A sprite's pixels changed, or it is about to be freed. */
void draw_forget (const void *sprite);

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
void browser_title (char *out, size_t n);
void browser_status (char *out, size_t n);
void browser_place (int *block);       /* inside the main window */
void browser_unplace (void);
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
int browser_item (int i, cat_item *out);   /* 1 if i is in the list */
int browser_focus (void);
void browser_focus_on (int i);         /* select and show item i, alone */

/* --- viewer.c: one photo and the adjustments panel */
int viewer_create (void);
int viewer_window (void);
int viewer_is_open (void);
void viewer_open (int index);          /* the browser's item index */
void viewer_show (int index);
void viewer_open_window (int *block);
void viewer_place (int *block);        /* inside the main window */
void viewer_unplace (void);
void viewer_close (void);              /* back to the Browser */
void viewer_shutdown (void);           /* the main window closed */
int viewer_has_photo (void);
int viewer_index (void);               /* the browser index shown, -1 */
void viewer_title (char *out, size_t n);
void viewer_panel_draw (int ox, int oy);   /* the Adjustments tab */
void viewer_panel_click (int *block);
int viewer_panel_height (void);
void viewer_redraw (int *block);
void viewer_click (int *block);
void viewer_drag_end (void);
int viewer_key (int key);
int viewer_busy (void);
void viewer_idle (void);
void viewer_undo (void);
void viewer_redo (void);
void viewer_reset_all (void);
void viewer_toggle_before (void);
int viewer_can_undo (void);
int viewer_can_redo (void);
int viewer_before (void);
void viewer_mode_changed (void);
int viewer_proxy_size (int *w, int *h);
const recipe *viewer_recipe (void);
int viewer_shown_size (int *w, int *h);   /* after geometry */
void viewer_button_centre (int k, int *x, int *y);   /* tests */
int viewer_curve_rect (int r[4]);                     /* tests */
cat_id viewer_item (void);             /* the photo shown, 0 if closed */
void viewer_reload (cat_id version);   /* its recipe changed from outside */
void viewer_apply_look (const char *look);
void viewer_look (char *out, size_t n);
void viewer_auto (void);
void viewer_toggle_zoom (void);        /* fit, or 100% of the original */
int viewer_zoomed (void);
void viewer_toggle_loupe (void);       /* the loupe on or off */
int viewer_loupe (void);               /* 0 off, else its magnification */
void viewer_set_loupe_mag (int mag);   /* 1, 2 or 4 */              /* 0 fit, 1 loading the full size, 2 at 100% */
void viewer_set (int p, float v);      /* one adjustment, saved */
void viewer_pick_white (void);         /* the white balance eyedropper, on or off */
int viewer_picking (void);
void viewer_rotate (int clockwise);
void viewer_crop_start (void);
void viewer_crop_finish (int apply);
void viewer_toggle_crop (void);
int viewer_crop_mode (void);
void viewer_set_crop_shape (int k);    /* Free, Original, Square, 3:2, 4:3, 16:9 */
int viewer_crop_shape (void);
const char *viewer_shape_name (int k);
void viewer_set_grid (int k);          /* None, Thirds, Quarters, Fine */
int viewer_grid (void);
const char *viewer_grid_name (int k);

/* --- mainwin.c: the one main window, Aperture's layout */
enum { LAYOUT_BROWSER, LAYOUT_SPLIT, LAYOUT_VIEWER };
enum { TAB_LIBRARY, TAB_INFO, TAB_ADJUST };
int mainwin_create (void);
int mainwin_window (void);
int mainwin_inspector (void);
void mainwin_open (int *block);        /* Open_Window_Request, or NULL */
void mainwin_open_child (int *block);  /* the inspector's own */
void mainwin_close (void);
int mainwin_is_open (void);
void mainwin_redraw (int *block);
void mainwin_inspector_redraw (int *block);
void mainwin_click (int *block);
void mainwin_inspector_click (int *block);
int mainwin_key (int key);
void mainwin_mode_changed (void);
int mainwin_mode (void);
void mainwin_set_mode (int m);
void mainwin_cycle_mode (void);
int mainwin_tab (void);
void mainwin_set_tab (int t);
int mainwin_inspector_on (void);
void mainwin_toggle_inspector (void);
void mainwin_toggle_full_screen (void);
int mainwin_full_screen (void);
void mainwin_update_title (void);
void mainwin_set_title (const char *t);
const char *mainwin_title (void);
void mainwin_inspector_changed (void);
void mainwin_inspector_extent (int h);
void mainwin_force_inspector (int x0, int y0, int x1, int y1);
void mainwin_library_changed (void);
void mainwin_info_changed (void);
void mainwin_status_changed (void);   /* the bottom bar: counts */
int mainwin_search_icon (void);        /* the search field's icon handle, -1 */
int mainwin_search_key (int key);      /* a key in the search field; 1 if used */
void mainwin_set_search (const char *text);

/* --- looks.c: copying adjustments between photos, and presets */
void looks_copy_viewer (void);         /* the viewer's look */
void looks_copy_selection (void);      /* the first selected photo's */
const char *looks_clip (void);         /* NULL if nothing copied */
/* Gives the selected photos a look (the viewer follows if it shows one). */
void looks_apply_selection (const char *look);
/* The presets, read again (freed by the next call). */
int looks_presets (cat_preset **out);
int looks_save_preset (const char *name);   /* the viewer's look */
void looks_delete_preset (cat_id id);

/* --- exportbox.c: the Export save box and the queue of exports */
int exportbox_create (void);
int exportbox_window (void);
void exportbox_open (int from_viewer, int x, int y);   /* for the selection, or the viewer's photo */
void exportbox_click (int *block);
int exportbox_key (int key);
int exportbox_dragging (void);
void exportbox_drag_end (void);
void exportbox_save_ack (int *message);
int exportbox_save_to (const char *path);
void exportbox_set_name (const char *s);   /* tests */
const char *exportbox_name (void);
int exports_busy (void);
void exports_idle (unsigned deadline);
void exports_status (char *out, size_t n);

/* --- menus.c */
void menus_init (void);
void menus_open_window_menu (int x, int y);
void menus_open_iconbar_menu (int x);
void menus_open_viewer_menu (int x, int y);
void menus_open_presets_menu (int x, int y);
void menus_open_new_menu (int x, int y);
void menus_open_rating_menu (int x, int y);
int menus_selection (const int *choice);   /* 1: quit */

#endif
