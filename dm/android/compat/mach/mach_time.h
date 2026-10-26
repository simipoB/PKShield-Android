// Darwin stand-in (dm/android/compat): mach_absolute_time in nanoseconds (timebase 1/1)
#pragma once
#include <stdint.h>
#include <time.h>
typedef struct { uint32_t numer, denom; } mach_timebase_info_data_t;
typedef mach_timebase_info_data_t *mach_timebase_info_t;
static inline int mach_timebase_info(mach_timebase_info_t i) { i->numer = 1; i->denom = 1; return 0; }
uint64_t mach_absolute_time(void);   // compat.c: a real symbol, the NVN profiling entry (nvn_entry.S) calls it from assembly
static inline uint64_t mach_continuous_time(void) { struct timespec t; clock_gettime(CLOCK_BOOTTIME, &t); return (uint64_t)t.tv_sec * 1000000000ull + (uint64_t)t.tv_nsec; }
static inline int mach_wait_until(uint64_t deadline) {
    struct timespec t = { (time_t)(deadline / 1000000000ull), (long)(deadline % 1000000000ull) };
    while (clock_nanosleep(CLOCK_MONOTONIC, TIMER_ABSTIME, &t, 0) == 4 /* EINTR */) {}
    return 0;
}
