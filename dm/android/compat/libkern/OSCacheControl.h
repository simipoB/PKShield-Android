// Darwin stand-in (dm/android/compat)
#pragma once
#include <stddef.h>
static inline void sys_icache_invalidate(void *p, size_t n) { __builtin___clear_cache((char *)p, (char *)p + n); }
static inline void sys_dcache_flush(void *p, size_t n) { (void)p; (void)n; }
