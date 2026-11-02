#include <cstdio>
#include "libraw/libraw.h"
int main(){ const char **l = LibRaw::cameraList(); int n=0; for(;l[n];n++) puts(l[n]); fprintf(stderr,"%d %s\n", LibRaw::cameraCount(), libraw_version()); }
