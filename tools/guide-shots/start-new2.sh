#!/bin/bash
# Second session: the RISCOS directory open (Photos is imported whole, as one project), printers running.
ps -eo pid,args | grep '[r]ol-run\|[r]ol-kvm' | awk '{print $1}' | xargs -r kill; sleep 3
cd /home/claude/rol
export LUMEN_CMD=$'WimpMode X1920 Y1080 C16M\nFiler_OpenDir IXFS:$.RISCOS 40 160 1700 300\nFiler_Run <Boot$Dir>.^.Printing.!Printers\n/IXFS:$.RISCOS.Apps.!Lumen'
(nohup drv/session.sh 5000 >/dev/null 2>&1 &)
sleep 1; drv/wait.sh 50
