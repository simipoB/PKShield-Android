// Unreal Engine's allocator on a host malloc zone. Octopath Traveler 0's engine allocator is an FMallocBinned3 constructed in place in nninitStartup; its constructor reserves 37 GB of
// address space (74 pools of 512 MB), which an iPhone cannot give (about 5 GB without the extended virtual addressing entitlement), and its pools are too uneven to shrink (patch_nso.py
// CUTS: the constructor call is cut). The host puts this vtable in the object instead: every FMemory allocation reaches it through the object (FPlatformMemory::BaseAllocator returns it,
// and the thread-local storage allocator calls its Malloc / Free slots), and the guest's own trivial functions stay in the slots that do nothing.
// The FMalloc slots (the engine's FMalloc interface, measured from the vtable and the thread-local callbacks): 0-1 destructors, 2-5 FExec, 6 Malloc, 7 TryMalloc, 8 Realloc, 9 TryRealloc, 10 Free, 11 QuantizeSize, 12 GetAllocationSize, 13 Trim, 14 SetupTLSCachesOnCurrentThread,
// 15 ClearAndDisableTLSCachesOnCurrentThread, 16 InitializeStatsMetadata, 17 UpdateStats, 18 GetAllocatorStats, 19 DumpAllocatorStats, 20 IsInternallyThreadSafe, 21 ValidateHeap,
// 22 GetDescriptiveName, 23-25 OnMallocInitialized / OnPreFork / OnPostFork (the thread-local callbacks main+0x12e80 / main+0x12ec0 call +0x30 and +0x50).
// Fixed addresses: each row runs only on the main build it was found in (AGENTS.md)
#include "hle.h"
#include <malloc/malloc.h>
#include <stdatomic.h>

static const struct { const char *build, *what; uint64_t obj, vtable, cut; } k_ue[] = {
    // Octopath Traveler 0 1.0.5: the FMallocBinned3 object (main+0x68022c8, bss), its vtable (main+0x5b06f38, 26 slots), the cut constructor call (main+0x12e40, a nop after the rewrite)
    { "9fe60a212cff9b9a3656ab63d4f882e5", "Octopath Traveler 0 1.0.5", 0x68022c8, 0x5b06f38, 0x12e40 },
};
enum { NSLOT = 26 };

static malloc_zone_t *g_zone;
static _Atomic int64_t g_used, g_peak;   // bytes in use (usable sizes) and their high-water mark, for the [mem] line (one per 256 MB of new peak)
static void used(int64_t d) {
    int64_t after = atomic_fetch_add(&g_used, d) + d, peak = atomic_load(&g_peak);
    while (after > peak && !atomic_compare_exchange_weak(&g_peak, &peak, after)) {}
    if (after > peak && after >> 28 != peak >> 28) LOG("[mem] Unreal heap (host malloc zone) up to %.2f GB in use", after / 1073741824.0);
}
static size_t align_of(uint32_t a) { return a > 16 ? a : 16; }   // 0 = DEFAULT_ALIGNMENT, 16 bytes as Binned3 gives
static void *alloc(size_t n, uint32_t a) {
    void *p = align_of(a) == 16 ? malloc_zone_malloc(g_zone, n ? n : 1) : malloc_zone_memalign(g_zone, align_of(a), n ? n : 1);
    if (p) used((int64_t)malloc_size(p));
    return p;
}
static void release(void *p) { if (p) { used(-(int64_t)malloc_size(p)); malloc_zone_free(g_zone, p); } }
static void *realloc_aligned(void *p, size_t n, uint32_t a) {
    if (!p) return alloc(n, a);
    if (!n) { release(p); return NULL; }
    size_t old = malloc_size(p);
    if (align_of(a) == 16) { void *q = malloc_zone_realloc(g_zone, p, n); if (q) used((int64_t)malloc_size(q) - (int64_t)old); return q; }
    if (!((uintptr_t)p & (align_of(a) - 1)) && old >= n) return p;
    void *q = alloc(n, a); if (!q) return NULL;
    memcpy(q, p, old < n ? old : n); release(p); return q;
}

static void *m_Malloc(void *self, size_t n, uint32_t a) {
    (void)self; void *p = alloc(n, a);
    if (!p) die("Unreal heap: out of memory (%zu bytes, alignment %u, %.2f GB in use)", n, a, atomic_load(&g_used) / 1073741824.0);
    return p;
}
static void *m_TryMalloc(void *self, size_t n, uint32_t a) { (void)self; return alloc(n, a); }
static void *m_Realloc(void *self, void *p, size_t n, uint32_t a) {
    (void)self; void *q = realloc_aligned(p, n, a);
    if (!q && n) die("Unreal heap: out of memory in Realloc (%zu bytes, alignment %u, %.2f GB in use)", n, a, atomic_load(&g_used) / 1073741824.0);
    return q;
}
static void *m_TryRealloc(void *self, void *p, size_t n, uint32_t a) { (void)self; return realloc_aligned(p, n, a); }
static void m_Free(void *self, void *p) { (void)self; release(p); }
static size_t m_QuantizeSize(void *self, size_t n, uint32_t a) { (void)self; (void)a; return malloc_good_size(n); }
static bool m_GetAllocationSize(void *self, void *p, size_t *out) { (void)self; if (!p) return false; *out = malloc_size(p); return true; }
static void m_Nothing(void *self) { (void)self; }   // Trim (its bool argument is ignored), the thread-local cache setup and teardown, DumpAllocatorStats
static void *g_vt[NSLOT];

// Called once the modules are loaded and relocated, before any guest code runs
void dm_ue_malloc_install(void) {
    for (int i = 0; i < g_nmod; i++) if (!strcmp(g_mod[i].name, "main"))
        for (int k = 0; k < (int)(sizeof k_ue / sizeof k_ue[0]); k++) if (!strcmp(g_mod[i].build_id, k_ue[k].build)) {
            uint8_t *b = g_mod[i].base;
            if (*(uint32_t *)(b + k_ue[k].cut) != 0xd503201f) { LOG("[RED] %s: main is rewritten without the allocator cut (patch_nso.py): Unreal's own FMallocBinned3 runs", k_ue[k].what); return; }
            memcpy(g_vt, b + k_ue[k].vtable, sizeof g_vt);   // the relocated vtable: the trivial slots stay the guest's
            g_vt[6] = (void *)m_Malloc; g_vt[7] = (void *)m_TryMalloc; g_vt[8] = (void *)m_Realloc; g_vt[9] = (void *)m_TryRealloc; g_vt[10] = (void *)m_Free;
            g_vt[11] = (void *)m_QuantizeSize; g_vt[12] = (void *)m_GetAllocationSize;
            g_vt[13] = g_vt[14] = g_vt[15] = g_vt[19] = (void *)m_Nothing;
            g_zone = malloc_create_zone(0, 0); malloc_set_zone_name(g_zone, "guest Unreal heap");
            *(void **)(b + k_ue[k].obj) = g_vt;
            LOG("%s: Unreal's allocator (FMallocBinned3 at main+%#llx) runs on a host malloc zone", k_ue[k].what, (unsigned long long)k_ue[k].obj);
        }
}
