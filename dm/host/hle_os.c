// Host common miscellany: guest object side table, monotonic clock, C library imports that none of the modules define.
// (The former 136 HLE entries for nn::os/mem/lmem/C++ ABI have been deleted: the SDK brings its own nn::os, and at load time not one of these entries was resolved and used.)
#include <pthread.h>
#include "hle.h"
#include <mach/mach_time.h>
#include <stdatomic.h>

// ---------- Side table ----------
#define OM_SIZE (1u << 18)
static struct { const void *k; void *v; } g_om[OM_SIZE];
static pthread_mutex_t g_om_lock = PTHREAD_MUTEX_INITIALIZER;
static unsigned om_h(const void *k) { uint64_t x = (uint64_t)k; x ^= x >> 29; x *= 0xbf58476d1ce4e5b9ull; return (unsigned)(x >> 40) & (OM_SIZE - 1); }
void *objmap_get(const void *k) {
    pthread_mutex_lock(&g_om_lock);
    void *r = NULL;
    for (unsigned i = om_h(k);; i = (i + 1) & (OM_SIZE - 1)) {
        if (g_om[i].k == k) { r = g_om[i].v; break; }
        if (!g_om[i].k) break;
    }
    pthread_mutex_unlock(&g_om_lock);
    return r;
}
void objmap_put(const void *k, void *v) {
    pthread_mutex_lock(&g_om_lock);
    for (unsigned i = om_h(k);; i = (i + 1) & (OM_SIZE - 1))
        if (g_om[i].k == k || !g_om[i].k || g_om[i].k == (void *)1) { g_om[i].k = k; g_om[i].v = v; break; }
    pthread_mutex_unlock(&g_om_lock);
}
void *objmap_del(const void *k) {
    pthread_mutex_lock(&g_om_lock);
    void *r = NULL;
    for (unsigned i = om_h(k);; i = (i + 1) & (OM_SIZE - 1)) {
        if (g_om[i].k == k) { r = g_om[i].v; g_om[i].k = (void *)1; g_om[i].v = NULL; break; }
        if (!g_om[i].k) break;
    }
    pthread_mutex_unlock(&g_om_lock);
    return r;
}
uint64_t now_ns(void) {
    static mach_timebase_info_data_t tb;
    if (!tb.denom) mach_timebase_info(&tb);
    return mach_absolute_time() * tb.numer / tb.denom;
}
// Guest clock: the system counters the guest can read (mrs cntpct gate, svc GetSystemTick) all go through here. Default = now_ns().
// Freeze (only for the test driver DM_TICK_HOLD): while frozen the guest clock does not advance ⇒ the game's next tick never comes due; after unfreezing the frozen duration is subtracted, the guest clock is continuous with no jump
// DM_DETCLOCK=<step ns> (for replay / test runs; product runs do not set it = wall clock): from the first present on the guest clock becomes "advance a fixed step per present", not following the wall clock --
// the client's per-frame dt and the server tick catch-up snapshots are both determined by the present count, so frame N in a run is byte-for-byte comparable between two passes of the same replay (the foundation of byte-for-byte picture proof).
// Before the first frame the wall clock is still used (the boot period has waits that poll by time, freezing would hang them); at the switch it continues from the guest clock value at that time, no jump. It does not advance while frozen.
static _Atomic uint64_t g_gfrozen, g_glag, g_gdet, g_guest_frame; static _Atomic int g_gfz, g_gdet_on; static uint64_t g_gdet_step = 1;
uint64_t dm_guest_frame(void) { return atomic_load(&g_guest_frame); }
static int detclock_cfg(void) {
    static int c = -1; if (c < 0) { const char *e = getenv("DM_DETCLOCK"); g_gdet_step = e ? strtoull(e, 0, 0) : 0; c = g_gdet_step > 0; }
    return c;
}
static uint64_t wall_guest_ns(void) { return atomic_load(&g_gfz) ? atomic_load(&g_gfrozen) : now_ns() - atomic_load(&g_glag); }
uint64_t guest_ns(void) { return atomic_load(&g_gdet_on) ? atomic_load(&g_gdet) : wall_guest_ns(); }
int dm_detclock_enabled(void) { return detclock_cfg() && atomic_load(&g_gdet_on); }
void dm_guest_frame_tick(void) {
    crash_watch_frame();
    atomic_fetch_add(&g_guest_frame, 1);
    if (!detclock_cfg()) return;
    if (!atomic_load(&g_gdet_on)) {
        atomic_store(&g_gdet, wall_guest_ns()); atomic_store(&g_gdet_on, 1);
        LOG("DM_DETCLOCK: from the first present on, the guest clock advances by presents, step %llu ns", (unsigned long long)g_gdet_step); return;
    }
    if (!atomic_load(&g_gfz)) { atomic_fetch_add(&g_gdet, g_gdet_step); dm_guest_clock_advanced(); }
}
static void guest_clock_freeze_apply(int on) {
    if (on && !atomic_load(&g_gfz)) { atomic_store(&g_gfrozen, now_ns() - atomic_load(&g_glag)); atomic_store(&g_gfz, 1); }
    else if (!on && atomic_load(&g_gfz)) { atomic_store(&g_glag, now_ns() - atomic_load(&g_gfrozen)); atomic_store(&g_gfz, 0); }
}
// Freezing has several unrelated reasons (0 freeze tick for testing / loading screen, 1 App in background, 2 system keyboard up): freeze if any holds, release only when all are lifted --
// otherwise "switch to background and back while the keyboard is up" would be released early by the unfreeze on returning to foreground
static pthread_mutex_t g_gfz_mu = PTHREAD_MUTEX_INITIALIZER; static unsigned g_gfz_reasons;
void guest_clock_freeze_for(int reason, int on) {
    pthread_mutex_lock(&g_gfz_mu);
    if (on) g_gfz_reasons |= 1u << reason; else g_gfz_reasons &= ~(1u << reason);
    guest_clock_freeze_apply(g_gfz_reasons != 0);
    pthread_mutex_unlock(&g_gfz_mu);
}
void guest_clock_freeze(int on) { guest_clock_freeze_for(0, on); }
// Entropy source: true random by default (arc4random); with DM_DETCLOCK on it becomes a fixed-seed splitmix64 stream (so that replay pictures are byte-for-byte comparable, the camp blue fire particles differed between two passes).
// The two outlets (libc getentropy, svc GetInfo RandomEntropy) share one stream; the call count is reported in the log (first call and every 1000 calls)
static _Atomic uint64_t g_detrng = 0x5357444554524e47ull; static _Atomic uint64_t g_detrng_n;
uint64_t dm_entropy64(void) {
    if (!detclock_cfg()) return ((uint64_t)arc4random() << 32) | arc4random();
    uint64_t z = atomic_fetch_add(&g_detrng, 0x9E3779B97F4A7C15ull) + 0x9E3779B97F4A7C15ull;
    z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ull; z = (z ^ (z >> 27)) * 0x94D049BB133111EBull; z ^= z >> 31;
    uint64_t n = atomic_fetch_add(&g_detrng_n, 1) + 1; if (n == 1 || n % 1000 == 0) LOG("DM_DETCLOCK: deterministic entropy call %llu", (unsigned long long)n);
    return z;
}
HLE("getentropy", int, g_getentropy, void *p, size_t n) {
    if (!detclock_cfg()) { arc4random_buf(p, n); return 0; }
    for (size_t i = 0; i < n; i += 8) { uint64_t v = dm_entropy64(); memcpy((uint8_t *)p + i, &v, n - i < 8 ? n - i : 8); }
    return 0;
}
// The SDK's own getentropy (Unicorn Overlord, SDK 16.2.4: std::random_device at nnMain) fills through nn::crypto::GenerateCryptographicallyRandomBytes, which opens the csrng service;
// Pokémon Legends: Z-A (SDK 19) calls it first thing in nnMain (main+0x1734b4)
// (InitializeCsrng → sm::Initialize, the game aborted there). The call goes through the sdk's PLT, so the takeover gives it the same stream as getentropy above
HLE_FORCE("_ZN2nn6crypto36GenerateCryptographicallyRandomBytesEPvm", "csrng service; host entropy (the getentropy stream)", void, g_csrng_bytes, void *p, size_t n) {
    g_getentropy(p, n);
}

// ---------- nn::mem::StandardAllocator::Allocate(size): Vanillaware's large blocks aligned to 64 KB (profile key alloc_big_align) ----------
// 13 Sentinels 1.10's own malloc (main 4b632573…, main+0x310550) is StandardAllocator::Allocate(size). It gets the storage of a GPU memory pool from malloc rounded only to 4 KB (main+0x3073b8),
//   then cuts three ring heaps out of the pool at 64 KB alignment relative to the GPU address (main+0x30c118), and the pool size is exactly their sum (main+0x307adc). On the Switch a pool's GPU address is aligned to a big page;
//   the host's GPU address is the CPU address, which is only 4 KB aligned, so up to 60 KB is lost at the start, the third ring heap (16 MB, used by the main thread's allocations while a command buffer is recorded) does not fit and keeps base 0,
//   and the first allocation while recording writes through a null pointer (main+0x2fa9a0, when the prologue enters its first battle).
// Unicorn Overlord 1.05 (main c841ffe2…), the same engine: operator new (main+0x5a55d4) takes the calling thread's ring heap (bump allocator main+0x5b8504: cursor +0x20 starts at the base) and one kept base 0.
//   Past the title screen the first allocation from it returned 0, which operator new's null check skipped, and the next returned 0x200; its constructor (main+0x5a3224 via main+0x4c754) wrote to 0x268.
//   With the profile key, allocations of 64 KB or more are aligned to 64 KB (Allocate(size, 0x10000)), which puts the pool storage on a 64 KB boundary. Other games forward unchanged
static void *(*g_sa_alloc)(void *, size_t), *(*g_sa_alloc_al)(void *, size_t, size_t);
static int g_sa_big_align;
static pthread_once_t g_sa_once = PTHREAD_ONCE_INIT;
static void sa_init(void) {
    module_t *ms = NULL; for (int i = 0; i < g_nmod; i++) if (!strcmp(g_mod[i].name, "sdk")) ms = &g_mod[i];
    g_sa_alloc = ms ? module_sym(ms, "_ZN2nn3mem17StandardAllocator8AllocateEm") : NULL;
    g_sa_alloc_al = ms ? module_sym(ms, "_ZN2nn3mem17StandardAllocator8AllocateEmm") : NULL;
    if (!g_sa_alloc || !g_sa_alloc_al) die("StandardAllocator::Allocate not found in the SDK");
    g_sa_big_align = DM_GAME_ALLOC_BIG_ALIGN;
    if (g_sa_big_align) LOG("StandardAllocator::Allocate: allocations ≥ 64 KB are 64 KB aligned (game profile alloc_big_align: memory pool storage, see hle_os.c)");
}
HLE_FORCE("_ZN2nn3mem17StandardAllocator8AllocateEm", "passed straight to the SDK; with the profile's alloc_big_align (13 Sentinels, Unicorn Overlord) allocations ≥ 64 KB are 64 KB aligned (memory pool start)", void *, h_mem_SAAllocate, void *self, size_t size) {
    pthread_once(&g_sa_once, sa_init);
    return g_sa_big_align && size >= 0x10000 ? g_sa_alloc_al(self, size, 0x10000) : g_sa_alloc(self, size);
}
// Address regions of virtual address memory (Unity's low-level allocator reserves its heap chunks with them): passed straight to the SDK. DM_VAMM_LOG=1 (diagnostic) logs every region
// with its size, address, caller and the live total, to see what fills the alias region (Risk of Rain 2 peaked at 6 GB of address space over 2.25 GB of mapped memory)
static int vamm_log(void) { static int on = -1; if (on < 0) on = getenv("DM_VAMM_LOG") != NULL; return on; }
static struct { uint64_t addr, size; } g_vregs[8192]; static int g_nvregs; static uint64_t g_vlive, g_vpeak; static pthread_mutex_t g_vmu = PTHREAD_MUTEX_INITIALIZER;
static const char *vsite(void *lr, char *buf, size_t n) { module_t *m = lr ? module_of(lr) : NULL; if (m) snprintf(buf, n, "%s+%#llx", m->name, (unsigned long long)((uint8_t *)lr - m->base)); else snprintf(buf, n, "host"); return buf; }
HLE_FORCE("_ZN2nn2os21AllocateAddressRegionEPmm", "address region of virtual address memory; passed to the SDK (DM_VAMM_LOG=1 logs each one)", Result, h_os_AllocRegion, uint64_t *out, uint64_t size) {
    static Result (*orig)(uint64_t *, uint64_t); if (!orig && !(orig = guest_sym("_ZN2nn2os21AllocateAddressRegionEPmm"))) die("sdk lacks os::AllocateAddressRegion");
    Result r = orig(out, size);
    if (vamm_log()) { char b[64]; pthread_mutex_lock(&g_vmu);
        if (!r && g_nvregs < 8192) { g_vregs[g_nvregs].addr = *out; g_vregs[g_nvregs++].size = size; g_vlive += size; if (g_vlive > g_vpeak) g_vpeak = g_vlive; }
        LOG("vamm: AllocateAddressRegion %#llx (%.2f MB) → %s %#llx, caller %s; live %.1f MB, peak %.1f MB", (unsigned long long)size, size / 1048576.0, r ? "FAILED" : "at", r ? (unsigned long long)r : (unsigned long long)*out,
            vsite(__builtin_return_address(0), b, sizeof b), g_vlive / 1048576.0, g_vpeak / 1048576.0);
        pthread_mutex_unlock(&g_vmu); }
    return r;
}
HLE_FORCE("_ZN2nn2os17FreeAddressRegionEm", "address region of virtual address memory; passed to the SDK", Result, h_os_FreeRegion, uint64_t addr) {
    static Result (*orig)(uint64_t); if (!orig && !(orig = guest_sym("_ZN2nn2os17FreeAddressRegionEm"))) die("sdk lacks os::FreeAddressRegion");
    if (vamm_log()) { pthread_mutex_lock(&g_vmu);
        for (int i = 0; i < g_nvregs; i++) if (g_vregs[i].addr == addr) { g_vlive -= g_vregs[i].size; g_vregs[i] = g_vregs[--g_nvregs]; break; }
        LOG("vamm: FreeAddressRegion %#llx; live %.1f MB", (unsigned long long)addr, g_vlive / 1048576.0); pthread_mutex_unlock(&g_vmu); }
    return orig(addr);
}
// Pages committed in those regions (DM_VAMM_LOG=1): split between the largest region (StandardAllocator's block) and the rest, one line per 64 MB change of either
HLE_FORCE("_ZN2nn2os19AllocateMemoryPagesEmm", "commit pages of virtual address memory; passed to the SDK", Result, h_os_AllocPages, uint64_t addr, uint64_t size) {
    static Result (*orig)(uint64_t, uint64_t); if (!orig && !(orig = guest_sym("_ZN2nn2os19AllocateMemoryPagesEmm"))) die("sdk lacks os::AllocateMemoryPages");
    Result r = orig(addr, size);
    if (vamm_log() && !r) { static uint64_t big_at, big_sz, in_big, other, said; pthread_mutex_lock(&g_vmu);
        for (int i = 0; i < g_nvregs; i++) if (g_vregs[i].size > big_sz) { big_sz = g_vregs[i].size; big_at = g_vregs[i].addr; }
        if (addr >= big_at && addr < big_at + big_sz) in_big += size; else other += size;
        if ((in_big + other) / (64u << 20) != said) { said = (in_big + other) / (64u << 20); LOG("vamm: committed %.0f MB in the largest region (%.0f MB), %.0f MB elsewhere", in_big / 1048576.0, big_sz / 1048576.0, other / 1048576.0); }
        pthread_mutex_unlock(&g_vmu); }
    return r;
}
