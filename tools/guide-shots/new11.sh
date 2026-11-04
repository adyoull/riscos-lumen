#!/bin/bash
# Import library and Check library (after start-lib.sh and new10.sh's DogsLib): import Eventing, drop
# DogsLib on Lumen (library-import), Import; a thumbnail deleted and a stray one added on the host,
# Check library... (library-check), Repair (library-repaired).
cd $HOME/rol
C="timeout 30 python3 drv/ctl.py"; W=drv/wait.sh; S=$HOME/guide/shots; K=$HOME/guide/crop.py; SH="timeout 20 $HOME/guide/shot.sh"
$C drag 222 976 900 500; $W 40; $C click 68 58; $W 2
$C drag 1112 955 900 600; $W 4; $C move 1400 300; $SH l3; $K $S/l3.png library-import 690 412 1230 648
$C click 1152 600; $W 15
L=$(ls -dt /tmp/rol-run.*/HardDisc4/\!Boot/Choices/Lumen/Library | head -1)
python3 -c "
import os,shutil,sys
L=sys.argv[1]; x=os.getxattr(L+'/Thumbs/02/2','user.RISC_OS.LoadExec')
os.remove(L+'/Thumbs/03/3'); os.makedirs(L+'/Thumbs/4d',exist_ok=True); shutil.copy(L+'/Thumbs/02/2',L+'/Thumbs/4d/77')
os.setxattr(L+'/Thumbs/4d/77','user.RISC_OS.LoadExec',x)" "$L"
$C click 1801 1050 menu; $W 2; $C move 1876 955; $W 2; $C click 1850 1011; $W 6; $C move 1400 300; $SH l4; $K $S/l4.png library-check 665 395 1255 665
$C click 1178 618; $W 6; $C move 1400 300; $SH l5; $K $S/l5.png library-repaired 665 395 1255 665
