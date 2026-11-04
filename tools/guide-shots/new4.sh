#!/bin/bash
# Part 4: loupe, crop, photo menu, save preset, presets menu (Gallop, after new3.sh).
cd $HOME/rol
C="timeout 30 python3 drv/ctl.py"; W=drv/wait.sh; S=$HOME/guide/shots; K=$HOME/guide/crop.py; SH="timeout 20 $HOME/guide/shot.sh"
$C move 830 300; $C key GRAVE; $W 3 >/dev/null; $C move 832 302; $W 15 >/dev/null
$SH g17; $K $S/g17.png loupe 420 180 1420 820 900; $C key GRAVE; $W 2 >/dev/null
$C key C; $W 6 >/dev/null; $C drag 424 190 540 250; $W 5 >/dev/null; $C move 1500 500
$SH g18; $K $S/g18.png crop 410 170 1430 830 900; $C key ESC; $W 4 >/dev/null
$C click 1000 500 menu; $W 2 >/dev/null; $SH g19; $K $S/g19.png photo-menu 962 466 1208 1080
$C move 1100 768; $C move 1198 768; $W 2 >/dev/null; $C move 1260 833; $C move 1336 833; $W 10 >/dev/null
$C type Show; $W 2 >/dev/null; $C type " "; $W 2 >/dev/null; $C type day; $W 1 >/dev/null
$SH g21; $K $S/g21.png save-preset 1196 728 1534 840; $C key ENTER; $W 2 >/dev/null
$C click 1000 500 menu; $W 1 >/dev/null; $C move 1100 768; $C move 1198 768; $W 2 >/dev/null
$SH g22; $K $S/g22.png presets-menu 962 466 1450 1080; $C key ESC; $W 2 >/dev/null
