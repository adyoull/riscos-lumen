#!/bin/bash
# Printing (after start-print.sh): import Dogs_and_garden, tick Colour in !Printers' Configure, print Dachshunds + Labradors 15x10 to the file printer, render with gs.
cd /home/claude/rol
C="timeout 30 python3 drv/ctl.py"; W=drv/wait.sh; S=/home/claude/guide/shots; K=/home/claude/guide/crop.py; SH="timeout 20 /home/claude/guide/shot.sh"
$C drag 1852 986 1420 880; $W 3 >/dev/null; $C drag 86 976 900 500; $W 30 >/dev/null
$C drag 600 938 600 600; $W 2 >/dev/null; $C click 178 1048 menu; $W 2 >/dev/null; $C click 220 932; $W 3 >/dev/null
$C click 40 981 menu; $W 2 >/dev/null; $C click 60 880; $W 3 >/dev/null; $C click 434 710; $W 1 >/dev/null; $C click 500 988; $W 3 >/dev/null
$C click 628 222; $W 1 >/dev/null; $C click 768 222 adjust; $W 1 >/dev/null; $C key P ctrl; $W 4 >/dev/null; $C click 852 336; $W 2 >/dev/null; $C move 1700 200
$SH p12; $K $S/p12.png print-box 734 198 1148 482; $C click 1090 458; $W 2 >/dev/null; for i in $(seq 12); do $W 10 >/dev/null; done
cd /tmp/claude-0 && rm -f pg*.png && gs -q -dNOPAUSE -dBATCH -sDEVICE=png16m -r40 -sOutputFile=pg%d.png /RISCOS/lumen.ps && python3 -c "
from PIL import Image
a=Image.open('pg1.png'); b=Image.open('pg2.png'); w,h=a.size; g=24
c=Image.new('RGB',(w*2+g*3,h+g*2),(120,120,120)); c.paste(a,(g,g)); c.paste(b,(w+g*2,g)); c.save('/build/lumen/docs/guide/images/print-pages.png',optimize=True)"
