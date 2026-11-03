#!/bin/bash
# Export as library (after start-lib.sh): import Dogs_and_garden, rate Labradors 4, the project's
# menu > Export as library... (library-export), named DogsLib and dragged to IXFS:$.RISCOS; the
# icon bar menu's Open library submenu (library-menu).
cd /home/claude/rol
C="timeout 30 python3 drv/ctl.py"; W=drv/wait.sh; S=/home/claude/guide/shots; K=/home/claude/guide/crop.py; SH="timeout 20 /home/claude/guide/shot.sh"
rm -rf /RISCOS/DogsLib
$C drag 86 976 900 500; $W 30; $C click 737 222; $C key 4; $W 1
$C click 176 342 menu; $W 2; $C click 220 444; $W 2; $C move 900 800; $SH l1; $K $S/l1.png library-export 160 405 620 670
$C key U ctrl; $C type DogsLib; $C drag 386 482 1360 1062; $W 8
$C click 1801 1050 menu; $W 2; $C move 1876 955; $W 2; $SH l2; $K $S/l2.png library-menu 1500 830 1920 1080
$C key ESC; $W 1
