// Engine render scale kept in game memory: some engines draw their 3D scene into part of a full-size target (viewport and scissor scaled by a factor) and stretch it
// to the screen, with a dynamic resolution of their own that lowers the factor when the GPU runs late. The host lowers that factor for a lighter GPU load than the game's
// own settings give (a hot phone, a slow GPU): a factor on the game's own request (dm_res_factor_set: android_main.c graphics_auto, the frame-rate and heat guard), under
// a cap (DM_RES_CAP 0.5 - 1, Graphics on Lowest; dm_res_cap_set). The game's own requests are kept and only lowered, never raised: its scenes' lower factor and the
// background's near-zero one stay as it asks. Only where the game itself draws below full size: a scene that pins full size keeps it (the menus showing Pokémon models,
// zoomed in and misplaced when lowered, #97 #139).
// Measured on a Galaxy S25+ (Adreno 830, Shield, walking in town where the game itself asks 0.8): at 0.6 the GPU clock fell from 336 to 273 MHz on average, but frames
// took longer (43 against 36 ms): the GPU's governor lowers its clock with the load and the replay waits on the GPU. A strong GPU gains nothing from a lower factor; the
// guard checks every step it takes and takes back one that did not help
// Called on every present (hle_nvn.c), on the game's thread: the engines below take the requested factor at the end of the frame, after the present, so the value
// written here is the one the next frame renders at, even when the game's own controller set another one during the frame. Fixed addresses: each row runs only on the
// main build it was found in (AGENTS.md)
#include "hle.h"
#include <mach/mach.h>
#include <stdatomic.h>

static const struct { const char *build, *what; uint64_t ptr_off; uint32_t req, applied, gpu_ns, docked, mode_idx; uint64_t dyn_ptr_off; uint32_t dyn_ctl, dyn_mode, dyn_ovr; } k_scales[] = {
    // Pokémon Sword / Shield 1.3.2 (one engine build; every address below is the same in both mains): the graphics system object, its pointer a global in main's bss
    // (main+0x2636738, read through the GOT slot main+0x2610910 in both builds). [+0x85c0] is the requested 3D scale (float), copied to [+0x85c4] at the end of each frame
    // (main+0x5ea110) and multiplied into the scene passes' viewport and scissor (main+0x5ffa80); the targets stay full size. The game's controller (Shield main+0xf6acb0,
    // Sword main+0xf6ac80) averages the GPU time in [+0x1e0] (ns) over 5 frames and asks 0.8 at 92 % of 33.3 ms, 1.0 again after about 5 s under 70 %; scenes set it
    // fixed (1.0 or 0.8), the background 0.001. [+0x70] is the TV-mode flag and [+0x85bc] the resolution set (0 handheld 1280x720, 1 TV 1920x1080): both are fixed at start
    // from the performance mode (main+0x5def70), so a mode change while playing does not change the render size; this factor does.
    // The controller is an object of its own: [[main+0x26365b8] + 0xd0] (the global read through the GOT slot main+0x2610848 in both builds). Its mode at [+0] is set by
    // Shield main+0xf6ad90 (Sword main+0xf6ad60): 1 its dynamic resolution (the only mode the controller runs in), 2 fixed 0.8, 3 the background's 0.001, any other
    // (0) fixed 1.0. [+0x2c] overrides it while 0 - 3 (Shield main+0xf6ae30: 3 in the background, 4 = no override). Scenes that need full size save the mode, set 0
    // and put it back on leaving (seven such sites, e.g. Shield main+0xc850b0 / +0xc850f0); the menus that show Pokémon models (the summary, the Box), zoomed in and
    // misplaced at a lower scale (#97), behave as such a scene. Call sites listed in galar-re ghidra-out/shield-1.3.2-dynres-calls.txt; the controller's code is the
    // same in both mains, 0x30 lower in Sword
    { "a16802625e7826bf83b6f9708e475b91", "Pokémon Shield 3D render scale", 0x2636738, 0x85c0, 0x85c4, 0x1e0, 0x70, 0x85bc, 0x26365b8, 0xd0, 0x0, 0x2c },
    { "a3b75bcd3311385aeed67fbeeb79cbb7", "Pokémon Sword 3D render scale",  0x2636738, 0x85c0, 0x85c4, 0x1e0, 0x70, 0x85bc, 0x26365b8, 0xd0, 0x0, 0x2c },
};
#define NSCALES ((int)(sizeof k_scales / sizeof k_scales[0]))

static int rd(uint64_t a, void *out, size_t n) { vm_size_t got = 0; return a >= 0x1000 && vm_read_overwrite(mach_task_self(), (vm_address_t)a, n, (vm_address_t)out, &got) == KERN_SUCCESS && got == n; }

static _Atomic int g_row = -2;            // the matching row, -1 none, -2 not looked up yet
static _Atomic uint32_t g_cap_bits;       // the cap as float bits (0 = none set yet: DM_RES_CAP or 1)
static _Atomic uint32_t g_factor_bits = 0x3f800000;   // the guard's factor on the game's own request (float bits; 1.0): a step always lowers what is drawn, also where the game asks 0.8
static _Atomic int g_live;                // 1 once the game's graphics object exists (logged once)
static uint8_t *g_base;

static float cap_now(void) {
    uint32_t b = g_cap_bits; float c;
    if (!b) { const char *e = getenv("DM_RES_CAP"); c = e ? (float)atof(e) : 1.0f; if (!(c >= 0.5f && c <= 1.0f)) { if (e) LOG("[RED] DM_RES_CAP=%s: must be 0.5 - 1; no cap", e); c = 1.0f; }
              memcpy(&b, &c, sizeof b); g_cap_bits = b; }
    memcpy(&c, &b, sizeof c); return c;
}
static int row(void) {
    int r = g_row;
    if (r != -2) return r;
    int found = 0; r = -1;
    for (int i = 0; i < g_nmod; i++) if (!strcmp(g_mod[i].name, "main") && g_mod[i].build_id[0]) {
        found = 1;
        for (int k = 0; k < NSCALES; k++) if (!strcmp(g_mod[i].build_id, k_scales[k].build)) { r = k; g_base = g_mod[i].base; }
    }
    if (found) g_row = r;   // (asked before main is loaded: asked again later)
    return r;
}
// The front end: 1 when this game's render scale can be capped (a row for its main), so its guard steps the scale and not the operation mode. A cap set before the
// game's graphics object exists applies once it does
int dm_res_scale_ready(void) { return row() >= 0; }
void dm_res_cap_set(float c) {
    if (!(c >= 0.5f && c <= 1.0f)) return;
    float old = cap_now(); uint32_t b; memcpy(&b, &c, sizeof b); g_cap_bits = b;
    if (old != c) LOG("render scale cap %.2f → %.2f", old, c);
}
float dm_res_cap_get(void) { return cap_now(); }
// The frame-rate and heat guard (android_main.c graphics_auto): draw at this factor of what the game asks (0.5 - 1), still under the cap
void dm_res_factor_set(float f) {
    if (!(f >= 0.5f && f <= 1.0f)) return;
    uint32_t b; memcpy(&b, &f, sizeof b); uint32_t old = atomic_exchange(&g_factor_bits, b); float o; memcpy(&o, &old, sizeof o);
    if (o != f) LOG("render scale factor %.2f → %.2f", o, f);
}
static float factor_now(void) { uint32_t b = g_factor_bits; float f; memcpy(&f, &b, sizeof f); return f; }

// The game's dynamic-resolution mode, by the controller's own rule (Shield main+0xf6acb0): the override while it is 3 or less, else the scene's mode; -1 while the
// controller cannot be read (not created yet)
static int dyn_mode(int k) {
    uint64_t sys, ctl; int32_t m, o;
    if (!rd((uint64_t)(uintptr_t)(g_base + k_scales[k].dyn_ptr_off), &sys, sizeof sys) || !sys || !rd(sys + k_scales[k].dyn_ctl, &ctl, sizeof ctl) || !ctl) return -1;
    if (!rd(ctl + k_scales[k].dyn_mode, &m, sizeof m) || !rd(ctl + k_scales[k].dyn_ovr, &o, sizeof o)) return -1;
    return o > 3 ? m : o;
}
static const char *dyn_name(int m) { return m == 1 ? "dynamic" : m == 2 ? "fixed 0.8" : m == 3 ? "background" : m < 0 ? "not readable" : "fixed full size"; }
// 1 while the factor and the cap reach what is drawn (the game's mode lets its scale go below full): the guard (android_main.c) judges a step only then
static _Atomic int g_applies = 1;
int dm_res_scale_applies(void) { return g_applies; }

void dm_res_scale_tick(void) {
    static float game = -1, wrote = -1; static uint64_t n; static int nlog, was_mode = -2, nmode;
    int k = row(); if (k < 0) return;
    uint64_t obj;
    if (!rd((uint64_t)(uintptr_t)(g_base + k_scales[k].ptr_off), &obj, sizeof obj) || !obj) return;   // not created yet
    float cur, cap = cap_now();
    if (!rd(obj + k_scales[k].req, &cur, sizeof cur)) return;
    if (!g_live) { g_live = 1; LOG("main: %s found (requested %.3f); cap %.2f%s", k_scales[k].what, cur, cap, getenv("DM_RES_CAP") ? " (DM_RES_CAP)" : ""); }
    if (cur != wrote) game = cur;   // the game wrote it since (its controller, a scene, the background)
    if (!(game > 0 && game <= 1.0f)) { wrote = cur; return; }   // (an unexpected value: left alone; a later request is still seen as the game's)
    // lowered only in the modes where the game itself draws below full size, its dynamic resolution (1) and its fixed 0.8 (2): every pass then follows the scale.
    // A scene in mode 0 pins full size and gets the game's own request back (the game writes 1.0 itself when the mode changes; this covers a value of ours left over)
    int mode = dyn_mode(k), lower = mode == 1 || mode == 2;
    g_applies = lower;
    if (mode != was_mode) {
        if (nmode < 200 && (was_mode != -2 || mode != -1)) { nmode++; LOG("render scale: the game's mode %s → %s%s", dyn_name(was_mode), dyn_name(mode),
                                                                         lower ? "" : ": drawn at the game's own scale, the factor and the cap wait"); }
        was_mode = mode;
    }
    float want = game;
    if (lower) { if (game >= 0.05f) want = game * factor_now(); if (want > cap) want = cap; }   // (the background's near-zero request stays as it is)
    if (cur != want) { uint32_t *p = (uint32_t *)(uintptr_t)(obj + k_scales[k].req), w; memcpy(&w, &want, sizeof w); dm_guest_write_prepare(p, sizeof *p); __atomic_store_n(p, w, __ATOMIC_RELAXED); }
    wrote = want;
    // A line a minute (at 30 fps) while the cap or the game's own scale is below full: what the game measured, what it asked for and what it draws at, for tuning the guard
    if (++n % 1800 == 0 && (cap < 1.0f || game < 1.0f || factor_now() < 1.0f) && nlog < 240) {
        int64_t gpu = 0; float app = 0; int32_t tv = -1, set = -1;
        rd(obj + k_scales[k].gpu_ns, &gpu, sizeof gpu); rd(obj + k_scales[k].applied, &app, sizeof app); rd(obj + k_scales[k].docked, &tv, sizeof tv); rd(obj + k_scales[k].mode_idx, &set, sizeof set);
        nlog++; LOG("render scale: game asks %.3f (mode %s), factor %.2f, cap %.2f, drawing at %.3f (%s set, TV flag %d); the game's GPU time %.1f ms", game, dyn_name(mode),
                    factor_now(), cap, app, set == 1 ? "TV" : set == 0 ? "handheld" : "?", tv, gpu / 1e6);
    }
}
