// Darwin stand-in (dm/android/compat): dispatch semaphores and dispatch_time only (POSIX semaphores, monotonic deadlines)
#pragma once
#include <stdint.h>
#include <stdlib.h>
#include <semaphore.h>
#include <errno.h>
#include <time.h>
#include <mach/mach_time.h>
typedef uint64_t dispatch_time_t;
#define DISPATCH_TIME_NOW 0ull
#define DISPATCH_TIME_FOREVER (~0ull)
#define NSEC_PER_SEC 1000000000ull
#define NSEC_PER_MSEC 1000000ull
#define NSEC_PER_USEC 1000ull
static inline dispatch_time_t dispatch_time(dispatch_time_t when, int64_t delta) {
    if (when == DISPATCH_TIME_FOREVER) return when;
    uint64_t base = when == DISPATCH_TIME_NOW ? mach_absolute_time() : when;
    return delta < 0 && (uint64_t)-delta > base ? 1 : base + (uint64_t)delta;
}
typedef struct { sem_t s; } *dispatch_semaphore_t;
static inline dispatch_semaphore_t dispatch_semaphore_create(long v) { dispatch_semaphore_t d = malloc(sizeof *d); sem_init(&d->s, 0, (unsigned)v); return d; }
static inline long dispatch_semaphore_signal(dispatch_semaphore_t d) { sem_post(&d->s); return 0; }
static inline long dispatch_semaphore_wait(dispatch_semaphore_t d, dispatch_time_t t) {
    if (t == DISPATCH_TIME_FOREVER) { while (sem_wait(&d->s) && errno == EINTR) {} return 0; }
    struct timespec ts = { (time_t)(t / NSEC_PER_SEC), (long)(t % NSEC_PER_SEC) };   // monotonic deadline (bionic's sem_clockwait, API 30)
    while (sem_clockwait(&d->s, CLOCK_MONOTONIC, &ts)) if (errno != EINTR) return 1;
    return 0;
}
static inline void dispatch_release(void *o) { if (o) { sem_destroy(&((dispatch_semaphore_t)o)->s); free(o); } }
