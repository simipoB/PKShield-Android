// The render pass framebuffer cache (nvn/fb_cache.h) with fake handles: found again for the same render pass, views and size; never for another; gone with any of its
// views (and a view handle given out again after that gets a new framebuffer, not the old one); the one used longest ago evicted, deferred rather than destroyed.
// Then 200,000 random passes and view destructions checked against what each returned framebuffer was made for.
// cc -O2 -pthread -I../nvn -o /tmp/t_fb_cache t_fb_cache.c && /tmp/t_fb_cache
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
typedef struct rp_ *VkRenderPass; typedef struct iv_ *VkImageView; typedef struct fb_ *VkFramebuffer;
enum { MAXFB = 1 << 20 };
static struct { VkRenderPass rp; VkImageView v[9]; uint32_t nv, w, h, l; int state; } F[MAXFB];   // state 1 live, 2 destroyed, 3 deferred
static int nfb, ndestroyed, ndeferred;
static VkFramebuffer fake_create(VkRenderPass rp, const VkImageView *v, uint32_t nv, uint32_t w, uint32_t h, uint32_t l) {
    int i = ++nfb; F[i].rp = rp; for (uint32_t j = 0; j < nv; j++) F[i].v[j] = v[j]; F[i].nv = nv; F[i].w = w; F[i].h = h; F[i].l = l; F[i].state = 1;
    return (VkFramebuffer)(uintptr_t)i;
}
static void fake_destroy(VkFramebuffer fb) { int i = (int)(uintptr_t)fb; if (F[i].state == 1) { F[i].state = 2; ndestroyed++; } else F[i].state = -1; }
static void fake_defer(VkFramebuffer fb) { int i = (int)(uintptr_t)fb; if (F[i].state == 1) { F[i].state = 3; ndeferred++; } else F[i].state = -1; }
#define FBC_N 8
#define FBC_CREATE(rp, v, nv, w, h, l) fake_create(rp, v, nv, w, h, l)
#define FBC_DESTROY(fb) fake_destroy(fb)
#define FBC_DEFER(fb) fake_defer(fb)
#include "fb_cache.h"

static int checks, fails;
#define CHECK(c, s) do { checks++; int ok_ = !!(c); fails += !ok_; printf("%s %s\n", ok_ ? "✓" : "✗", s); } while (0)
#define RP(n) ((VkRenderPass)(uintptr_t)(0x100 + (n)))
#define IV(n) ((VkImageView)(uintptr_t)(0x1000 + (n)))
static int matches(VkFramebuffer fb, VkRenderPass rp, const VkImageView *v, uint32_t nv, uint32_t w, uint32_t h, uint32_t l) {
    int i = (int)(uintptr_t)fb; if (i <= 0 || i > nfb || F[i].state != 1 || F[i].rp != rp || F[i].nv != nv || F[i].w != w || F[i].h != h || F[i].l != l) return 0;
    for (uint32_t j = 0; j < nv; j++) if (F[i].v[j] != v[j]) return 0;
    return 1;
}
static uint32_t rs = 777u; static uint32_t rnd(void) { rs ^= rs << 13; rs ^= rs >> 17; rs ^= rs << 5; return rs; }

int main(void) {
    VkImageView a[2] = { IV(1), IV(2) }, b[2] = { IV(1), IV(3) };
    VkFramebuffer f1 = fbc_get(RP(1), a, 2, 1280, 720, 1), f2 = fbc_get(RP(1), a, 2, 1280, 720, 1);
    CHECK(f1 == f2 && nfb == 1 && g_fbc_hit == 1, "the same render pass, views and size: the same framebuffer, made once");
    VkFramebuffer f3 = fbc_get(RP(1), b, 2, 1280, 720, 1), f4 = fbc_get(RP(1), a, 2, 640, 360, 1), f5 = fbc_get(RP(2), a, 2, 1280, 720, 1), f6 = fbc_get(RP(1), a, 2, 1280, 720, 6), f7 = fbc_get(RP(1), a, 1, 1280, 720, 1);
    CHECK(f3 != f1 && f4 != f1 && f5 != f1 && f6 != f1 && f7 != f1 && nfb == 6, "another view, size, render pass, layer count or attachment count: a framebuffer of its own");
    fbc_forget(IV(2));
    CHECK(F[(uintptr_t)f1].state == 2 && F[(uintptr_t)f4].state == 2 && F[(uintptr_t)f5].state == 2 && F[(uintptr_t)f6].state == 2 && F[(uintptr_t)f3].state == 1 && F[(uintptr_t)f7].state == 1,
          "a view destroyed: every framebuffer naming it destroyed, the others kept");
    VkFramebuffer f8 = fbc_get(RP(1), a, 2, 1280, 720, 1);   // IV(2)'s handle value handed out again for a new view
    CHECK(f8 != f1 && matches(f8, RP(1), a, 2, 1280, 720, 1), "the handle given out again for a new view: a new framebuffer, not the destroyed one");
    // eviction: fill the 8 slots, keep using the first, add one more
    for (int k = 0; k < 8; k++) { VkImageView v = IV(100 + k); fbc_get(RP(3), &v, 1, 64, 64, 1); }
    VkImageView keep = IV(100); VkFramebuffer fk = fbc_get(RP(3), &keep, 1, 64, 64, 1);
    int before_def = ndeferred; VkImageView extra = IV(200); fbc_get(RP(3), &extra, 1, 64, 64, 1);
    CHECK(ndeferred == before_def + 1 && fbc_get(RP(3), &keep, 1, 64, 64, 1) == fk, "cache full: one framebuffer evicted (deferred, not destroyed at once), the one just used kept");
    // random passes and view destructions against what each framebuffer was made for
    int bad = 0, reuse_bad = 0;
    for (int it = 0; it < 200000; it++) {
        if (rnd() % 10 == 0) { fbc_forget(IV(rnd() % 24)); continue; }
        uint32_t nv = 1 + rnd() % 3; VkImageView v[3]; for (uint32_t j = 0; j < nv; j++) v[j] = IV(rnd() % 24);
        VkRenderPass rp = RP(rnd() % 3); uint32_t w = 64u << (rnd() % 2), l = 1 + rnd() % 2;
        VkFramebuffer fb = fbc_get(rp, v, nv, w, w, l);
        if (!matches(fb, rp, v, nv, w, w, l)) bad++;
    }
    for (int i = 1; i <= nfb; i++) if (F[i].state < 0) reuse_bad++;
    CHECK(!bad, "200,000 random passes: every framebuffer returned is live and was made for exactly that render pass, those views and that size");
    CHECK(!reuse_bad, "no framebuffer destroyed or deferred twice");
    printf("  %d made, %llu found again, %d destroyed with a view, %d evicted\n", nfb, (unsigned long long)g_fbc_hit, ndestroyed, ndeferred);
    printf("%d checks, %d failed\n", checks, fails);
    return fails != 0;
}
