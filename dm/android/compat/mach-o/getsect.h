// Darwin stand-in (dm/android/compat): the host's registration sections (DM_SECT in dm.h) read through the ELF linker's __start_ / __stop_ symbols
#pragma once
#include <string.h>
extern const char __start_swhle[] __attribute__((weak)), __stop_swhle[] __attribute__((weak));
extern const char __start_swforce[] __attribute__((weak)), __stop_swforce[] __attribute__((weak));
extern const char __start_swnvn[] __attribute__((weak)), __stop_swnvn[] __attribute__((weak));
struct mach_header_64;
extern const struct mach_header_64 _mh_execute_header;
static inline const void *getsectiondata(const struct mach_header_64 *mh, const char *seg, const char *sect, unsigned long *size) {
    (void)mh; (void)seg; const char *a = 0, *b = 0;
    if (!strcmp(sect, "swhle")) a = __start_swhle, b = __stop_swhle;
    else if (!strcmp(sect, "swforce")) a = __start_swforce, b = __stop_swforce;
    else if (!strcmp(sect, "swnvn")) a = __start_swnvn, b = __stop_swnvn;
    *size = a && b ? (unsigned long)(b - a) : 0; return *size ? a : 0;
}
