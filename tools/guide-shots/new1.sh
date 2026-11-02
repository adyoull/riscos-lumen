#!/bin/bash
# Guide pictures with Andrew's photos, part 1: start screen, first import, all photos (after start-new.sh).
cd /home/claude/rol
C="timeout 30 python3 drv/ctl.py"; W=drv/wait.sh; S=/home/claude/guide/shots; K=/home/claude/guide/crop.py; SH="timeout 20 /home/claude/guide/shot.sh"
$SH g01; $K $S/g01.png start-screen 58 44 1432 892 1100
$C drag 222 976 900 500; $W 40 >/dev/null; $C move 1500 1000
$SH g02; $K $S/g02.png first-import 58 44 1432 892 1100
for xy in "86 1038" "626 976" "86 976" "356 976" "490 976"; do $C drag $xy 900 500; $W 30 >/dev/null; done
$C click 126 212; $W 8 >/dev/null; $C move 1500 1000
$SH g03
