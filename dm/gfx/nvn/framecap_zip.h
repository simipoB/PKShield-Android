// A frame capture's files (nvn_vk.c, frame capture) into one ZIP a player can attach to a bug report: pictures as PNG, notes as text
#pragma once
#include <stddef.h>
#include <stdint.h>
typedef struct fczip fczip;
fczip *fczip_open(const char *path);
int fczip_add(fczip *z, const char *name, const void *data, size_t n);
int fczip_add_png(fczip *z, const char *name, const uint8_t *rgba, int w, int h);   // w x h RGBA8 rows, tightly packed
int fczip_close(fczip *z);   // writes the directory; 1 if every write went through
