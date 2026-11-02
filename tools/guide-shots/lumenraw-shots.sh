#!/bin/bash
# LumenRaw guide pictures (after guide/start.sh): Andrew's D700 NEFs.
cd /home/claude/rol
C="timeout 30 python3 drv/ctl.py"; W=drv/wait.sh; S=/home/claude/guide/shots; K="python3 /home/claude/lrguide/crop.py"; SH="timeout 20 /home/claude/guide/shot.sh"
$C drag 1852 986 1420 880; $W 3 >/dev/null
$C drag 356 1016 900 500; $W 30 >/dev/null; $C click 1360 880; $W 4 >/dev/null; $C click 500 240; $W 2 >/dev/null; $C click 232 146; $W 4 >/dev/null; $C move 1500 1000
$SH lrA; $K $S/lrA.png browser 58 44 1432 892 1100; $K $S/lrA.png info-raw 62 130 402 870
# The Labrador: catch the preview while it develops, then the developed photo.
$C dclick 700 240; sleep 1; $SH lrB; $W 30 >/dev/null; $C move 1500 1000; $SH lrC
$K $S/lrB.png preview 420 166 1416 828 540; $K $S/lrB.png status-reading 58 866 740 892; $K $S/lrC.png developed 420 166 1416 828 540
# The polo shot for the cover.
$C move 1000 500; $C key RIGHT; $W 30 >/dev/null; $C move 1500 1000; $SH lrD; $K $S/lrD.png d4 420 166 1416 828 1000
# Switched off: Plug-ins > Camera raw (LibRaw), then open the cow.
$C click 1800 1050 menu; $W 2 >/dev/null; $C move 1820 1034; $C move 1876 1034; $W 2 >/dev/null; $C click 1820 1011; $W 2 >/dev/null
$C key ESC; $W 3 >/dev/null; $C click 232 146; $W 2 >/dev/null; $C dclick 500 240; $W 10 >/dev/null; $C move 1500 1000; $SH lrE; $K $S/lrE.png switched-off 58 44 1432 892 1100
$C click 1800 1050 menu; $W 2 >/dev/null; $C move 1820 1034; $C move 1876 1034; $W 2 >/dev/null; $C click 1820 1011; $W 2 >/dev/null
