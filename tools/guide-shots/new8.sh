#!/bin/bash
# Copyright (c) 2026 Andrew Youll
# Released under the MIT licence: see the LICENSE file.
# SPDX-License-Identifier: MIT
# Second session (after start-new2.sh): Photos imported whole as one project, status bar, project menu, watching + folder albums, activity, folder-albums.
cd $HOME/rol
C="timeout 30 python3 drv/ctl.py"; W=drv/wait.sh; S=$HOME/guide/shots; K=$HOME/guide/crop.py; SH="timeout 20 $HOME/guide/shot.sh"
$C drag 1852 986 1420 880; $W 3 >/dev/null; $C click 1360 880; $W 1 >/dev/null
$C drag 358 1038 900 500; for i in 1 2 3 4 5 6 7 8; do sleep 0.4; $SH f5_$i; done   # pick the frame with "Making thumbnails: N to go"
$K $S/f5_2.png status-bar 58 866 898 892
$W 5 >/dev/null; $C click 200 880; $W 3 >/dev/null
$C click 120 342 menu; $W 2 >/dev/null; $C click 160 386 adjust; $W 3 >/dev/null; $C click 160 408 adjust; $W 5 >/dev/null; $C move 165 386; $W 2 >/dev/null
$SH f8; $K $S/f8.png project-menu 60 300 400 500; $C key ESC; $W 2 >/dev/null; $C click 140 394; $W 5 >/dev/null
# A genuinely new photo (a copy would be recognised as the same photo), then wait for the 5-minute look.
python3 -c "
from PIL import Image
im=Image.open('/RISCOS/Photos/Eventing/Big_smile,c85'); w,h=im.size; im.crop((int(w*0.25),0,int(w*0.85),int(h*0.9))).save('/RISCOS/Photos/Eventing/Big_smile_close,c85','JPEG',quality=92)"
for i in $(seq 22); do $W 15 >/dev/null; done
$SH f13; $K $S/f13.png activity 660 344 1274 716; $C click 1260 354; $W 3 >/dev/null; $C move 1500 1000; $SH f14; $K $S/f14.png folder-albums 58 44 1432 892 1100
rm "/RISCOS/Photos/Eventing/Big_smile_close,c85"
