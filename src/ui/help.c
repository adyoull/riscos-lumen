/* Copyright (c) 2026 Andrew Youll
   Released under the MIT licence: see the LICENSE file.
   SPDX-License-Identifier: MIT */
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
  "The Info tab: the photo's size, caption and keywords (click them to change them), file, colour, and everything "
  "its camera recorded (EXIF).",
  "The Adjustments tab: exposure, white balance, curves, colour by hue (pick a band, then its sliders), noise "
  "reduction, sharpening, vignette, glow and more for the photo in the Viewer. Your original is never changed.",
};

static const char *const exportbox[32] = {
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
  "Saves TIFF files: no loss of quality, for editing and printing programs (8 bits a channel, LZW compressed).",
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
  if (window == viewer_window () && viewer_brushing ())
    return "Layers: adjustments on part of the photo, each where its mask is. Paint with Select to add to the chosen "
           "layer's mask (a new brush layer if none is chosen), Erase to take it off, Adjust on a stroke to take it away. "
           "New layer Gradient or Radial, then drag on the photo to draw one; drag its handles to change it. Click a pin, "
           "or a line in the Adjustments tab's Layers list, to choose a layer; the tab has its sliders (Adjust: back to 0), "
           "Invert, Show mask (in red) and Delete; its tick switches a layer off and on. "
           "[ and ] size, { and } feather.";
  if (window == viewer_window () && viewer_retouching ())
    return "Retouch: click on a blemish to paint it out, or drag along a scratch, a wire or a hair. Repair matches the "
           "source to what's round it, so it blends in; Clone copies it as it is. Drag a spot or its source to move it; "
           "Adjust takes it away. [ and ] size, { and } feather.";
  if (window == viewer_window ())
    return "The photo, with its adjustments. Keys: arrows next or previous photo, Z 100%, M before and after, "
           "A auto, C crop, G grid, [ and ] turn, Escape back. Menu has the rest.";
  if (window == exportbox_window ())
    return icon >= 0 && icon < 32 && exportbox[icon] ? exportbox[icon] : "Exports photos with their adjustments.";
  if (window == choicewin_window ())
    switch (icon)
      {
      case 1: return "Where the library is kept, from the next start. Empty: the usual place. Drag a directory here to use it.";
      case 4: case 5: case 31: return "The format the Export box starts with.";
      case 7: return "The JPEG quality the Export box starts with, 1-100.";
      case 9: case 10: case 11: case 12: return "The size the Export box starts with.";
      case 14: case 15: case 16: return "The thumbnail size Lumen starts with.";
      case 17: return "Ticked: Lumen checks that every photo's file is still there when it opens.";
      case 32: case 33: case 34: case 35: case 36:
        return "Noise reduction in the camera raw plug-in, on the raw data before it's made into a picture (LibRaw's "
               "FBDD and wavelet noise reduction). For raw photos opened and exported from now on; the Adjustments "
               "tab's Noise Reduction works on top of it.";
      case 18: return "Closes the window without saving. Adjust: puts back what is saved.";
      case 19: return "Saves these choices.";
      case 21: return "Ticked: folders you drag in from now on are watched: Lumen brings in new photos put in them.";
      case 22: return "Ticked: the folders inside the folders you drag in become albums, under their project.";
      case 24: return "How often Lumen looks in watched folders for new photos, in minutes. 0: only when it starts "
                      "and when its window opens.";
      default: return "Lumen's Choices.";
      }
  if (window == printbox_window ())
    switch (icon)
      {
      case 1: return "The printer chosen in !Printers. To use another, choose it in !Printers.";
      case 3: return "How many copies of each photo to print.";
      case 5: return "Each photo as large as it fits on the page, all of it showing.";
      case 6: return "Each photo filling the page; parts beyond the printable area are cut off.";
      case 7: case 8: case 10: return "Each photo at most this size in centimetres, width by height, keeping its shape.";
      case 12: return "Ticked: a landscape photo on a portrait page (or the other way) is turned a quarter to use the page better.";
      case 13: return "Ticked: the photo is sharpened for print, where ink and paper soften detail.";
      case 15: return "Closes the box without printing.";
      case 16: return "Prints. The photos are prepared in the background first (the bottom bar shows how far it has got).";
      default: return "Prints photos, one to a page.";
      }
  if (window == tagbox_window ())
    switch (icon)
      {
      case 1: return "A line about the photo. It goes into exported files (as XMP), and search finds it.";
      case 3: return "Words or short phrases, with commas between: places, people, subjects. Search and smart albums find them; "
                     "exports carry them.";
      case 4: return "The keywords already in the library: choose one to add it.";
      case 6: return "Closes the window without changing anything.";
      case 7: return "Saves the caption and keywords. For several photos: the caption goes to all of them if you changed it, "
                     "and the keywords are added to all.";
      default: return "The caption and keywords of the photo, or the photos selected.";
      }
  if (window == smartbox_window ())
    switch (icon)
      {
      case 1: return "The album's name, in the Library tab.";
      case 3: return "Words the photos must all match: in the name, caption, keywords, camera, project or date. Empty: any.";
      case 6: case 7: case 8: case 9: case 10: case 11: return "The lowest rating a photo may have to be in the album.";
      case 12: return "Ticked: only flagged photos.";
      case 13: return "Ticked: only photos with adjustments.";
      case 15: case 17: return "The dates the photos were taken between, such as 2026, 2026-06 or 16/6/2026. Empty: no limit.";
      case 20: return "Closes the window without changing anything.";
      case 21: return "Makes the album (or saves the change). It keeps up as photos change.";
      default: return "A smart album gathers the photos that match what you choose here.";
      }
  if (window == activitywin_window ())
    return activitywin_part_at (x, y) ? "Clears the list of what Lumen has done (the file in Lumen's Choices too)."
                                      : "What Lumen is doing now, each with how far it has got, and what it has "
                                        "done, newest first. The list is kept between sessions.";
  if (window == libbox_export_window ())
    switch (icon)
      {
      case 0: return "Drag this to a directory display to make the new library there.";
      case 1: return "The new library's name. Or type a full pathname and click Save.";
      case 3: return "Ticked: the original files are copied into the new library (Originals, a directory for each "
                     "project), so it can go to another disc or computer. Not ticked: it refers to them where they are.";
      case 4: return "Closes the box without exporting.";
      case 5: return "Exports to the full pathname typed in the name.";
      default: return "Makes a new Lumen library of these photos, with all their adjustments, albums and keywords.";
      }
  if (window == libbox_import_window ())
    switch (icon)
      {
      case 4: return "Ticked: the new photos' files are copied into this library's Originals directory. "
                     "Not ticked: this library refers to them where they are (a disc that must stay connected).";
      case 5: return "Closes the window without importing.";
      case 6: return "Brings the photos in: their versions, adjustments, ratings, keywords, albums and presets.";
      default: return "Drag another Lumen library's directory here. What would come across is shown before anything "
                      "changes; photos already here aren't added twice.";
      }
  if (window == libbox_check_window ())
    return icon == 7 ? "Puts right what the check found. A copy of the catalogue is kept first, as Catalogue-backup."
         : icon == 6 ? "Closes the window (and stops the check, if it is still going)."
         : "Checks the library's catalogue and thumbnails for anything wrong.";
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
