#!/bin/bash
# Copyright (c) 2026 Andrew Youll
# Released under the MIT licence: see the LICENSE file.
# SPDX-License-Identifier: MIT
# Fresh test bed for the guide with Andrew's photos: 16M colours, IXFS:$.RISCOS.Photos in a wide Filer window, Lumen running.
ps -eo pid,args | grep '[r]ol-run\|[r]ol-kvm' | awk '{print $1}' | xargs -r kill; sleep 3
cd $HOME/rol
export LUMEN_CMD=$'WimpMode X1920 Y1080 C16M\nFiler_OpenDir IXFS:$.RISCOS.Photos 40 160 1700 300\nFiler_Run <Boot$Dir>.^.Printing.!Printers\n/IXFS:$.RISCOS.Apps.!Lumen'
(nohup drv/session.sh 5000 >/dev/null 2>&1 &)
sleep 1; drv/wait.sh 50
