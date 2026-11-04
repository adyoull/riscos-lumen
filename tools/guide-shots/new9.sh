#!/bin/bash
# Printing (after start-print.sh): import Dogs_and_garden and Eventing, tick Colour in !Printers' Configure,
# then print all nine 4 to a page with names (print-box, print-pages) and as many 9 x 6 cm as fit (print-cut).
# The Print box opens at the bottom right here. Pages go to /RISCOS/lumen.ps and are rendered with gs.
cd $HOME/rol
C="timeout 30 python3 drv/ctl.py"; W=drv/wait.sh; S=$HOME/guide/shots; K=$HOME/guide/crop.py; SH="timeout 20 $HOME/guide/shot.sh"
render () { mkdir -p /tmp/lumen-shots && cd /tmp/lumen-shots && rm -f pg*.png && gs -q -dNOPAUSE -dBATCH -sDEVICE=png16m -r$1 -sOutputFile=pg%d.png /RISCOS/lumen.ps; cd $HOME/rol; }
$C drag 1852 986 1420 880; $W 3 >/dev/null; $C drag 86 976 900 500; $W 30 >/dev/null; $C drag 222 976 900 500; $W 30 >/dev/null; $C click 126 212; $W 4 >/dev/null
$C drag 600 938 600 600; $W 2 >/dev/null; $C click 178 1048 menu; $W 2 >/dev/null; $C click 220 932; $W 3 >/dev/null
$C click 40 981 menu; $W 2 >/dev/null; $C click 60 880; $W 3 >/dev/null; $C click 434 710; $W 1 >/dev/null; $C click 500 988; $W 3 >/dev/null
rm -f /RISCOS/lumen.ps
$C click 1200 700; $W 1 >/dev/null; $C key A ctrl; $W 1 >/dev/null; $C key P ctrl; $W 4 >/dev/null
$C click 1368 846; $W 1 >/dev/null; $C click 1286 906; $W 1 >/dev/null; $C move 1700 300; $SH m3; $K $S/m3.png print-box 1166 680 1622 1060
$C click 1560 1020; $W 2 >/dev/null; for i in $(seq 18); do $W 10 >/dev/null; done
render 50; python3 -c "
from PIL import Image
import glob
ps=[Image.open(f) for f in sorted(glob.glob('/tmp/lumen-shots/pg*.png'))]; w,h=ps[0].size; g=16
c=Image.new('RGB',(len(ps)*(w+g)+g,h+2*g),(110,110,110))
for i,p in enumerate(ps): c.paste(p,(g+i*(w+g),g))
c=c.resize((1100,round(1100*c.height/c.width)),Image.LANCZOS); c.save('/build/lumen/docs/guide/images/print-pages.png',optimize=True)"
rm -f /RISCOS/lumen.ps
$C click 1200 700; $W 1 >/dev/null; $C key A ctrl; $W 1 >/dev/null; $C key P ctrl; $W 4 >/dev/null
$C click 1340 816; $C key U ctrl; $C type 9; $C click 1412 816; $C key U ctrl; $C type 6; $W 1 >/dev/null; $C click 1286 876; $W 1 >/dev/null
$C click 1560 1020; $W 2 >/dev/null; for i in $(seq 18); do $W 10 >/dev/null; done
render 60; python3 -c "
from PIL import Image
p=Image.open('/tmp/lumen-shots/pg1.png'); g=16
c=Image.new('RGB',(p.width+2*g,p.height+2*g),(110,110,110)); c.paste(p,(g,g)); c.save('/build/lumen/docs/guide/images/print-cut.png',optimize=True)"
