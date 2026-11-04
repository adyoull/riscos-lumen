#!/bin/bash
# Part 5: info pictures (Polo, taken before), window/selection menus, search, rating filter, export box.
cd $HOME/rol
C="timeout 30 python3 drv/ctl.py"; W=drv/wait.sh; S=$HOME/guide/shots; K=$HOME/guide/crop.py; SH="timeout 20 $HOME/guide/shot.sh"

$C click 672 96; $W 4 >/dev/null; $C click 120 146; $W 2 >/dev/null
$C click 900 700 menu; $W 2 >/dev/null; $SH h25; $K $S/h25.png window-menu 864 676 1038 882
$C move 960 734; $C move 1030 734; $W 3 >/dev/null; $SH h26; $K $S/h26.png selection-menu 864 676 1288 1080; $C key ESC
$C click 1100 880; $W 1 >/dev/null; $C type jump; $W 4 >/dev/null; $C move 1500 600; $SH h27; $K $S/h27.png search 58 44 1432 892 1100
$C click 1100 880; $C key ESC; $W 3 >/dev/null
$C click 868 880; $W 2 >/dev/null; $SH h28; $K $S/h28.png rating-menu 830 710 970 880
$C click 900 834; $W 3 >/dev/null; $C move 1500 600; $SH h29; $K $S/h29.png four-stars 58 44 1432 892 1100
$C click 868 880; $W 1 >/dev/null; $C click 900 746; $W 3 >/dev/null
$C click 1186 372; $C move 700 300; $C key E ctrl; $W 3 >/dev/null; $SH h30; $K $S/h30.png export-box 664 276 1112 668; $C key ESC; $W 1 >/dev/null
