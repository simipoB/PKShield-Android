// Touch on menus that have no touch support (Pokémon Legends: Arceus): a quick tap on a menu item moves the game's own cursor there with D-pad presses and
// presses A, by reading the game's UI out of its memory. The game's menus are nn::ui2d-style layouts: pane objects with a name, a parent, a local position,
// scale and size and a visible flag. The panes are found by their class (vtable addresses from the game profile, key touch_menu_panes, valid for one build
// of main: touch_menu_build) in the process's writable memory, then kept: a rescan happens only when the kept ones no longer show a cursor.
// Pane fields (ui2d, the same in every pane class seen): +0x18 parent, +0x30 / +0x34 position, +0x48 / +0x4c scale, +0x50 / +0x54 size,
// +0x58 flags (bit 0 visible), +0x59 alpha, +0x5b base position (bits 0-1 horizontal: centre / left / right, bits 2-3 vertical: centre / top / bottom), +0xb0 name.
// Layout space: 1920 x 1080, origin at the centre, y up.
#include <math.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/uio.h>
#include <time.h>
#include <unistd.h>
#include "dm.h"
#include "dm_input.h"
#include "game_profile.h"

#if defined(DM_GAME_TOUCH_MENU) && DM_GAME_TOUCH_MENU
static const uint64_t PANE_VT[] = DM_GAME_TOUCH_MENU_PANES;
enum { NVT = sizeof PANE_VT / sizeof *PANE_VT, MAXP = 65536 };

static ssize_t rd(uintptr_t a, void *b, size_t n) { struct iovec l = { b, n }, r = { (void *)a, n }; return process_vm_readv(getpid(), &l, 1, &r, 1, 0); }
static double now_s(void) { struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t); return t.tv_sec + t.tv_nsec / 1e9; }

typedef struct { uintptr_t a; float x0, y0, x1, y1; int vis; char name[25]; } pane_t;   // a rectangle in layout space
static uintptr_t g_vt[NVT]; static int g_ok = -1;
static uintptr_t *g_found; static int g_nfound;                      // pane objects kept from the last scan
static struct { unsigned long s, e; } g_reg[4096]; static int g_nreg;     // the mappings that held panes (later scans read only these)

static int setup(void) {
    if (g_ok >= 0) return g_ok;
    g_ok = 0;
    if (strcmp(dm_main_build(), DM_GAME_TOUCH_MENU_BUILD)) { fprintf(stderr, "[touch-menu] off: main is build %s, the pane classes were found in %s\n", dm_main_build(), DM_GAME_TOUCH_MENU_BUILD); return 0; }
    for (int i = 0; i < g_nmod; i++) if (!strcmp(g_mod[i].name, "main")) for (int k = 0; k < NVT; k++) g_vt[k] = (uintptr_t)g_mod[i].base + PANE_VT[k];
    g_found = malloc(sizeof *g_found * MAXP); g_ok = g_found != NULL;
    return g_ok;
}
// every pane object: 8-byte words equal to a pane class's vtable, in all private writable mappings (first time) or the ones that held panes before
static void scan(int all) {
    static uint64_t buf[1 << 16]; double t0 = now_s(); g_nfound = 0;
    static struct { unsigned long s, e; } *reg; static int creg; int nreg = 0;   // (the game has thousands of mappings)
    if (all || !g_nreg) {
        FILE *m = fopen("/proc/self/maps", "r"); char ln[256];
        while (m && fgets(ln, sizeof ln, m)) { unsigned long s, e; char pr[8] = "";
            if (sscanf(ln, "%lx-%lx %7s", &s, &e, pr) == 3 && pr[0] == 'r' && pr[1] == 'w' && pr[3] == 'p') {
                if (nreg == creg) { creg = creg ? creg * 2 : 4096; reg = realloc(reg, sizeof *reg * creg); }
                reg[nreg].s = s; reg[nreg].e = e; nreg++; } }
        if (m) fclose(m);
    } else { if (creg < 4096) { creg = 4096; reg = realloc(reg, sizeof *reg * creg); } for (int i = 0; i < g_nreg; i++) { reg[i].s = g_reg[i].s; reg[i].e = g_reg[i].e; } nreg = g_nreg; }
    int nkeep = 0;
    for (int r = 0; r < nreg; r++) { int hit = 0;
        for (unsigned long a = reg[r].s; a < reg[r].e && g_nfound < MAXP; a += sizeof buf) {
            size_t n = reg[r].e - a < sizeof buf ? reg[r].e - a : sizeof buf; if (rd(a, buf, n) != (ssize_t)n) continue;
            for (size_t i = 0; i < n / 8; i++) for (int k = 0; k < NVT; k++) if (buf[i] == g_vt[k]) { if (g_nfound < MAXP) g_found[g_nfound++] = a + i * 8; hit = 1; break; } }
        if (hit && nkeep < 4096) { g_reg[nkeep].s = reg[r].s; g_reg[nkeep].e = reg[r].e; nkeep++; } }
    g_nreg = nkeep;
    fprintf(stderr, "[touch-menu] scan (%s): %d panes in %d of %d mappings, %.0f ms (first pane class at %lx)\n", all ? "all memory" : "kept mappings", g_nfound, g_nreg, nreg,
            (now_s() - t0) * 1e3, (unsigned long)g_vt[0]);
}
// a pane's rectangle in layout space and whether it shows (itself and every parent visible, alpha above 0), through its parents
static int pane_get(uintptr_t a, pane_t *p) {
    unsigned char b[0xc8]; if (rd(a, b, sizeof b) != (ssize_t)sizeof b) return 0;
    float tx, ty, sx, sy, w, h; memcpy(&tx, b + 0x30, 4); memcpy(&ty, b + 0x34, 4); memcpy(&sx, b + 0x48, 4); memcpy(&sy, b + 0x4c, 4); memcpy(&w, b + 0x50, 4); memcpy(&h, b + 0x54, 4);
    if (!(w == w) || !(h == h) || fabsf(w) > 1e5f || fabsf(h) > 1e5f) return 0;
    p->a = a; memcpy(p->name, b + 0xb0, 24); p->name[24] = 0;
    int vis = (b[0x58] & 1) && b[0x59]; uint8_t base = b[0x5b];
    // up the parents: position = parent position + parent scale * own position, scale multiplied (rotation ignored)
    float gx = 0, gy = 0, gsx = 1, gsy = 1; float lx = tx, ly = ty, lsx = sx, lsy = sy; uintptr_t par; memcpy(&par, b + 0x18, 8);
    float chain[64][4]; int n = 0; chain[n][0] = lx; chain[n][1] = ly; chain[n][2] = lsx; chain[n][3] = lsy; n++;
    for (int d = 0; par && d < 63; d++) { unsigned char q[0x60]; if (rd(par, q, sizeof q) != (ssize_t)sizeof q) break;
        float qx, qy, qsx, qsy; memcpy(&qx, q + 0x30, 4); memcpy(&qy, q + 0x34, 4); memcpy(&qsx, q + 0x48, 4); memcpy(&qsy, q + 0x4c, 4);
        if (!((q[0x58] & 1) && q[0x59])) vis = 0;
        chain[n][0] = qx; chain[n][1] = qy; chain[n][2] = qsx; chain[n][3] = qsy; n++; memcpy(&par, q + 0x18, 8); }
    for (int i = n - 1; i >= 0; i--) { gx += gsx * chain[i][0]; gy += gsy * chain[i][1]; gsx *= chain[i][2]; gsy *= chain[i][3]; }
    float W = fabsf(w * gsx), H = fabsf(h * gsy), x0 = gx - W / 2, y1 = gy + H / 2;
    if ((base & 3) == 1) x0 = gx; else if ((base & 3) == 2) x0 = gx - W;
    if ((base >> 2 & 3) == 1) y1 = gy; else if ((base >> 2 & 3) == 2) y1 = gy + H;
    p->x0 = x0; p->x1 = x0 + W; p->y1 = y1; p->y0 = y1 - H; p->vis = vis;
    return 1;
}
static int is_cursor(const char *n) { return strstr(n, "cursor") != NULL; }
// the game's cursor: the smallest showing pane named *cursor* (its centre)
static int cursor_at(float *cx, float *cy) {
    pane_t p; float best = 1e30f; int ok = 0;
    for (int i = 0; i < g_nfound; i++) { if (!pane_get(g_found[i], &p) || !p.vis || !is_cursor(p.name)) continue;
        float ar = (p.x1 - p.x0) * (p.y1 - p.y0); if (ar < 16 || ar > 1920 * 1080 * 0.5f || ar >= best) continue;
        float mx = (p.x0 + p.x1) / 2, my = (p.y0 + p.y1) / 2; if (mx < -960 || mx > 960 || my < -540 || my > 540) continue;   // on screen only (hidden menus keep theirs off it)
        best = ar; *cx = mx; *cy = my; ok = 1; }
    return ok;
}
// what was tapped: the smallest showing part (a pane named L_*: list items, buttons) under the point, not a cursor and not most of the screen
static int target_at(float x, float y, pane_t *out) {
    pane_t p; float best = 1e30f; int ok = 0;
    for (int i = 0; i < g_nfound; i++) { if (!pane_get(g_found[i], &p) || !p.vis || is_cursor(p.name) || p.name[0] != 'L' || p.name[1] != '_') continue;
        if (x < p.x0 || x > p.x1 || y < p.y0 || y > p.y1) continue;
        float ar = (p.x1 - p.x0) * (p.y1 - p.y0); if (ar < 64 || ar > 1920 * 1080 * 0.4f || ar >= best) continue;
        best = ar; *out = p; ok = 1; }
    return ok;
}
// for tuning: the showing panes under a point and the showing cursor-like panes, to the log
static void diag(float x, float y) {
    pane_t p; int n = 0, nc = 0, nv = 0;
    for (int i = 0; i < g_nfound; i++) { if (!pane_get(g_found[i], &p) || !p.vis) continue; nv++;
        if (x >= p.x0 && x <= p.x1 && y >= p.y0 && y <= p.y1 && n < 24) { fprintf(stderr, "[touch-menu]   under: %-24s %lx [%.0f %.0f %.0f %.0f]\n", p.name, (unsigned long)p.a, p.x0, p.y0, p.x1, p.y1); n++; }
        if ((strstr(p.name, "cursor") || strstr(p.name, "select") || strstr(p.name, "focus")) && nc < 16) { fprintf(stderr, "[touch-menu]   cursor-like: %-24s %lx [%.0f %.0f %.0f %.0f]\n", p.name, (unsigned long)p.a, p.x0, p.y0, p.x1, p.y1); nc++; } }
    fprintf(stderr, "[touch-menu]   %d of %d panes showing\n", nv, g_nfound);
}
static void press(uint64_t b) { dm_input_press(b, 70000000ull); usleep(90000); }
// steers the cursor into the target with D-pad presses (re-reading where it went after each one), then presses A
static void drive(float tx, float ty) {
    pane_t t; float cx, cy;
    if (!g_nfound || !cursor_at(&cx, &cy)) scan(0);
    if (!cursor_at(&cx, &cy)) scan(1);
    if (!cursor_at(&cx, &cy)) { fprintf(stderr, "[touch-menu] tap (%.0f, %.0f): no cursor showing (not a menu)\n", tx, ty); diag(tx, ty); return; }
    if (!target_at(tx, ty, &t)) { scan(1); cursor_at(&cx, &cy); }   // a menu opened since the last scan lives in other mappings
    if (!target_at(tx, ty, &t)) { fprintf(stderr, "[touch-menu] tap (%.0f, %.0f): no menu item there\n", tx, ty); diag(tx, ty); return; }
    fprintf(stderr, "[touch-menu] tap (%.0f, %.0f) on %s [%.0f %.0f %.0f %.0f], cursor at (%.0f, %.0f)\n", tx, ty, t.name, t.x0, t.y0, t.x1, t.y1, cx, cy);
    float mx = (t.x0 + t.x1) / 2, my = (t.y0 + t.y1) / 2; int stuck = 0, tried_other = 0;
    for (int step = 0; step < 40; step++) {
        if (cx >= t.x0 && cx <= t.x1 && cy >= t.y0 && cy <= t.y1) { press(DM_BTN_A); fprintf(stderr, "[touch-menu] on %s after %d moves: A\n", t.name, step); return; }
        float dx = mx - cx, dy = my - cy; int horiz = fabsf(dx) * 1.5f > fabsf(dy); if (tried_other) horiz = !horiz;
        uint64_t b = horiz ? (dx > 0 ? DM_BTN_RIGHT : DM_BTN_LEFT) : (dy > 0 ? DM_BTN_UP : DM_BTN_DOWN);
        press(b);
        float nx = cx, ny = cy;   // where the cursor settles (it may glide): read until two reads agree, at most 0.4 s
        for (int k = 0; k < 8; k++) { usleep(50000); float ax, ay; if (!cursor_at(&ax, &ay)) break; int same = fabsf(ax - nx) < 0.5f && fabsf(ay - ny) < 0.5f; nx = ax; ny = ay; if (same && k) break; }
        if (fabsf(nx - cx) < 1 && fabsf(ny - cy) < 1) { if (++stuck >= 2) { if (tried_other) break; tried_other = 1; stuck = 0; } }
        else { stuck = 0; tried_other = 0; }
        cx = nx; cy = ny;
    }
    fprintf(stderr, "[touch-menu] could not reach %s (cursor ended at (%.0f, %.0f))\n", t.name, cx, cy); diag(tx, ty);
}
static pthread_mutex_t g_mu = PTHREAD_MUTEX_INITIALIZER; static pthread_cond_t g_cv = PTHREAD_COND_INITIALIZER;
static pthread_mutex_t g_scan_mu = PTHREAD_MUTEX_INITIALIZER;   // the pane list (scan, drive, the watcher)
static float g_tx, g_ty; static int g_pending, g_started;
// Menu open = one of the game's cursor panes showing. The watcher checks the cursor panes (picked out of the pane list) twice a second and refreshes the
// list every 12 s while no menu shows; the touch controls stay faint while a menu shows (touch_ui.c), its items being tapped directly
static _Atomic int g_menu; static uintptr_t g_cur[2048]; static int g_ncur; static double g_scan_t;
static void cursors_pick(void) {
    g_ncur = 0; char nm[25];
    for (int i = 0; i < g_nfound && g_ncur < 2048; i++) if (rd(g_found[i] + 0xb0, nm, 24) == 24) { nm[24] = 0; if (is_cursor(nm)) g_cur[g_ncur++] = g_found[i]; }
}
static void *watch_main(void *a) {
    (void)a; if (!setup()) return NULL;
    for (;;) { usleep(500000);
        pthread_mutex_lock(&g_scan_mu);
        if (!g_nfound || (!atomic_load(&g_menu) && now_s() - g_scan_t > 12)) { scan(g_nreg == 0); cursors_pick(); g_scan_t = now_s(); }
        int on = 0; pane_t p;
        for (int i = 0; i < g_ncur && !on; i++) if (pane_get(g_cur[i], &p) && p.vis) { float ar = (p.x1 - p.x0) * (p.y1 - p.y0); if (ar >= 16 && ar < 1920 * 1080 * 0.5f) on = 1; }
        pthread_mutex_unlock(&g_scan_mu);
        if (on != atomic_load(&g_menu)) { atomic_store(&g_menu, on); fprintf(stderr, "[touch-menu] menu %s\n", on ?"open: touch controls kept faint" : "closed"); } }
    return NULL;
}
int touch_menu_active(void) {
    static int started; if (!started) { started = 1; pthread_t t; if (!pthread_create(&t, NULL, watch_main, NULL)) pthread_detach(t); }
    return atomic_load(&g_menu);
}
static void *driver_main(void *a) {
    (void)a;
    for (;;) { pthread_mutex_lock(&g_mu); while (!g_pending) pthread_cond_wait(&g_cv, &g_mu); float x = g_tx, y = g_ty; g_pending = 0; pthread_mutex_unlock(&g_mu);
        if (setup()) { pthread_mutex_lock(&g_scan_mu); drive(x, y); if (g_nfound) { cursors_pick(); g_scan_t = now_s(); } pthread_mutex_unlock(&g_scan_mu); } }
    return NULL;
}
// a quick tap at screen point (x, y) of a w x h window showing the 16:9 game picture fitted (as the presentation blits it): to the driver thread
void touch_menu_tap(float x, float y, float w, float h) {
    float s = fminf(w / 1920.0f, h / 1080.0f), ox = (w - 1920 * s) / 2, oy = (h - 1080 * s) / 2;
    float lx = (x - ox) / s - 960, ly = 540 - (y - oy) / s;
    if (lx < -960 || lx > 960 || ly < -540 || ly > 540) return;
    pthread_mutex_lock(&g_mu);
    if (!g_started) { pthread_t t; if (!pthread_create(&t, NULL, driver_main, NULL)) { pthread_detach(t); g_started = 1; } }
    g_tx = lx; g_ty = ly; g_pending = 1; pthread_cond_signal(&g_cv); pthread_mutex_unlock(&g_mu);
}
#else
void touch_menu_tap(float x, float y, float w, float h) { (void)x; (void)y; (void)w; (void)h; }
int touch_menu_active(void) { return 0; }
#endif
