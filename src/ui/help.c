/* help.c: interactive help. !Help (or any help program) sends
   Message_HelpRequest (&502, PRM p1016) for whatever is under the
   pointer: +20 the pointer's x, y, +28 buttons, +32 window, +36 icon.
   Lumen answers with Message_HelpReply (&503): the text at +20, ending in
   a zero byte; "|M" starts a new line. */
#include <stdio.h>
#include <string.h>
#include <kernel.h>
#include <swis.h>
#include "ui.h"

static const char *const toolbar[9] = {
  "Shows or hides the inspector, with its Library, Info and Adjustments tabs.|MKey: I",
  "To import photos, drag them, or a directory of them, from a Filer window into Lumen's window. "
  "A directory becomes a project.",
  "Makes a new album: from the photos selected, or empty.",
  "The Browser: every photo of what the Library tab has chosen, as thumbnails.|MKey: V goes round the layouts.",
  "Split View: the photo in focus above, the Browser's thumbnails below.|MKey: V",
  "The Viewer: one photo, as big as the window. Its adjustments are in the inspector.|MKey: V",
  "The loupe: a round magnifier that follows the pointer over the photo. Menu on the photo picks 100%, 200% or 400%.|MKey: `",
  "Exports the photos selected (or the one in the Viewer) as new JPEG or PNG files, with their adjustments.|MKey: Ctrl-E",
  "Full screen: the photo alone, filling the screen.|MKeys: F; Escape returns.",
};

static const char *const tabs[3] = {
  "The Library tab: all photos, flagged, rejected, the last import, projects and albums. Click one to show it. "
  "Menu over a project or album to rename or remove it.",
  "The Info tab: the photo's size, file, colour, and everything its camera recorded (EXIF).",
  "The Adjustments tab: exposure, white balance, highlights and shadows, curves and more for the photo in the Viewer. "
  "Your original is never changed.",
};

static const char *const exportbox[14] = {
  "Drag this icon to a directory display to save the export there (or to another program to send it).",
  "The file's name. Type a full pathname and click Save, or drag the icon.",
  NULL,
  "Saves JPEG files: small, for sharing.",
  "Saves PNG files: no loss of quality, but larger.",
  NULL,
  "The photo at its full size.",
  "Shrinks the photo so its longer side is 3840 pixels (4K).",
  "Shrinks the photo so its longer side is 2048 pixels.",
  "Shrinks the photo so its longer side is 1024 pixels.",
  NULL,
  "JPEG quality, 1-100: higher keeps more detail and makes larger files. 90 is good for most.",
  "Closes the box without exporting.",
  "Exports to the pathname typed in the box.",
};

static const char *main_part (int part)
{
  switch (part)
    {
    case MW_SEARCH:
      return "Search: type words to find photos by name, camera, date or project. Escape clears it.";
    case MW_GRIP:
      return "Drag to change the size of the window.";
    case MW_SIZE:
      return "Thumbnail size: Small, Medium or Large.";
    case MW_RATING:
      return "Click to show only photos with at least this many stars.";
    case MW_STATUS:
      return "How many photos are shown and how many selected.";
    default:
      if (part >= MW_TOOLBAR && part < MW_TOOLBAR + 9)
        return toolbar[part - MW_TOOLBAR];
      if (part >= MW_TAB && part < MW_TAB + 3)
        return tabs[part - MW_TAB];
      return "This is Lumen's window. Menu gives Lumen's menu.";
    }
}

const char *help_text (int window, int icon, int x, int y)
{
  const char *m;
  if (window == -2)
    return "This is Lumen, a photo library and editor.|MClick Select to open its window, Menu for its menu.";
  if ((m = menus_help (window, icon)) != NULL)
    return m;
  if (window == mainwin_window ())
    return main_part (mainwin_part_at (x, y, icon));
  if (window == mainwin_inspector ())
    return tabs[mainwin_tab () >= 0 && mainwin_tab () < 3 ? mainwin_tab () : 0];
  if (window == browser_window ())
    return "Click a photo to select it, Adjust to add to the selection, double-click to open it in the Viewer. "
           "Keys: 1-5 stars, 0 none, / flag, 9 reject, Delete removes from the library. Drag photos in to import.";
  if (window == viewer_window ())
    return "The photo, with its adjustments. Keys: arrows next or previous photo, Z 100%, M before and after, "
           "A auto, C crop, G grid, [ and ] turn, Escape back. Menu has the rest.";
  if (window == exportbox_window ())
    return icon >= 0 && icon < 14 && exportbox[icon] ? exportbox[icon] : "Exports photos with their adjustments.";
  if (window == plugwin_window ())
    return icon == 4 ? "Starts PackMan, where Lumen's plug-ins (such as LumenRaw, for camera raw files) are installed from."
                     : "Drag a plug-in's directory here to add it to Lumen.";
  return "";
}

void help_request (int *b)
{
  const char *t = help_text (b[8], b[9], b[5], b[6]);
  size_t len;
  int task = b[1];
  if (!t || !*t)
    return;
  len = strlen (t);
  if (len > 235)
    len = 235;                         /* a message is at most 256 bytes */
  b[3] = b[2];                         /* your_ref = their my_ref */
  b[4] = Message_HelpReply;
  memcpy (&b[5], t, len);
  ((char *) &b[5])[len] = '\0';
  b[0] = (int) ((20 + len + 1 + 3) & ~3u);
  _swix (Wimp_SendMessage, _INR (0, 2), User_Message, b, task);
}
