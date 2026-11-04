#!/bin/bash
# Copyright (c) 2026 Andrew Youll
# Released under the MIT licence: see the LICENSE file.
# SPDX-License-Identifier: MIT
# Part 6: full screen, remove, missing, raw import + raw in the viewer.
cd $HOME/rol
C="timeout 30 python3 drv/ctl.py"; W=drv/wait.sh; S=$HOME/guide/shots; K=$HOME/guide/crop.py; SH="timeout 20 $HOME/guide/shot.sh"
$C dclick 486 372; $W 10 >/dev/null; $C key F; $W 10 >/dev/null; $C move 1919 540; $W 1 >/dev/null; $SH h31; $K $S/h31.png full-screen 0 0 1920 1080 900
$C key F; $W 4 >/dev/null; $C click 672 96; $W 3 >/dev/null
$C click 486 534; $C key DELETE; $W 3 >/dev/null; $SH h32; $K $S/h32.png remove 772 410 1144 652; $C click 1082 622; $W 1 >/dev/null
mkdir -p /RISCOS/Photos/Moved && mv "/RISCOS/Photos/Schooling/Trot,c85" /RISCOS/Photos/Moved/
$C click 88 58; $W 2 >/dev/null; $C click 1800 1048; $W 8 >/dev/null; $SH h33
mv "/RISCOS/Photos/Moved/Trot,c85" /RISCOS/Photos/Schooling/; rmdir /RISCOS/Photos/Moved
$K $S/h33.png missing 58 44 1432 660 1100
# Raw: import the folder, open the Labrador NEF; its Info tab is the guide's Info pictures too (no email in its EXIF).
$C drag 760 976 900 500; $W 60 >/dev/null; $C move 1500 1000
$C dclick 626 222; $W 40 >/dev/null; $C click 232 146; $W 4 >/dev/null; $C move 1500 1000; $SH h35
$C click 394 700; $W 2 >/dev/null; $SH h36
$K $S/h35.png info-tab 62 130 402 870; $K $S/h36.png info-exif 62 130 402 870; $K $S/h35.png raw 58 44 1432 892 1100
