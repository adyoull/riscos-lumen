#!/bin/bash
# Part 2: all photos, toolbar, ratings, new album, library tab, viewer, split view.
cd $HOME/rol
C="timeout 30 python3 drv/ctl.py"; W=drv/wait.sh; S=$HOME/guide/shots; K=$HOME/guide/crop.py; SH="timeout 20 $HOME/guide/shot.sh"
$K $S/g03.png all-photos 58 44 1432 892 1100; $K $S/g03.png toolbar 60 64 1428 128 1100
$C click 766 210; $C key 5; $C click 1326 210; $C key 4; $C key SLASH; $C click 486 372; $C key 5; $C key SLASH
$C click 906 372; $C key 3; $C click 1046 372; $C key 4; $C click 1326 372; $C key 5; $C click 1186 372; $C key 2
$C click 486 372; $C click 1326 372 adjust; $W 2 >/dev/null; $C move 1500 1000
$SH g07; $K $S/g07.png ratings 412 132 1418 630 900
$C click 240 96; $W 2 >/dev/null; $C move 300 108; $C move 376 108; $W 2 >/dev/null; $C type Favourites; $W 1 >/dev/null
$SH g08; $K $S/g08.png new-album 200 74 600 170
$C key ENTER; $W 3 >/dev/null
$SH g09; $K $S/g09.png library-tab 60 128 404 470
$C dclick 486 372; $W 15 >/dev/null; $C move 1500 1000
$SH g10; $K $S/g10.png viewer 58 44 1432 892 1100
$C click 740 96; $W 8 >/dev/null; $C move 1500 1000
$SH g11; $K $S/g11.png split-view 58 44 1432 892 1100
