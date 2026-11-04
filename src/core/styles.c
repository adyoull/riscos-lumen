/* Copyright (c) 2026 Andrew Youll
   Released under the MIT licence: see the LICENSE file.
   SPDX-License-Identifier: MIT */
/* styles.c: Lumen's built-in looks (see styles.h).

   Each is a recipe's look lines, as a preset stores them: generic darkroom
   and film-era styles, named for what they do, not after any product. */
#include <stddef.h>
#include "styles.h"

static const style list[] = {
  { "Faded Film",
    "Lifted blacks and soft contrast, a touch warm: an old print",
    "wb temp=6\n"
    "enhance contrast=-12 saturation=-18\n"
    "curves rgb=0.000:0.090,0.250:0.290,0.750:0.760,1.000:0.940\n" },
  { "Warm Vintage",
    "Warm, faded and a little yellow in the highlights",
    "wb temp=18 tint=8\n"
    "enhance contrast=4 saturation=-26\n"
    "curves rgb=0.000:0.070,0.500:0.520,1.000:0.950\n"
    "tone shadow_hue=20 shadows=20 highlight_hue=38 highlights=40\n" },
  { "Golden Hour",
    "The warm, glowing light just before sunset",
    "wb temp=38 tint=6\n"
    "exposure ev=0.10\n"
    "hs shadows=20\n"
    "enhance contrast=-6 vibrance=18\n"
    "curves red=0.000:0.000,0.500:0.540,1.000:1.000\n" },
  { "Soft Portrait",
    "Gentle contrast, warm skin, open shadows",
    "wb temp=10\n"
    "hs shadows=18\n"
    "enhance contrast=-14 saturation=-8 vibrance=6\n" },
  { "Sunlit Garden",
    "Fresh greens, warm fur and skin, and a soft glow round the light",
    "wb temp=2 tint=-2\n"
    "exposure ev=0.10\n"
    "hs highlights=45 shadows=10\n"
    "levels black=5 white=99 mid=-4\n"
    "curves rgb=0.000:0.000,0.250:0.215,0.750:0.785,1.000:1.000\n"
    "enhance contrast=22 saturation=5 vibrance=40\n"
    "hsl orange_saturation=-10 yellow_hue=10 yellow_saturation=10 green_hue=6 green_saturation=40 "
    "green_luminance=-8 aqua_saturation=15\n"
    "vignette amount=-40 midpoint=40\n"
    "glow amount=30 spread=35 highlights=55 warmth=25\n" },
  { "Vivid Landscape",
    "Rich colour and depth for skies, water and hills",
    "hs highlights=18 shadows=16\n"
    "enhance contrast=16 saturation=6 vibrance=38\n"
    "sharpen amount=30\n" },
  { "Teal & Orange",
    "Cinema grading: warm skin and highlights, cool shadows",
    "wb temp=6\n"
    "enhance contrast=12 vibrance=20\n"
    "tone shadow_hue=190 shadows=75 highlight_hue=28 highlights=45\n" },
  { "Cross Process",
    "Strong, shifted colour, as slide film developed as negative",
    "enhance contrast=14 saturation=18\n"
    "curves red=0.000:0.000,0.250:0.200,0.750:0.860,1.000:1.000 "
    "green=0.000:0.000,0.250:0.230,0.750:0.820,1.000:1.000 "
    "blue=0.000:0.100,0.500:0.500,1.000:0.800\n" },
  { "Bleach Bypass",
    "Silvery, low colour and hard contrast",
    "levels black=3\n"
    "enhance contrast=36 saturation=-58\n"
    "curves rgb=0.000:0.000,0.250:0.200,0.750:0.830,1.000:1.000\n" },
  { "Nordic Matte",
    "Cool and quiet, with soft blacks",
    "wb temp=-20 tint=-2\n"
    "enhance contrast=-8 saturation=-28\n"
    "curves rgb=0.000:0.080,0.500:0.500,1.000:0.950\n" },
  { "Classic Black & White",
    "A neutral black and white with full tones",
    "levels black=1\n"
    "enhance contrast=14 saturation=-100\n" },
  { "High-Contrast Noir",
    "Deep blacks and bright whites, for drama",
    "levels black=4 white=96\n"
    "enhance contrast=38 saturation=-100\n"
    "curves rgb=0.000:0.000,0.250:0.160,0.750:0.860,1.000:1.000\n" },
  { "Sepia",
    "The warm brown of a toned print",
    "enhance contrast=6 saturation=-100\n"
    "curves rgb=0.000:0.040,1.000:0.970\n"
    "tone shadow_hue=30 shadows=70 highlight_hue=38 highlights=55\n" },
  { "Cyanotype",
    "The Prussian blue of the sun print",
    "enhance contrast=10 saturation=-100\n"
    "curves rgb=0.000:0.050,1.000:0.960\n"
    "tone shadow_hue=212 shadows=100 highlight_hue=200 highlights=60\n" },
  { "Platinum Tone",
    "Soft, warm grey tones with a long scale",
    "enhance contrast=-10 saturation=-100\n"
    "curves rgb=0.000:0.050,0.500:0.520,1.000:0.960\n"
    "tone shadow_hue=28 shadows=22 highlight_hue=45 highlights=14\n" },
};

int styles_count (void)
{
  return (int) (sizeof list / sizeof list[0]);
}

const style *styles_get (int k)
{
  return k >= 0 && k < styles_count () ? &list[k] : NULL;
}
