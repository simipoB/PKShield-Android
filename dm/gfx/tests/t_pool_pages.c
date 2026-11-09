// Giving a pool's pages back to the game (nvn/pool_pages.h) while another thread uses them: the way mtl_pool_del does it on the mremap route (the driver's pages
// moved onto the pool, then back to the driver's address) with a "driver" mapping made from a memfd, and a thread that keeps reading the pool's pages the whole time
// (Pokémon Shield's allocator reuses memory around pools on other threads: #54). Each read must find the content, never a fault and never zeros. The old way (fresh
// zero pages, then the content) runs first, to show the reader catches its window.
// cc -O2 -pthread -I../nvn -o /tmp/t_pool_pages t_pool_pages.c && /tmp/t_pool_pages
#define _GNU_SOURCE
#include <pthread.h>
#include <setjmp.h>
#include <signal.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/mman.h>
#include <unistd.h>
#include "pool_pages.h"

enum { PG = 4096, POOL = 64 * PG, ROUNDS = 3000 };
static uint8_t *g_pool;                       // the pool's pages, inside the game's heap
static atomic_int g_stop, g_reads, g_faults, g_wrong;
static __thread sigjmp_buf t_jb; static __thread volatile int t_armed;
static void on_segv(int s, siginfo_t *si, void *uc) {   // as the engine's crash handler: the page fault hooks first (pp_fault), then the crash
    (void)s; (void)uc;
    if (pp_fault(si->si_addr)) return;
    if (t_armed) siglongjmp(t_jb, 1);
    _exit(99);
}
static void *reader(void *u) {
    (void)u; volatile uint32_t sink = 0;
    while (!atomic_load(&g_stop)) for (int k = 0; k < POOL / PG && !atomic_load(&g_stop); k++) {
        volatile uint32_t *w = (volatile uint32_t *)(g_pool + k * PG);
        t_armed = 1;
        if (sigsetjmp(t_jb, 1)) { t_armed = 0; atomic_fetch_add(&g_faults, 1); continue; }
        uint32_t v = *w; t_armed = 0; sink += v;
        if (v != 0xC0DE0000u + (uint32_t)k) atomic_fetch_add(&g_wrong, 1);
        atomic_fetch_add(&g_reads, 1);
    }
    return NULL;
}
static int g_memfd;
// the driver's pages onto the pool (mtl_pool_add): content copied in, moved over the pool, a placeholder where they were
static uint8_t *take(void) {
    uint8_t *p = mmap(NULL, POOL, PROT_READ | PROT_WRITE, MAP_SHARED, g_memfd, 0);
    memcpy(p, g_pool, POOL);
    if (mremap(p, POOL, POOL, MREMAP_MAYMOVE | MREMAP_FIXED, g_pool) != g_pool) { perror("mremap take"); exit(2); }
    mmap(p, POOL, PROT_NONE, MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED | MAP_NORESERVE, -1, 0);
    return p;
}
static void give_back_old(uint8_t *to) {   // as mtl_pool_del did: copy, move the driver's pages back, fresh pages, then the content
    void *keep = malloc(POOL); memcpy(keep, g_pool, POOL);
    if (mremap(g_pool, POOL, POOL, MREMAP_MAYMOVE | MREMAP_FIXED, to) != to) { perror("mremap back"); exit(2); }
    mmap(g_pool, POOL, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED, -1, 0);
    memcpy(g_pool, keep, POOL); free(keep);
    munmap(to, POOL);
}
static int g_one_step;
static void give_back_new(uint8_t *to) {   // as mtl_pool_del does now: the copy first, the driver's pages back, the copy over the pool in one step
    void *copy = pages_copy((uintptr_t)g_pool, POOL);
    int s = pp_begin((uintptr_t)g_pool, POOL);
    if (mremap(g_pool, POOL, POOL, MREMAP_MAYMOVE | MREMAP_FIXED, to) != to) { perror("mremap back"); exit(2); }
    g_one_step += pages_back((uintptr_t)g_pool, POOL, copy);
    pp_end(s);
    munmap(to, POOL);
}
static int checks, fails;
#define CHECK(c, s) do { checks++; int ok_ = !!(c); fails += !ok_; printf("%s %s\n", ok_ ? "✓" : "✗", s); } while (0)
static void run(void (*give_back)(uint8_t *), int *faults, int *wrong, int *reads) {
    atomic_store(&g_stop, 0); atomic_store(&g_faults, 0); atomic_store(&g_wrong, 0); atomic_store(&g_reads, 0);
    pthread_t th; pthread_create(&th, NULL, reader, NULL);
    for (int r = 0; r < ROUNDS; r++) give_back(take());
    atomic_store(&g_stop, 1); pthread_join(th, NULL);
    *faults = g_faults; *wrong = g_wrong; *reads = g_reads;
}
int main(void) {
    struct sigaction sa = { .sa_sigaction = on_segv, .sa_flags = SA_SIGINFO }; sigemptyset(&sa.sa_mask); sigaction(SIGSEGV, &sa, NULL); sigaction(SIGBUS, &sa, NULL);
    g_memfd = memfd_create("driver", 0); if (g_memfd < 0 || ftruncate(g_memfd, POOL)) { perror("memfd"); return 2; }
    uint8_t *heap = mmap(NULL, POOL + 2 * PG, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);   // the game's heap, the pool one page in
    g_pool = heap + PG;
    for (int k = 0; k < POOL / PG; k++) *(uint32_t *)(g_pool + k * PG) = 0xC0DE0000u + (uint32_t)k;
    int f, w, n;
    run(give_back_old, &f, &w, &n);
    printf("  old way, %d give-backs: %d reads, %d faults, %d wrong values\n", ROUNDS, n, f, w);
    run(give_back_new, &f, &w, &n);
    printf("  new way, %d give-backs: %d reads, %d faults, %d wrong values (%d of them in one step)\n", ROUNDS, n, f, w, g_one_step);
    CHECK(n > 0 && f == 0 && w == 0, "new way: every read during the give-backs found the content (no fault, no zeros)");
    CHECK(g_one_step == ROUNDS, "new way: every give-back went in one mremap");
    int same = 1; for (int k = 0; k < POOL / PG; k++) if (*(uint32_t *)(g_pool + k * PG) != 0xC0DE0000u + (uint32_t)k) same = 0;
    CHECK(same, "the pool's content is unchanged after all the moves");
    printf("%d checks, %d failed\n", checks, fails);
    return fails != 0;
}
