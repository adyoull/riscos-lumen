#!/bin/bash
# Copyright (c) 2026 Andrew Youll
# Released under the MIT licence: see the LICENSE file.
# SPDX-License-Identifier: MIT
# Part 7: black & white (Dry_stone_wall). Browser grid after the raw import: columns 488..1328 step 140, rows 220 382 544 706.
cd $HOME/rol
C="timeout 30 python3 drv/ctl.py"; W=drv/wait.sh; S=$HOME/guide/shots; K=$HOME/guide/crop.py; SH="timeout 20 $HOME/guide/shot.sh"
$C click 120 146; $W 2 >/dev/null; $C click 126 212; $W 6 >/dev/null; $C click 672 96; $W 4 >/dev/null
$C dclick 768 382; $W 20 >/dev/null; $C click 348 146; $W 6 >/dev/null; $C click 394 700; $W 3 >/dev/null
$C drag 180 574 330 574; $W 6 >/dev/null; $C drag 230 656 212 656; $W 6 >/dev/null; $C drag 238 600 254 600; $W 10 >/dev/null; $C move 1500 1000
$SH bw1; $K $S/bw1.png black-white 58 44 1432 892 1100
$C move 1000 500; for i in 1 2 3 4; do $C key Z ctrl; $W 3 >/dev/null; done
# Colour panel: Blue on Dry_stone_wall (the panel is scrolled to Colour by the B&W step's page-down).
$C drag 248 458 236 458; $W 5 >/dev/null; $C drag 248 486 280 486; $W 8 >/dev/null; $C move 1500 1000
$SH c1; $K $S/c1.png colour-panel 62 368 402 538
$C move 1000 500; $C key Z ctrl; $W 3 >/dev/null; $C key Z ctrl; $W 3 >/dev/null
# Noise reduction and vignette: Dachshunds (9 of 22), panel scrolled to the bottom.
$C click 672 96; $W 4 >/dev/null; $C dclick 628 220; $W 20 >/dev/null; $C drag 394 500 394 860; $W 3 >/dev/null
$C drag 180 482 208 482; $W 4 >/dev/null; $C drag 180 510 222 510; $W 4 >/dev/null; $C drag 248 654 210 654; $W 10 >/dev/null; $C move 1500 1000
$SH v3; $K $S/v3.png noise-vignette-panel 62 446 402 706; $K $S/v3.png vignette 420 166 1416 828 900
# Retouch: the pot behind the dachshunds.
$C move 1000 500; $C key R; $W 5 >/dev/null; for i in 1 2 3 4 5 6; do $C key RIGHTBRACE; done; $W 3 >/dev/null; $C move 502 312; $W 4 >/dev/null; $SH r1
$C click 502 312; $W 15 >/dev/null; $C move 1500 1000; $W 2 >/dev/null; $SH r2; $K $S/r2.png retouch 58 44 1432 892 1100
$C key ENTER; $W 10 >/dev/null; $C move 1500 1000; $W 2 >/dev/null; $SH r3; $K $S/r1.png retouch-before 422 210 762 430; $K $S/r3.png retouch-after 422 210 762 430
# Brush: Terrier (18 of 22): burn round the edges, dodge the dog.
$C move 1000 500; for i in $(seq 9); do $C key RIGHT; $W 2 >/dev/null; done; $W 20 >/dev/null; $C move 1500 1000; $SH b0; $C move 1000 500; $C key B; $W 5 >/dev/null
$C click 562 146; $W 2 >/dev/null; $C drag 550 180 585 180; $W 2 >/dev/null; $C drag 950 180 972 180; $W 2 >/dev/null
for d in "440 240 1030 240" "450 280 450 760" "1260 230 1400 230" "1400 260 1400 640"; do $C drag $d; $W 20 >/dev/null; done
$C click 466 146; $W 2 >/dev/null; $C drag 585 180 562 180; $W 2 >/dev/null; $C drag 972 180 955 180; $W 2 >/dev/null; $C drag 1150 330 1190 640; $W 20 >/dev/null
$C move 1320 700; $W 4 >/dev/null; $SH b1; $K $S/b1.png brush 58 44 1432 892 1100
$C key ENTER; $W 12 >/dev/null; $C move 1500 1000; $W 2 >/dev/null; $SH b2; $K $S/b0.png brush-before 420 165 1416 830 540; $K $S/b2.png brush-after 420 165 1416 830 540
# Compare: Cross_pole and Upright.
$C click 672 96; $W 4 >/dev/null; $C move 900 600; $C key HOME; $W 3 >/dev/null
$C click 488 382; $C click 1328 544 adjust; $W 2 >/dev/null; $C key O; $W 30 >/dev/null; $C move 1500 1000; $SH cp1; $K $S/cp1.png compare 58 44 1432 892 1100
# Labels: close and reopen the window (Trot's missing badge goes), label menu on Gallop, then more labels.
$C move 1000 500; $C key ESC; $W 3 >/dev/null; $C key ESC; $W 3 >/dev/null; $C click 88 58; $W 2 >/dev/null; $C click 1800 1048; $W 10 >/dev/null
$C move 900 600; $C key HOME; $C key ESC; $W 2 >/dev/null; $C click 908 382; $W 1 >/dev/null
$C click 900 700 menu; $W 2 >/dev/null; $C move 960 734; $C move 1030 734; $W 2 >/dev/null; $C move 1100 743; $C move 1276 743; $W 3 >/dev/null; $SH l1; $K $S/l1.png label-menu 1036 690 1400 925; $C click 1300 843; $W 2 >/dev/null
lab() { $C click $1 $2; $W 1 >/dev/null; $C click 900 700 menu; $W 2 >/dev/null; $C move 960 734; $C move 1030 734; $W 2 >/dev/null; $C move 1100 743; $C move 1276 743; $W 2 >/dev/null; $C click 1300 $3; $W 2 >/dev/null; }
lab 768 544 843; lab 1188 382 821; lab 488 706 777; lab 768 382 865; lab 1328 382 777; $C click 1188 544; $W 2 >/dev/null; $C move 1500 1000; $SH l2; $K $S/l2.png labels 412 132 1418 630 900
# Styles preview: Leading_out, pointer on Sepia.
$C dclick 488 544; $W 20 >/dev/null; $C click 1000 500 menu; $W 2 >/dev/null; $C move 1100 768; $C move 1198 768; $W 3 >/dev/null
$C move 1260 766; $C move 1336 766; $W 3 >/dev/null; $C move 1380 766; $C move 1380 1023; $W 12 >/dev/null; $SH st3; $K $S/st3.png styles-preview 420 170 1530 1080 900; $C key ESC; $W 3 >/dev/null
# Split toning panel after choosing Sepia (then undone).
$C click 1000 500 menu; $W 2 >/dev/null; $C move 1100 768; $C move 1198 768; $W 3 >/dev/null; $C move 1260 766; $C move 1336 766; $W 3 >/dev/null; $C move 1380 766; $C click 1380 1023; $W 10 >/dev/null
$C click 348 146; $W 4 >/dev/null; $C move 1500 1000; $SH st4; $K $S/st4.png split-toning 62 304 402 556; $C move 1000 500; $C key Z ctrl; $W 4 >/dev/null
# Keywords (window opens beside the clicked photo; Down goes caption -> keywords).
$C key ESC; $W 4 >/dev/null; $C move 900 600; $C key HOME; $W 2 >/dev/null
tag() { $C click $1 $2; $W 1 >/dev/null; $C key K ctrl; $W 3 >/dev/null; $C key DOWN; $W 1 >/dev/null; $C type "$3"; $W 2 >/dev/null; $C key ENTER; $W 3 >/dev/null; }
tag 768 220 "Horses, Polo"; tag 908 382 "Horses, Eventing"; tag 1188 220 "Horses, Show jumping"; tag 1328 382 "Horses, Jousting"
$C click 488 706; $W 1 >/dev/null; $C key K ctrl; $W 3 >/dev/null; $C type "The knights shake hands after the joust"; $W 2 >/dev/null; $C key DOWN; $C type "Jousting, Knights"; $W 2 >/dev/null
$C click 930 767; $W 3 >/dev/null; $SH kw5; $K $S/kw5.png tags-window 454 676 1080 870; $C click 1000 808; $W 2 >/dev/null
$C click 900 832; $W 3 >/dev/null; $C click 232 146; $C click 488 706; $W 3 >/dev/null; $SH kw6; $K $S/kw6.png info-keywords 62 160 402 440
# Smart album "Best horses": horses, 3+.
$C click 240 96; $W 2 >/dev/null; $C click 280 151; $W 3 >/dev/null; $C type "Best horses"; $W 2 >/dev/null; $C click 570 212; $W 1 >/dev/null; $C type horses; $W 2 >/dev/null; $C click 570 267; $W 2 >/dev/null
$C move 1700 200; $SH sm2; $K $S/sm2.png smart-album 244 128 792 446; $C click 730 427; $W 6 >/dev/null; $C click 120 146; $W 3 >/dev/null; $SH sm4; $K $S/sm4.png smart-library 60 128 404 600
# Versions and stacks: Over_the_rail and Gallop each get a duplicate (stacked with it), styled Classic Black & White; then the Gallop stack closed.
$C click 126 212; $W 4 >/dev/null; $C move 900 600; $C key HOME; $C key ESC; $W 2 >/dev/null
$C click 708 544; $W 1 >/dev/null; $C key D ctrl; $W 5 >/dev/null; $C click 908 382; $W 1 >/dev/null; $C key D ctrl; $W 5 >/dev/null
$C click 1048 382; $C click 1048 544 adjust; $W 1 >/dev/null; $C click 900 700 menu; $W 2 >/dev/null; $C move 960 734; $C move 1030 734; $W 2 >/dev/null
$C move 1100 943; $C move 1276 943; $W 2 >/dev/null; $C move 1330 943; $C move 1394 943; $W 3 >/dev/null; $C move 1450 943; $C click 1480 979; $W 12 >/dev/null
$C key ESC; $W 2 >/dev/null; $C click 768 544; $W 2 >/dev/null; $C move 1500 1000; $SH stk4; $K $S/stk4.png stack-open 830 290 1150 630
$C click 908 382; $W 1 >/dev/null; $C key S; $W 4 >/dev/null; $C move 1500 1000; $SH stk5; $K $S/stk5.png stack-closed 700 330 1260 470
$C click 900 700 menu; $W 2 >/dev/null; $C move 960 778; $C move 1030 778; $W 3 >/dev/null; $SH vm1; $K $S/vm1.png versions-menu 864 676 1246 948; $C key ESC; $W 1 >/dev/null
