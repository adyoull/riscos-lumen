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

static const char *const toolbar[11] = {
  "Shows or hides the inspector, with its Library, Info and Adjustments tabs.|MKey: I",
  "To import photos, drag them, or a directory of them, from a Filer window into Lumen's window. "
  "A directory becomes a project.",
  "Makes a new album: from the photos selected, or empty.",
  "Takes back the last change to the photo's adjustments (in the Viewer or Split View).|MKey: Ctrl-Z",
  "Does again the change Undo took back.|MKey: Ctrl-Y",
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

static const char *const exportbox[31] = {
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
  "Shrinks the photo so it fits inside the width and height beside it, keeping its shape (a screen, a website's limit...).",
  "The width of the box the photo must fit in, in pixels.",
  NULL,
  "The height of the box the photo must fit in, in pixels.",
  NULL,
  NULL,
  "sRGB: right for screens, the web and most printing services. Use it if unsure.",
  "Adobe RGB: a wider range of colours, for printing and editing programs that understand it (a profile goes in the file).",
  "Display P3: the wider colours of recent Apple screens and phones (a profile goes in the file).",
  NULL,
  "No extra sharpening (the photo's own Sharpen adjustment still applies).",
  "A little sharpening for the export's size: for screens.",
  "Moderate sharpening for the export's size: for most uses.",
  "Strong sharpening for the export's size: for printing, where ink softens detail.",
  NULL,
  "How files are named when several photos are exported: {name}, {n}, {nnn}, {date}, {year}, {month}, {day}, {time}, "
  "{project}, {camera}, {rating}, and any text of your own.",
  "An example name pattern. The parts a pattern can use: {name}, {n} (1, 2, 3...), {nnn} (001, 002...), {date}, {year}, {month}, {day}, {time}, {project}, {camera}, {rating}.",
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
    case MW_ACTIVITY:
      return "What Lumen is doing: the ring fills as the job goes on (it turns when Lumen can't tell how far "
             "it has got). Click to see everything running and what Lumen has done.";
    default:
      if (part >= MW_TOOLBAR && part < MW_TOOLBAR + 11)
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
    return icon >= 0 && icon < 31 && exportbox[icon] ? exportbox[icon] : "Exports photos with their adjustments.";
  if (window == choicewin_window ())
    switch (icon)
      {
      case 1: return "Where the library is kept, from the next start. Empty: the usual place. Drag a directory here to use it.";
      case 4: case 5: return "The format the Export box starts with.";
      case 7: return "The JPEG quality the Export box starts with, 1-100.";
      case 9: case 10: case 11: case 12: return "The size the Export box starts with.";
      case 14: case 15: case 16: return "The thumbnail size Lumen starts with.";
      case 17: return "Ticked: Lumen checks that every photo's file is still there when it opens.";
      case 18: return "Closes the window without saving. Adjust: puts back what is saved.";
      case 19: return "Saves these choices.";
      case 21: return "Ticked: folders you drag in from now on are watched: Lumen brings in new photos put in them.";
      case 22: return "Ticked: the folders inside the folders you drag in become albums, under their project.";
      case 24: return "How often Lumen looks in watched folders for new photos, in minutes. 0: only when it starts "
                      "and when its window opens.";
      default: return "Lumen's Choices.";
      }
  if (window == activitywin_window ())
    return activitywin_part_at (x, y) ? "Clears the list of what Lumen has done (the file in Lumen's Choices too)."
                                      : "What Lumen is doing now, each with how far it has got, and what it has "
                                        "done, newest first. The list is kept between sessions.";
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
