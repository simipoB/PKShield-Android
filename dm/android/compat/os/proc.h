// Darwin stand-in (dm/android/compat)
#pragma once
#include <stddef.h>
#include <unistd.h>
static inline size_t os_proc_available_memory(void) { return (size_t)sysconf(_SC_AVPHYS_PAGES) * (size_t)sysconf(_SC_PAGESIZE); }
