// Darwin stand-in (dm/android/compat): Darwin's extended-attribute calls (with position / options) on Linux's (names live in the user. namespace)
#pragma once
#include_next <sys/xattr.h>
#include <errno.h>
#define XATTR_NOFOLLOW 1
#ifndef ENOATTR
#define ENOATTR ENODATA
#endif
#define getxattr(p, n, v, s, pos, opt) lgetxattr(p, "user." n, v, s)
#define setxattr(p, n, v, s, pos, opt) lsetxattr(p, "user." n, v, s, 0)
#define removexattr(p, n, opt) lremovexattr(p, "user." n)
