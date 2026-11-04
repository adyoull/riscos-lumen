#!/bin/bash
# shot.sh NAME : full screenshot to $HOME/guide/shots/NAME.png plus a half-size preview NAME-s.png
python3 $HOME/rol/drv/ctl.py shot $HOME/guide/shots/$1.png
python3 -c "
from PIL import Image; Image.open('$HOME/guide/shots/$1.png').resize((960,540)).save('$HOME/guide/shots/$1-s.png')"
