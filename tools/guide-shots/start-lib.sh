#!/bin/bash
# Fresh test bed for the library pictures: the Photos directory and IXFS:$.RISCOS open, Lumen running.
ps -eo pid,args | grep '[r]ol-run\|[r]ol-kvm' | awk '{print $1}' | xargs -r kill; sleep 3
cd $HOME/rol
export LUMEN_CMD=$'WimpMode X1920 Y1080 C16M\nFiler_OpenDir IXFS:$.RISCOS.Photos 40 160 1700 300\nFiler_OpenDir IXFS:$.RISCOS 1000 160 1880 300\n/IXFS:$.RISCOS.Apps.!Lumen'
(nohup drv/session.sh 5000 >/dev/null 2>&1 &)
sleep 1; drv/wait.sh 50
