// Darwin stand-in (dm/android/compat): clonefile as a plain recursive copy (Android's file systems have no copy-on-write clones). Same contract: the destination must not exist;
// a directory is copied with its whole tree, file modes kept. Implemented in compat.c
#pragma once
#define CLONE_NOFOLLOW 1
#define CLONE_NOOWNERCOPY 2
int clonefile(const char *src, const char *dst, unsigned flags);
