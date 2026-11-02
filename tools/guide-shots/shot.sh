#!/bin/bash
# shot.sh NAME : full screenshot to /home/claude/guide/shots/NAME.png plus a half-size preview NAME-s.png
python3 /home/claude/rol/drv/ctl.py shot /home/claude/guide/shots/$1.png
python3 -c "
from PIL import Image; Image.open('/home/claude/guide/shots/$1.png').resize((960,540)).save('/home/claude/guide/shots/$1-s.png')"
