#!/bin/bash
# Copyright (c) 2026 Andrew Youll
# Released under the MIT licence: see the LICENSE file.
# SPDX-License-Identifier: MIT
# Part 3: adjustments, adjusted, panel-top/bottom, before, loupe, crop, photo menu, presets (on Gallop, after new2.sh).
cd $HOME/rol
C="timeout 30 python3 drv/ctl.py"; W=drv/wait.sh; S=$HOME/guide/shots; K=$HOME/guide/crop.py; SH="timeout 20 $HOME/guide/shot.sh"
$C click 812 96; $W 4 >/dev/null; $C click 348 146; $W 6 >/dev/null; $C move 1500 1000
$SH g12; $K $S/g12.png adjustments 58 44 1432 892 1100
$C drag 180 547 250 547; $W 4 >/dev/null; $C drag 180 575 230 575; $W 4 >/dev/null; $C drag 248 489 254 489; $W 8 >/dev/null; $C move 1500 1000
$SH g13; $K $S/g13.png adjusted 58 44 1432 892 1100; $K $S/g13.png panel-top 62 130 402 870
$C click 394 700; $W 3 >/dev/null
$C drag 248 293 262 293; $W 4 >/dev/null; $C drag 248 349 270 349; $W 8 >/dev/null; $C move 1500 1000
$SH g15; $K $S/g15.png panel-bottom 62 160 402 860
$C move 1000 500; $C key M; $W 3 >/dev/null; $SH g16; $K $S/g16.png before 420 180 1420 820 700; $C key M; $W 3 >/dev/null
