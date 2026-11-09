// Framebuffers of render passes on drivers without dynamic rendering (nvn_vk.c, rend_begin), kept and found again by render pass, views and size. Sword and Shield open
// about 75 passes a frame onto the same few targets, and a framebuffer made (and destroyed) for each one cost a vkCreateFramebuffer every pass (#99: the Adreno 613's own
// Vulkan 1.1 driver, twice the CPU time per draw of the normal path). An entry goes when one of its views is destroyed (fbc_forget, just before the view: the driver may
// hand the same handle value to a new view, which must not find this framebuffer), or, when the cache is full, the one used longest ago, after its submission.
// The includer defines FBC_CREATE(rp, views, nv, w, h, layers) -> VkFramebuffer, FBC_DESTROY(fb) (now) and FBC_DEFER(fb) (once the work recorded so far is done).
// Test: dm/gfx/tests/t_fb_cache.c
#ifndef FB_CACHE_H
#define FB_CACHE_H
#include <pthread.h>
#include <stdint.h>
#include <string.h>
typedef struct { VkRenderPass rp; VkImageView v[9]; uint32_t nv, w, h, layers; VkFramebuffer fb; uint64_t used; } fbc_ent;
#ifndef FBC_N
#define FBC_N 256
#endif
static fbc_ent g_fbc[FBC_N]; static int g_nfbc; static uint64_t g_fbc_tick, g_fbc_hit, g_fbc_made;
static pthread_mutex_t g_fbcmu = PTHREAD_MUTEX_INITIALIZER;   // (may be taken while the caller holds its own deferred-destroy lock: never held while calling FBC_DEFER)
static void fbc_forget(VkImageView v) {
    if (!v) return;
    pthread_mutex_lock(&g_fbcmu);
    for (int i = 0; i < g_nfbc;) {
        int has = 0; for (uint32_t j = 0; j < g_fbc[i].nv; j++) has |= g_fbc[i].v[j] == v;
        if (has) { FBC_DESTROY(g_fbc[i].fb); g_fbc[i] = g_fbc[--g_nfbc]; } else i++;
    }
    pthread_mutex_unlock(&g_fbcmu);
}
static VkFramebuffer fbc_get(VkRenderPass rp, const VkImageView *views, uint32_t nv, uint32_t w, uint32_t h, uint32_t layers) {
    fbc_ent k; memset(&k, 0, sizeof k); k.rp = rp; if (nv > 9) nv = 9; memcpy(k.v, views, nv * sizeof *views); k.nv = nv; k.w = w; k.h = h; k.layers = layers;
    pthread_mutex_lock(&g_fbcmu);
    for (int i = 0; i < g_nfbc; i++) { fbc_ent *e = &g_fbc[i];
        if (e->rp == rp && e->nv == nv && e->w == w && e->h == h && e->layers == layers && !memcmp(e->v, k.v, sizeof k.v)) {
            e->used = ++g_fbc_tick; g_fbc_hit++; VkFramebuffer fb = e->fb; pthread_mutex_unlock(&g_fbcmu); return fb; } }
    pthread_mutex_unlock(&g_fbcmu);
    k.fb = FBC_CREATE(rp, views, nv, w, h, layers);
    VkFramebuffer old = 0;
    pthread_mutex_lock(&g_fbcmu);
    g_fbc_made++; k.used = ++g_fbc_tick;
    if (g_nfbc < FBC_N) g_fbc[g_nfbc++] = k;
    else { int lru = 0; for (int i = 1; i < g_nfbc; i++) if (g_fbc[i].used < g_fbc[lru].used) lru = i; old = g_fbc[lru].fb; g_fbc[lru] = k; }
    pthread_mutex_unlock(&g_fbcmu);
    if (old) FBC_DEFER(old);   // (passes already recorded may use it: it goes once their work is done)
    return k.fb;
}
#endif
