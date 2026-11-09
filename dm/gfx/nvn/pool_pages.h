// Giving a memory pool's pages back to the game (nvn_vk.c, mtl_pool_del) without a moment where another thread finds them unmapped or zeroed.
// The game's allocator hands out memory next to and inside pools on other threads while a pool is finalized: Pokémon Shield's file loader crashed writing a heap
// block header into a page that was unmapped at that moment (#54, Xclipse 940, where every give-back took the slow path). The old way mapped fresh zero pages over
// the range and copied the content in afterwards (milliseconds for a large pool); now a private copy is made first and moved over the range in one mremap, which
// holds the address space's lock throughout. Test: dm/gfx/tests/t_pool_pages.c
#ifndef POOL_PAGES_H
#define POOL_PAGES_H
#include <errno.h>
#include <stdatomic.h>
#include <stdint.h>
#include <string.h>
#include <sys/mman.h>
#include <time.h>

// The one moment left: between the driver's pages leaving the range and the copy arriving (two mremaps back to back; no single call does both). The range is marked
// for it (pp_begin / pp_end), and a fault inside a marked range, or one that ended less than 100 ms ago, waits 50 µs and runs the faulting instruction again
// (pp_fault, called from the page fault hooks: dm_fault_hook_register). A fault anywhere else is left to the crash handler as before
typedef struct { _Atomic uintptr_t lo, hi; _Atomic int64_t end_ns; } pp_range;   // end_ns 0: still going
static pp_range g_pp[16]; static atomic_uint g_pp_next;
static int64_t pp_now(void) { struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t); return (int64_t)t.tv_sec * 1000000000 + t.tv_nsec; }
static int pp_begin(uintptr_t lo, size_t size) {
    int s = (int)(atomic_fetch_add(&g_pp_next, 1) & 15);
    atomic_store(&g_pp[s].end_ns, 0); atomic_store(&g_pp[s].hi, lo + size); atomic_store(&g_pp[s].lo, lo);
    return s;
}
static void pp_end(int s) { atomic_store(&g_pp[s].end_ns, pp_now()); }
static int pp_fault(void *addr) {   // async-signal-safe: atomics, clock_gettime and nanosleep only
    uintptr_t a = (uintptr_t)addr; int64_t now = 0;
    for (int s = 0; s < 16; s++) {
        uintptr_t lo = atomic_load(&g_pp[s].lo), hi = atomic_load(&g_pp[s].hi);
        if (!lo || a < lo || a >= hi) continue;
        int64_t end = atomic_load(&g_pp[s].end_ns); if (end && !now) now = pp_now();
        if (end && now - end > 100000000) continue;
        struct timespec w = { 0, 50000 }; nanosleep(&w, NULL);
        return 1;
    }
    return 0;
}

// a private copy of the pages at [base, base+size), for pages_back (MAP_FAILED when there is no memory for it)
static void *pages_copy(uintptr_t base, size_t size) {
    void *c = mmap(NULL, size, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (c != MAP_FAILED) memcpy(c, (void *)base, size);
    return c;
}
// [base, base+size) becomes the copy (ordinary memory, readable and writable), whatever was mapped there; the copy's own address is gone afterwards.
// Returns 1 when it went in one step, 0 when the fallback (fresh pages, then the content) was needed
static int pages_back(uintptr_t base, size_t size, void *copy) {
#ifdef __linux__
    if (mremap(copy, size, size, MREMAP_MAYMOVE | MREMAP_FIXED, (void *)base) == (void *)base) return 1;
#endif
    mmap((void *)base, size, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED, -1, 0);
    memcpy((void *)base, copy, size); munmap(copy, size);
    return 0;
}
#endif
