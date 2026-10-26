// NVN null GPU for the Android command-line host (Phase 1): stands in for the Metal replay (nvn_mtl.m) until the Vulkan replay exists.
// It draws nothing but keeps every effect the CPU can observe, in submission order and complete at once: event writes, fence syncs, counter reports (timestamps on the
// guest's 19.2 MHz clock, zero samples passed), buffer copies, fills and inline uniform updates (GPU addresses are CPU addresses). Presents are paced to the period the game asks for.
#include "dm.h"
#include "nvn_mtl.h"
#include "nvn_obj.h"
#include <stdatomic.h>
#include <time.h>

static _Atomic uint64_t g_fence, g_present_n;
static uint64_t ticks(void) { uint64_t g = guest_ns(); if (!g) g = now_ns(); return g * 12 / 625; }

void mtl_init(void) {}
void mtl_set_depth_zero(int on) { (void)on; }
void mtl_set_origin_lower_left(int on) { (void)on; }
void mtl_pool_add(const void *mem, uint64_t size) { (void)mem; (void)size; }
void mtl_pool_del(const void *mem) { (void)mem; }
void mtl_alias_drop_pool(const void *pool) { (void)pool; }
int mtl_addr(uint64_t addr, uint64_t len, void **buf, uint64_t *off) { (void)addr; (void)len; *buf = NULL; *off = 0; return 0; }
void mtl_set_tex_pool_n(int n) { (void)n; }
void mtl_reg_tex(int id, const tex_t *t, const view_t *v) { (void)id; (void)t; (void)v; }
void mtl_reg_samp(int id, const samp_par *p) { (void)id; (void)p; }
void mtl_tex_init(const tex_t *t) { (void)t; }
void mtl_tex_release(tex_t *t) { (void)t; }
void mtl_tex_cpu_written(const tex_t *t) { (void)t; }
void mtl_read_texels(const tex_t *t, unsigned level, unsigned layer0, const NVNcopyRegion *r, uint8_t *dst, size_t row, size_t img) { (void)t; (void)level; (void)layer0; (void)r; (void)dst; (void)row; (void)img; }
void mtl_prog_release(prog_t *p) { (void)p; }
void mtl_xv_window_tex(uint64_t serial) { (void)serial; }

// Submission: the replay walks the recorded command list; only CPU-visible operations act
void mtl_sub_begin(void) {}
uint64_t mtl_sub_commit(void) { return atomic_fetch_add(&g_fence, 1) + 1; }
void mtl_sub_index(const uint64_t *h, int n) { (void)h; (void)n; }
uint64_t mtl_last_fence(void) { return atomic_load(&g_fence); }
int mtl_wait(uint64_t fence, uint64_t timeout_ns) { (void)fence; (void)timeout_ns; return 1; }
void mtl_finish(void) {}
void mtl_drain(void) {}
void mtl_exec_rest(const uint64_t *handles, int n) { (void)handles; (void)n; }
void mtl_exec(const uint8_t *p) {
    for (int guard = 0; guard < 1 << 24; guard++) {
        op_hdr h; memcpy(&h, p, sizeof h); const uint8_t *a = p + 8;
        switch (h.op) {
            case OP_END: return;
            case OP_JUMP: memcpy(&p, a, 8); continue;
            case OP_CALL: { const uint8_t *t; memcpy(&t, a, 8); mtl_exec(t); break; }
            case OP_SIGNAL_EVENT: { volatile uint32_t *val; uint32_t m, v; memcpy(&val, a, 8); memcpy(&m, a + 8, 4); memcpy(&v, a + 12, 4); if (m == 1) *val += v; else *val = v; break; }
            case OP_REPORT_COUNTER: { uint64_t t, ad; memcpy(&t, a, 8); memcpy(&ad, a + 8, 8); uint64_t now = ticks();
                uint64_t rec[2] = { t == NVN_COUNTER_TYPE_SAMPLES_PASSED ? 0 : (t == 0 || t == 0x10) && !DM_GAME_RENDER_COND ? now : 0, now }; memcpy((void *)ad, rec, sizeof rec); break; }
            case OP_FENCE_SYNC: { sync_t *s; memcpy(&s, a, 8); s->fence = atomic_load(&g_fence) + 1; s->fenced = 1; break; }
            case OP_CLEAR_BUF: { const p_clear_buf *cb = (const p_clear_buf *)a; uint32_t *d = (uint32_t *)cb->addr; for (uint64_t i = 0; i < cb->size / 4; i++) d[i] = cb->value; break; }
            case OP_COPY_B2B: { uint64_t s, d, n; memcpy(&s, a, 8); memcpy(&d, a + 8, 8); memcpy(&n, a + 16, 8); memmove((void *)d, (const void *)s, n); break; }
            case OP_UPDATE_UB: { uint64_t d, n; memcpy(&d, a, 8); memcpy(&n, a + 8, 8); memcpy((void *)d, a + 16, n); break; }
            default: break;
        }
        p += h.size;
    }
    LOG("[RED] null gpu: command list did not end");
}

// Presents: nothing is shown; the call returns once the image's period has passed since the previous one, as a display would pace it
static uint64_t g_last_present;
uint64_t mtl_present(const tex_t *t, double period_s, const int crop[4]) {
    (void)t; (void)crop; uint64_t now = now_ns(), due = g_last_present + (uint64_t)(period_s * 1e9);
    if (period_s > 0 && due > now) { struct timespec ts = { (time_t)((due - now) / 1000000000ull), (long)((due - now) % 1000000000ull) }; nanosleep(&ts, NULL); now = due; }
    g_last_present = now;
    uint64_t n = atomic_fetch_add(&g_present_n, 1) + 1;
    if (n == 1 || n % 300 == 0) LOG("null gpu: present %llu", (unsigned long long)n);
    return n;
}
void mtl_present_wait(uint64_t ticket) { (void)ticket; }
double mtl_presented_time(uint64_t n) { (void)n; return 0; }
uint32_t mtl_present_qos(void) { return 0; }
int mtl_has_window(void) { return 0; }
int mtl_display_hz(void) { return 60; }
int mtl_fps_target(void) { const char *e = getenv("DM_FPS_TARGET"); int f = e ? atoi(e) : 0; return f == 60 ? 60 : 30; }
int mtl_link_active(void) { return 0; }
int mtl_link_wait_tick(int vsyncs, uint64_t timeout_ns) { (void)vsyncs; (void)timeout_ns; return 0; }
void mtl_set_frame(uint64_t f) { (void)f; }
double mtl_gpu_ms(uint64_t f) { (void)f; return 0; }
int mtl_gpu_span(uint64_t f, uint64_t *t0, uint64_t *t1) { (void)f; *t0 = *t1 = 0; return 0; }
void mtl_ub_present(double iv_ms) { (void)iv_ms; }

// Statistics read by the host's report lines: all zero
void mtl_alloc_stats(uint64_t *a, uint64_t *b, int *n) { *a = *b = 0; *n = 0; }
void mtl_args_stats(uint64_t *enc, uint64_t *reuse) { *enc = *reuse = 0; }
void mtl_bw(uint64_t *load, uint64_t *store, uint64_t *full) { *load = *store = *full = 0; }
void mtl_create_ns(uint64_t *lib, uint64_t *pso) { *lib = *pso = 0; }
void mtl_create_stats(uint64_t *lib, uint64_t *pso) { *lib = *pso = 0; }
uint64_t mtl_drawable_ns_take(void) { return 0; }
uint64_t mtl_ds_omit(void) { return 0; }
uint64_t mtl_pkg_miss(void) { return 0; }
void mtl_pkg_stats(uint64_t *u, uint64_t *r) { *u = *r = 0; }
void mtl_stats(uint64_t *passes, uint64_t *draws) { *passes = *draws = 0; }
uint64_t mtl_udc_saved_bytes(void) { return 0; }
uint64_t mtl_upload_bytes(void) { return 0; }
uint64_t mtl_upload_gpu_ns_take(void) { return 0; }
uint64_t mtl_upload_ns_take(void) { return 0; }
void mtl_use_stats(uint64_t *call, uint64_t *skip) { *call = *skip = 0; }
