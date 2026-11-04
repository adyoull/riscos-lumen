<!-- Copyright (c) 2026 Andrew Youll
     Released under the MIT licence: see the LICENSE file.
     SPDX-License-Identifier: MIT -->
# Lumen format plug-ins: API 1

A format plug-in teaches Lumen to read photos in a format Lumen doesn't
know itself. LumenHEIF (HEIF/HEIC photos) is one. Lumen has no knowledge of
any format a plug-in reads. The plug-in's `Plugin` file says which files are
its, and its program does all the reading. A new format needs only a new
plug-in, never a new Lumen.

Camera raw files are the exception. Lumen knows them itself, as it knows
JPEG and PNG: it imports them, and reads their camera, date and EXIF,
even before a raw plug-in is installed. LumenRaw (`Kind: raw`) develops
them, run in the same way as described below. Format plug-ins are for
every other format.

## Where plug-ins go

Each plug-in is a directory in `!Lumen.Plugins`. PackMan installs them
there, and a plug-in can also be dragged into Lumen's Plug-ins > Add
plug-in window. Lumen reads every plug-in when it starts, and again
whenever one is added, removed or switched on or off from its Plug-ins
menu.

## The Plugin file

The directory holds a text file called `Plugin`, with one `Key: value`
per line. Lines starting with `#` are comments.

```
Name: HEIF photos (libheif)
Version: 1.0
Kind: format
API: 1
Format: HEIF photo
Extensions: heic heif hif
Signature: 4 ftypheic
Signature: 4 ftypmif1
Program: LumenHEIF
Package: LumenHEIF
Description: Reads HEIF/HEIC photos with libheif and FFmpeg's HEVC decoder
```

| Key | Meaning |
| --- | --- |
| `Name` | The plug-in's name. Lumen shows it in its Plug-ins menu and messages, followed by `Version`, without a closing part in brackets: "HEIF photos (libheif)" is shown as "HEIF photos". |
| `Kind` | `format` for these plug-ins. (`raw` is LumenRaw's.) |
| `API` | The version of this API that the plug-in follows: `1`. Lumen lists a plug-in that asks for a newer API but doesn't run it, and says a newer Lumen is needed. |
| `Format` | The name for one of its files, used in Lumen's messages and on the Info tab, such as "This is a HEIF photo...". |
| `Extensions` | The file extensions it claims, separated by spaces, in any case: `heic` matches `IMG_0001/HEIC` on RISC OS and `IMG_0001.heic` elsewhere. |
| `Signature` | Bytes it claims at an offset in the file: the offset in decimal, a space, then the bytes. `\xNN` gives any byte, `\\` a backslash, and `\x20` a space at the end. A file matches if any one signature matches. At most 8 signatures, each up to 24 bytes, within the first 4 KB of the file. |
| `Program` | The program in the directory that Lumen runs (`LumenHEIF,ff8`, an absolute file). |
| `Package` | The PackMan package it comes in. Lumen names it in its messages. |
| `Description` | One line about it. |

A file is the plug-in's if its extension is one of `Extensions`, or if
its first bytes match a `Signature`. Lumen reads JPEG and PNG files
itself, whatever their names, and camera raw files go to LumenRaw. Lumen
imports a file a format plug-in claims even while that plug-in is
switched off, because it is still installed. Files that no installed
plug-in claims are left alone.

## The program

Lumen runs the program once per job, in a task window, one job at a time,
in one of two ways:

```
<Program> thumb  SRC IMAGE INFO
<Program> decode SRC IMAGE INFO MAXSIDE
```

- `SRC` is the photo, by its full pathname.
- `thumb` asks for a small picture for the thumbnail, about 512 pixels on
  its longer side. The file's own embedded thumbnail is best if it has one.
- `decode` asks for the photo itself, no bigger than `MAXSIDE` pixels on
  its longer side. 0 means full size. The Viewer asks for 1600 pixels;
  100%, the loupe and exports ask for 0.
- `IMAGE` is where to write the picture. Write a binary PPM (`P6`, 8 bits a
  sample), or a baseline JPEG. Use sRGB, turned the right way up, and
  convert other colour spaces (Display P3, say) first.
- `INFO` is where to write the details, one `key=value` per line:

| Key | Meaning |
| --- | --- |
| `width`, `height` | The photo's full size, in pixels, as the picture is turned. |
| `orientation` | 1 if the picture is already upright (the usual case). Otherwise the EXIF orientation (1-8) for Lumen to apply. |
| `make`, `model` | The camera. |
| `iso`, `exposure`, `fnumber`, `focal` | Exposure details: `exposure` in seconds, `focal` in mm. |
| `taken` | When the photo was taken, as `YYYY:MM:DD HH:MM:SS`. |
| `exif` | `OFFSET LENGTH`: where the photo's EXIF data (a TIFF header, `II*\0` or `MM\0*`, and its IFDs) is in `SRC`, as a byte offset and length. Lumen lists it on the Info tab. Leave it out if there's no EXIF. |
| `error` | Something went wrong. The value says what, in a few words, and Lumen shows it. |

Write `INFO` and `IMAGE` under other names first, then rename them, so
Lumen never reads half a file. Lumen waits for the program to end, then
reads `INFO`. If `INFO` has `error`, or the program stops without writing
it, the job has failed.

Lumen keeps the details in its library, and makes thumbnails and previews
from the pictures. It does everything else itself: adjustments, exports
and printing. It never changes the photo's file.

## Building one

LumenHEIF's source, `plugins/heif/lumenheif.c`, is a complete example. It
is about 500 lines around libheif, and its tests are
`tests/test_heif.sh`. A plug-in built with GCCSDK for ARMv7 with VFPv3
runs wherever Lumen does.
