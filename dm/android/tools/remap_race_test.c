// The pool move and mirrored-memory paths (nvn_vk.c mtl_pool_add / mtl_pool_del on Android): whole driver mappings move onto game pools, while larger/suballocated mappings
// stay separate and are synchronized by copies. Thousands of pool make / free cycles run while other threads keep mapping and unmapping memory. Usage: remap_race_test [cycles]
#define _GNU_SOURCE
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>
static volatile int g_run = 1; static long g_bad;
static void *churn(void *p) {   // another thread: maps, writes, checks, unmaps
    (void)p; unsigned seed = (unsigned)(uintptr_t)&p;
    while (g_run) { size_t n = (size_t)(rand_r(&seed) % 64 + 1) * 4096; unsigned char *m = mmap(NULL, n, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
        if (m == MAP_FAILED) continue;
        memset(m, 0x5a, n); usleep(50);
        for (size_t i = 0; i < n; i += 4096) if (m[i] != 0x5a) { __sync_fetch_and_add(&g_bad, 1); break; }   // someone unmapped / replaced our mapping
        munmap(m, n); }
    return NULL;
}
static int mirror_roundtrip(size_t pg) {
    size_t n = 3 * pg; unsigned char *pool = mmap(NULL, n, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    unsigned char *drv = mmap(NULL, n + pg, PROT_READ | PROT_WRITE, MAP_SHARED | MAP_ANONYMOUS, -1, 0);
    if (pool == MAP_FAILED || drv == MAP_FAILED) { if (pool != MAP_FAILED) munmap(pool, n); if (drv != MAP_FAILED) munmap(drv, n + pg); return 1; }
    memset(pool, 0x33, n); memcpy(drv, pool, n);   // initial pool contents
    pool[pg] = 0xa5; memcpy(drv, pool, n);         // game writes copied in before GPU submission
    int fails = drv[pg] != 0xa5;
    drv[2 * pg] = 0x5a; memcpy(pool, drv, n);     // GPU writes copied back after completion
    if (pool[pg] != 0xa5 || pool[2 * pg] != 0x5a) fails++;
    munmap(drv, n + pg); munmap(pool, n); return fails;
}
int main(int argc, char **argv) {
    int cycles = argc > 1 ? atoi(argv[1]) : 20000; pthread_t t[6];
    for (int i = 0; i < 6; i++) pthread_create(&t[i], NULL, churn, NULL);
    long fails = 0, dontunmap_refused = 0; size_t pg = (size_t)getpagesize();
    int mirror_fails = mirror_roundtrip(pg); fails += mirror_fails;
    for (int c = 0; c < cycles; c++) {
        size_t n = (size_t)(c % 37 + 1) * pg;
        unsigned char *pool = mmap(NULL, n, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0); memset(pool, c & 0xff, n);   // the game's pool
        unsigned char *drv = mmap(NULL, n, PROT_READ | PROT_WRITE, MAP_SHARED | MAP_ANONYMOUS, -1, 0);                              // the driver's mapping
        // pool add (nvn_vk.c): fill, move the driver's pages onto the pool leaving the old range mapped (MREMAP_DONTUNMAP), that range becomes the placeholder
        memcpy(drv, pool, n);
        if (mremap(drv, n, n, MREMAP_MAYMOVE | MREMAP_FIXED | 4 /* MREMAP_DONTUNMAP */, pool) != pool) { fails++; dontunmap_refused++; continue; }
        mmap(drv, n, PROT_NONE, MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED | MAP_NORESERVE, -1, 0);
        pool[n - 1] ^= 0xff;   // the game writes through the pool (shared with the "GPU")
        // pool del: the pool's pages replaced by anonymous memory with the same content in one step
        unsigned char *keep = malloc(n); memcpy(keep, pool, n);
        mmap(pool, n, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED, -1, 0); memcpy(pool, keep, n); free(keep);
        if (pool[0] != (unsigned char)(c & 0xff) || pool[n - 1] != (unsigned char)((c & 0xff) ^ 0xff)) fails++;
        munmap(drv, n);   // the driver's vkFreeMemory unmaps its range (the placeholder)
        munmap(pool, n);  // the game frees its memory
    }
    g_run = 0; for (int i = 0; i < 6; i++) pthread_join(t[i], NULL);
    printf("%d cycles: %ld content / move failures (%ld MREMAP_DONTUNMAP refusals), %d mirrored-memory failures, %ld other-thread mappings damaged\n", cycles, fails, dontunmap_refused, mirror_fails, g_bad);
    return fails || g_bad;
}
