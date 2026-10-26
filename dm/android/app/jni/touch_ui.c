// The on-screen controls: what they look like (front_overlay, drawn by the Vulkan replay over the game, nvn_ovl.h), what touching them does (touch_ui_event), and the
// haptic click of each press (Android's Vibrator through JNI, on a thread of its own so input never waits on it). Proportions and look after LAS Recomp's Android port
// (runtime/ui/touch_controls.mm), in units of the screen height, landscape: glossy bubbles (shadow, dark rim, body, glow, a gloss spot, a bright rim), all the same
// size as on the Switch; a floating stick (a compass ring that rests bottom-left and comes to where the thumb lands in the lower-left part); A B X Y in the Switch's diamond bottom-right;
// L / R round in the top corners; − and + at the top centre; a right stick below-left of the diamond, as on a right Joy-Con (the camera, pushed like the real stick).
// Dragging anywhere else on the right turns the camera too, following the finger:
// the stick is the finger's speed, back to rest as soon as the finger stops. A quick tap is held at least 80 ms (the game reads its input once a frame, every 33 ms);
// touch areas are 1.3 times the drawing; a thumb sliding across the face buttons presses the one under it. Hidden while a controller is used, back on the next touch.
// The layout and drawing parts also build into the Mac Vulkan host (mac_vk_build.sh), whose DM_OVL_SHOT composes them over a game frame to check the look.
#include "dm.h"
#include "game_profile.h"   // DM_GAME_TOUCH_ZLZR (the profile key touch_zlzr)
#if defined(DM_GAME_TOUCH_MENU) && DM_GAME_TOUCH_MENU
#define GESTURES_ON 1   // the throw button and the touch gestures (touch_ui_event): the games with touch menus (Legends: Arceus)
#else
#define GESTURES_ON 0
#endif
#include "dm_input.h"
#include "nvn_ovl.h"
#ifdef __ANDROID__
#include <android/input.h>
#include <android/log.h>
#include <android/native_activity.h>
#include <jni.h>
#endif
#include <math.h>
#include <pthread.h>
#include <string.h>
#include <time.h>

enum { C_STICK, C_CAM, C_RSTICK, C_A, C_B, C_X, C_Y, C_L, C_R, C_ZL, C_ZR, C_PLUS, C_MINUS, C_UP, C_DOWN, C_LEFT, C_RIGHT, C_THROW, NCTL };
typedef struct { float cx, cy, r; int glyph, shown; uint64_t btn; } ctl;
static ctl g_c[NCTL];
static float g_w, g_h;
static struct { int32_t id; int target; float x0, y0, x, y, lx, ly; double lt, t0; int thr, lp; } g_ptr[10];   // (thr: this finger's swipe became a throw)   // (t0: when the finger landed)   // fingers down: id, what they hold, where, last move (camera)
static uint64_t g_btn; static int32_t g_lx, g_ly, g_rx, g_ry;
static double g_held_until[NCTL];                   // the 80 ms minimum press
static float g_cam_vx, g_cam_vy; static double g_cam_t;   // the camera finger's speed (pixels / s) and when it last moved
static float g_stick_x, g_stick_y, g_knob_x, g_knob_y; static int g_stick_on;
static float g_rknob_x, g_rknob_y; static int g_rstick_on;
static double g_inner_t;   // the last touch of the D-pad or the right stick   // the right stick: its base stays put, the knob follows the thumb
static int g_visible = 1;
static int g_edit;   // the layout editor is on (touch_ui_event, front_overlay)
static double g_touch_t; static double g_two_t, g_tap_mute;   // two-finger tap: when the second finger landed; single taps ignored until g_tap_mute
int touch_menu_active(void);   // touch_menu.c: one of the game's menus shows
#if defined(DM_GAME_TOUCH_MENU) && DM_GAME_TOUCH_MENU
enum { GESTURES = 1 };   // tap = A in the world, swipe up = throw: only where menu taps work too (else a tap in a menu would press A on whatever is selected)
#else
enum { GESTURES = 0 };
#endif   // the last touch anywhere: every control fades 3 s after it (they cover the game's own menus and text)
static pthread_mutex_t g_mu = PTHREAD_MUTEX_INITIALIZER;
static double now_s(void) { struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t); return t.tv_sec + t.tv_nsec / 1e9; }
static float clampf(float v, float lo, float hi) { return v < lo ? lo : v > hi ? hi : v; }

// ---------- the player's layout (the in-game editor): position, size and visibility of each control, kept in <files>/touch_layout.txt ----------
// One line per changed control: "<name> <x / width> <y / height> <radius / height> <shown>"; controls without a line keep the default layout
static const char *const CTL_NAME[NCTL] = { "stick", "camera", "rstick", "a", "b", "x", "y", "l", "r", "zl", "zr", "plus", "minus", "up", "down", "left", "right", "throw" };
static struct { int set; float fx, fy, fr; int shown; } g_ov[NCTL];
static char g_lay_path[1100]; static int g_lay_loaded;
void touch_ui_set_dir(const char *dir) { snprintf(g_lay_path, sizeof g_lay_path, "%s/touch_layout.txt", dir); g_lay_loaded = 0; }
static void layout_load(void) {
    g_lay_loaded = 1; memset(g_ov, 0, sizeof g_ov); if (!g_lay_path[0]) return;
    FILE *f = fopen(g_lay_path, "r"); char ln[160], nm[32]; float fx, fy, fr; int sh;
    while (f && fgets(ln, sizeof ln, f)) if (sscanf(ln, "%31s %f %f %f %d", nm, &fx, &fy, &fr, &sh) == 5)
        for (int c = 0; c < NCTL; c++) if (!strcmp(nm, CTL_NAME[c])) { g_ov[c].set = 1; g_ov[c].fx = fx; g_ov[c].fy = fy; g_ov[c].fr = fr; g_ov[c].shown = sh; }
    if (f) fclose(f);
}
static void layout_save(void) {
    if (!g_lay_path[0]) return; FILE *f = fopen(g_lay_path, "w"); if (!f) return;
    for (int c = 0; c < NCTL; c++) if (g_ov[c].set) fprintf(f, "%s %.4f %.4f %.4f %d\n", CTL_NAME[c], g_ov[c].fx, g_ov[c].fy, g_ov[c].fr, g_ov[c].shown);
    fclose(f);
}
static void layout_apply(float w, float h) {
    if (!g_lay_loaded) layout_load();
    for (int c = 0; c < NCTL; c++) if (g_ov[c].set && c != C_CAM) { g_c[c].cx = g_ov[c].fx * w; g_c[c].cy = g_ov[c].fy * h; g_c[c].r = g_ov[c].fr * h; g_c[c].shown = g_ov[c].shown; }
}
static void layout(float w, float h) {
    static int init; if (!init++) for (int i = 0; i < 10; i++) g_ptr[i].id = -1;   // no finger down
    if (w == g_w && h == g_h) return;
    g_w = w; g_h = h;
    // the outer controls keep about 0.2 of the height (~290 px on a 1440 px phone) clear of the screen's long edges, where the system's navigation buttons and edge
    // swipes are: with the A button nearer, a thumb on it opened recents
    float s = h, br = 0.074f * s, gap = 0.135f * s, fx = w - 0.40f * s, fy = h - 0.30f * s, sr = 0.064f * s, mr = 0.040f * s;
    g_c[C_A] = (ctl){ fx + gap, fy, br, OVL_G_A, 1, DM_BTN_A };   // the Switch's diamond, all four the same
    g_c[C_B] = (ctl){ fx, fy + gap, br, OVL_G_B, 1, DM_BTN_B };
    g_c[C_X] = (ctl){ fx, fy - gap, br, OVL_G_X, 1, DM_BTN_X };
    g_c[C_Y] = (ctl){ fx - gap, fy, br, OVL_G_Y, 1, DM_BTN_Y };
    g_c[C_L] = (ctl){ 0.25f * s, 0.30f * s, sr, OVL_G_L, 1, DM_BTN_L };
    g_c[C_R] = (ctl){ w - 0.25f * s, 0.30f * s, sr, OVL_G_R, 1, DM_BTN_R };
    // ZL / ZR above L / R where the game uses them (profile key touch_zlzr: Legends: Arceus aims with ZL and throws with ZR); hidden otherwise (Shield)
#if defined(DM_GAME_TOUCH_ZLZR) && DM_GAME_TOUCH_ZLZR
    g_c[C_ZL] = (ctl){ 0.25f * s, 0.11f * s, sr, OVL_G_ZL, 1, DM_BTN_ZL };
    g_c[C_ZR] = (ctl){ w - 0.25f * s, 0.11f * s, sr, OVL_G_ZR, 1, DM_BTN_ZR };
#else
    g_c[C_ZL] = (ctl){ 0.26f * s, 0.10f * s, sr * 0.8f, OVL_G_ZL, 0, DM_BTN_ZL };
    g_c[C_ZR] = (ctl){ w - 0.26f * s, 0.10f * s, sr * 0.8f, OVL_G_ZR, 0, DM_BTN_ZR };
#endif
    g_c[C_MINUS] = (ctl){ w * 0.5f - 0.13f * s, 0.06f * s, mr, OVL_G_MINUS, 1, DM_BTN_MINUS };
    g_c[C_PLUS] = (ctl){ w * 0.5f + 0.13f * s, 0.06f * s, mr, OVL_G_PLUS, 1, DM_BTN_PLUS };
    g_c[C_STICK] = (ctl){ 0.36f * s, h - 0.27f * s, 0.14f * s, OVL_G_NONE, 1, 0 };
    {   // the D-pad below-right of the left stick, as on a left Joy-Con (bag pockets, menus): four small buttons with a dot towards their direction
        float dx = 0.80f * s, dy = h - 0.19f * s, dg = 0.085f * s, dr = 0.050f * s;
        g_c[C_UP] = (ctl){ dx, dy - dg, dr, OVL_G_NONE, 1, DM_BTN_UP }; g_c[C_DOWN] = (ctl){ dx, dy + dg, dr, OVL_G_NONE, 1, DM_BTN_DOWN };
        g_c[C_LEFT] = (ctl){ dx - dg, dy, dr, OVL_G_NONE, 1, DM_BTN_LEFT }; g_c[C_RIGHT] = (ctl){ dx + dg, dy, dr, OVL_G_NONE, 1, DM_BTN_RIGHT };
    }
    // THROW (Legends: Arceus): a Poké Ball where the right thumb rests, above-left of A B X Y: held = ZR (aim, the finger's drag turns the aim), released = throw
    g_c[C_THROW] = (ctl){ w - 0.66f * s, h - 0.56f * s, 0.10f * s, OVL_G_NONE, GESTURES_ON, DM_BTN_ZR };
    g_c[C_RSTICK] = (ctl){ w - 0.80f * s, h - 0.19f * s, 0.11f * s, OVL_G_NONE, 1, 0 };
    // touch-first games (Legends: Arceus): only the movement stick and − / + on screen; everything else is touch on the game itself (touch_ui_event's gestures, touch_menu.c)
    if (GESTURES_ON) for (int c = C_RSTICK; c < NCTL; c++) if (c != C_PLUS && c != C_MINUS) g_c[c].shown = 0;
    if (!g_rstick_on) { g_rknob_x = g_c[C_RSTICK].cx; g_rknob_y = g_c[C_RSTICK].cy; }
    layout_apply(w, h);
    if (!g_stick_on) { g_stick_x = g_knob_x = g_c[C_STICK].cx; g_stick_y = g_knob_y = g_c[C_STICK].cy; }
}
static int hit(int c, float x, float y, float slack) {
    const ctl *k = &g_c[c]; float dx = x - k->cx, dy = y - k->cy, r = k->r * slack;
    return k->shown && dx * dx + dy * dy <= r * r;
}
static int target_at(float x, float y) {
    if (hit(C_RSTICK, x, y, 1.3f)) return C_RSTICK;
    for (int c = C_A; c < NCTL; c++) if (hit(c, x, y, 1.3f)) return c;   // thumbs miss: touch areas 1.3 times the drawing
    if (x < g_w * 0.42f && y > g_h * 0.30f) return C_STICK;
    if (x > g_w * 0.45f) return C_CAM;
    return -1;
}
static int32_t ax(float v) { v = clampf(v, -1, 1); return (int32_t)(v * DM_STICK_MAX); }

#ifdef __ANDROID__
void touch_menu_tap(float x, float y, float w, float h);   // touch_menu.c
// ---------- haptics ----------
static JavaVM *g_vm; static jobject g_act; static pthread_cond_t g_hcv = PTHREAD_COND_INITIALIZER; static int g_hpend;
static void *haptic_main(void *p) {
    (void)p; JNIEnv *e; if ((*g_vm)->AttachCurrentThread(g_vm, &e, NULL)) return NULL;
    jclass ctx = (*e)->FindClass(e, "android/content/Context"); jmethodID gss = (*e)->GetMethodID(e, ctx, "getSystemService", "(Ljava/lang/String;)Ljava/lang/Object;");
    jstring name = (*e)->NewStringUTF(e, "vibrator"); jobject vib = (*e)->CallObjectMethod(e, g_act, gss, name);
    jclass vc = (*e)->FindClass(e, "android/os/Vibrator"), ec = (*e)->FindClass(e, "android/os/VibrationEffect");
    jmethodID vibrate = vc ? (*e)->GetMethodID(e, vc, "vibrate", "(Landroid/os/VibrationEffect;)V") : NULL;
    jmethodID pre = ec ? (*e)->GetStaticMethodID(e, ec, "createPredefined", "(I)Landroid/os/VibrationEffect;") : NULL;
    if ((*e)->ExceptionCheck(e)) (*e)->ExceptionClear(e);
    jobject click = pre ? (*e)->CallStaticObjectMethod(e, ec, pre, 0 /* EFFECT_CLICK */) : NULL;
    if (click) click = (*e)->NewGlobalRef(e, click);
    if (!vib || !vibrate || !click) { __android_log_print(ANDROID_LOG_WARN, "nxnative", "haptics unavailable"); (*g_vm)->DetachCurrentThread(g_vm); return NULL; }
    vib = (*e)->NewGlobalRef(e, vib);
    for (;;) {
        pthread_mutex_lock(&g_mu); while (!g_hpend) pthread_cond_wait(&g_hcv, &g_mu); g_hpend = 0; pthread_mutex_unlock(&g_mu);
        (*e)->CallVoidMethod(e, vib, vibrate, click); if ((*e)->ExceptionCheck(e)) (*e)->ExceptionClear(e);
    }
    return NULL;
}
void touch_ui_init(ANativeActivity *a) {
    g_vm = a->vm; g_act = a->clazz;
    pthread_t t; if (!pthread_create(&t, NULL, haptic_main, NULL)) pthread_detach(t);
}
static void click_locked(void) { g_hpend = 1; pthread_cond_signal(&g_hcv); }   // callers hold g_mu
#endif
#ifdef __ANDROID__

// ---------- touch ----------
static void recompute_locked(void) {
    uint64_t b = 0; float lx = 0, ly = 0, rx = 0, ry = 0; int stick = 0, rstick = 0; double t = now_s();
    for (int i = 0; i < 10; i++) {
        if (g_ptr[i].id < 0) continue;
        int c = g_ptr[i].target;
        if (c == C_STICK) { float r = g_c[C_STICK].r, dx = g_ptr[i].x - g_stick_x, dy = g_ptr[i].y - g_stick_y, d = sqrtf(dx * dx + dy * dy);
            if (d > r) { dx *= r / d; dy *= r / d; }
            float nx = dx / r, ny = -dy / r; if (nx * nx + ny * ny < 0.12f * 0.12f) nx = ny = 0;   // dead zone
            lx = nx; ly = ny; stick = 1; g_knob_x = g_stick_x + dx; g_knob_y = g_stick_y + dy; }
        else if (c == C_RSTICK) { const ctl *k = &g_c[C_RSTICK]; float r = k->r, dx = g_ptr[i].x - k->cx, dy = g_ptr[i].y - k->cy, d = sqrtf(dx * dx + dy * dy);
            if (d > r) { dx *= r / d; dy *= r / d; }
            float nx = dx / r, ny = -dy / r; if (nx * nx + ny * ny < 0.12f * 0.12f) nx = ny = 0;
            rx = nx; ry = ny; rstick = 1; g_rknob_x = k->cx + dx; g_rknob_y = k->cy + dy; }
        else if (c >= C_A) b |= g_c[c].btn;
    }
    for (int c = C_A; c < NCTL; c++) if (t < g_held_until[c]) b |= g_c[c].btn;
    for (int i = 0; i < 10; i++) if (g_ptr[i].id >= 0 && g_ptr[i].thr) b |= DM_BTN_ZR;
    for (int i = 0; i < 10; i++) if (GESTURES_ON && g_ptr[i].id >= 0 && g_ptr[i].target == C_CAM && !g_ptr[i].thr && !touch_menu_active() && t - g_ptr[i].t0 > 0.45 &&
                                     fabsf(g_ptr[i].x - g_ptr[i].x0) < g_h * 0.03f && fabsf(g_ptr[i].y - g_ptr[i].y0) < g_h * 0.03f) { g_ptr[i].lp = 1; b |= DM_BTN_ZL; }   // press and hold: ZL (lock on)
    for (int i = 0; i < 10; i++) if (g_ptr[i].id >= 0 && g_ptr[i].lp) b |= DM_BTN_ZL;   // a throw swipe holds ZR (aim) until the finger lifts (throw)
    if (rstick || (b & (DM_BTN_UP | DM_BTN_DOWN | DM_BTN_LEFT | DM_BTN_RIGHT))) g_inner_t = t;   // held: stays clear
    g_rstick_on = rstick; if (!rstick) { g_rknob_x = g_c[C_RSTICK].cx; g_rknob_y = g_c[C_RSTICK].cy; }
    if (!stick) { g_stick_on = 0; g_stick_x = g_knob_x = g_c[C_STICK].cx; g_stick_y = g_knob_y = g_c[C_STICK].cy; }
    // the camera: the finger's speed, full turn at about a screen height per 0.6 s, at rest within 60 ms of the finger stopping
    float vmax = g_h / 0.6f; if (t - g_cam_t > 0.06) g_cam_vx = g_cam_vy = 0;
    g_btn = b; g_lx = ax(lx); g_ly = ax(ly); g_rx = rstick ? ax(rx) : ax(g_cam_vx / vmax); g_ry = rstick ? ax(ry) : ax(-g_cam_vy / vmax);
}
// ---------- the layout editor: hold − for 2 s to enter; drag a control to move it, pinch on it to resize it, double-tap it to hide / show it;
// hold + for 2 s to go back to the default layout; hold − for 2 s again to save and leave. The game gets no input meanwhile ----------
static double g_edit_hold_t;   // (g_edit_hold_t: when the hold that toggles the editor started)
static struct { int32_t id; int c; float ox, oy, x0, y0; double t0; } g_ep[10];
static struct { int c, active; float d0, r0; } g_pinch;
static int g_last_tap_c = -1; static double g_last_tap_t;
static int edit_hit(float x, float y) {   // the smallest control (shown or not) under the finger
    int best = -1; float br = 1e9f;
    for (int c = 0; c < NCTL; c++) { if (c == C_CAM || (!g_c[c].shown && !g_ov[c].set && c != C_STICK)) continue;
        if (GESTURES_ON && !g_c[c].shown && !g_ov[c].set && c != C_PLUS && c != C_MINUS) continue;
        float dx = x - g_c[c].cx, dy = y - g_c[c].cy, r = g_c[c].r * 1.2f; if (dx * dx + dy * dy <= r * r && g_c[c].r < br) { br = g_c[c].r; best = c; } }
    return best;
}
static void edit_store(int c) { g_ov[c].set = 1; g_ov[c].fx = g_c[c].cx / g_w; g_ov[c].fy = g_c[c].cy / g_h; g_ov[c].fr = g_c[c].r / g_h; g_ov[c].shown = g_c[c].shown; }
static void edit_event(const AInputEvent *e) {
    int32_t act = AMotionEvent_getAction(e) & AMOTION_EVENT_ACTION_MASK;
    size_t idx = (size_t)((AMotionEvent_getAction(e) & AMOTION_EVENT_ACTION_POINTER_INDEX_MASK) >> AMOTION_EVENT_ACTION_POINTER_INDEX_SHIFT);
    if (act == AMOTION_EVENT_ACTION_CANCEL) { for (int i = 0; i < 10; i++) g_ep[i].id = -1; g_pinch.active = 0; return; }
    if (act == AMOTION_EVENT_ACTION_DOWN || act == AMOTION_EVENT_ACTION_POINTER_DOWN) {
        size_t k = act == AMOTION_EVENT_ACTION_DOWN ? 0 : idx; float x = AMotionEvent_getX(e, k), y = AMotionEvent_getY(e, k);
        if (act == AMOTION_EVENT_ACTION_DOWN) { for (int i = 0; i < 10; i++) g_ep[i].id = -1; g_pinch.active = 0; }
        int c = edit_hit(x, y);
        for (int i = 0; i < 10; i++) if (g_ep[i].id >= 0 && c >= 0 && g_ep[i].c == c) {   // a second finger on the same control: pinch
            float dx = x - g_ep[i].x0, dy = y - g_ep[i].y0; g_pinch.c = c; g_pinch.active = 1; g_pinch.d0 = sqrtf(dx * dx + dy * dy) + 1; g_pinch.r0 = g_c[c].r; break; }
        for (int i = 0; i < 10; i++) if (g_ep[i].id < 0) { g_ep[i].id = AMotionEvent_getPointerId(e, k); g_ep[i].c = c; g_ep[i].x0 = x; g_ep[i].y0 = y; g_ep[i].t0 = now_s();
            g_ep[i].ox = c >= 0 ? g_c[c].cx - x : 0; g_ep[i].oy = c >= 0 ? g_c[c].cy - y : 0; break; }
    } else if (act == AMOTION_EVENT_ACTION_MOVE) {
        float px[10], py[10]; int n = 0;
        for (size_t k = 0; k < AMotionEvent_getPointerCount(e); k++) { int32_t id = AMotionEvent_getPointerId(e, k); float x = AMotionEvent_getX(e, k), y = AMotionEvent_getY(e, k);
            for (int i = 0; i < 10; i++) if (g_ep[i].id == id && g_ep[i].c >= 0) {
                if (g_pinch.active && g_pinch.c == g_ep[i].c) { if (n < 10) { px[n] = x; py[n] = y; n++; } }
                else { int c = g_ep[i].c; g_c[c].cx = clampf(x + g_ep[i].ox, g_c[c].r, g_w - g_c[c].r); g_c[c].cy = clampf(y + g_ep[i].oy, g_c[c].r, g_h - g_c[c].r); edit_store(c); } } }
        if (g_pinch.active && n >= 2) { float dx = px[0] - px[1], dy = py[0] - py[1]; int c = g_pinch.c;
            g_c[c].r = clampf(g_pinch.r0 * (sqrtf(dx * dx + dy * dy) + 1) / g_pinch.d0, g_h * 0.025f, g_h * 0.25f); edit_store(c); }
    } else if (act == AMOTION_EVENT_ACTION_UP || act == AMOTION_EVENT_ACTION_POINTER_UP) {
        int32_t id = AMotionEvent_getPointerId(e, act == AMOTION_EVENT_ACTION_UP ? 0 : idx); double t = now_s();
        for (int i = 0; i < 10; i++) if (g_ep[i].id == id) { int c = g_ep[i].c;
            float mx = AMotionEvent_getX(e, act == AMOTION_EVENT_ACTION_UP ? 0 : idx) - g_ep[i].x0, my = AMotionEvent_getY(e, act == AMOTION_EVENT_ACTION_UP ? 0 : idx) - g_ep[i].y0;
            if (c >= 0 && t - g_ep[i].t0 < 0.3 && mx * mx + my * my < g_h * g_h * 0.0004f) {   // a tap: a double tap toggles the control
                if (g_last_tap_c == c && t - g_last_tap_t < 0.4 && c != C_PLUS && c != C_MINUS) { g_c[c].shown = !g_c[c].shown; edit_store(c); g_last_tap_c = -1; click_locked(); }
                else { g_last_tap_c = c; g_last_tap_t = t; } }
            if (g_pinch.active && g_pinch.c == c) g_pinch.active = 0;
            g_ep[i].id = -1; }
    }
}
// the 2-second holds, checked once a frame (a finger held still sends no events): − toggles the editor (saving on the way out), + in the editor resets the layout
static void edit_holds_locked(void) {
    double t = now_s(); int minus = 0, plus = 0;
    if (g_edit) { for (int i = 0; i < 10; i++) if (g_ep[i].id >= 0 && !g_pinch.active) { if (g_ep[i].c == C_MINUS) minus = 1; if (g_ep[i].c == C_PLUS) plus = 1; } }
    else for (int i = 0; i < 10; i++) if (g_ptr[i].id >= 0 && g_ptr[i].target == C_MINUS) minus = 1;
    if (!minus && !plus) { g_edit_hold_t = 0; return; }
    if (!g_edit_hold_t) { g_edit_hold_t = t; return; }
    if (t - g_edit_hold_t < 2.0) return;
    g_edit_hold_t = 1e18;   // fired: nothing more until the finger lifts
    if (plus && g_edit) { memset(g_ov, 0, sizeof g_ov); remove(g_lay_path); g_w = g_h = 0; click_locked(); fprintf(stderr, "[touch] layout reset to the default\n"); return; }
    if (g_edit) { layout_save(); g_edit = 0; for (int i = 0; i < 10; i++) g_ep[i].id = -1; fprintf(stderr, "[touch] layout saved: %s\n", g_lay_path); }
    else { g_edit = 1; for (int i = 0; i < 10; i++) { if (g_ptr[i].id >= 0) { g_ep[0].id = g_ptr[i].id; g_ep[0].c = C_MINUS; g_ep[0].t0 = t; } g_ptr[i].id = -1; }
           g_btn = 0; g_lx = g_ly = g_rx = g_ry = 0; dm_input_set(0, 0, 0, 0, 0); fprintf(stderr, "[touch] layout editor on\n"); }
    click_locked();
}
// Returns the touch buttons and sticks (for the host's touch source) after this event
void touch_ui_event(const AInputEvent *e, float w, float h, uint64_t *btn, int32_t s[4]) {
    pthread_mutex_lock(&g_mu);
    layout(w, h); g_visible = 1; g_touch_t = now_s();
    if (g_edit) { edit_event(e); *btn = 0; s[0] = s[1] = s[2] = s[3] = 0; pthread_mutex_unlock(&g_mu); return; }
    int32_t act = AMotionEvent_getAction(e) & AMOTION_EVENT_ACTION_MASK;
    size_t idx = (size_t)((AMotionEvent_getAction(e) & AMOTION_EVENT_ACTION_POINTER_INDEX_MASK) >> AMOTION_EVENT_ACTION_POINTER_INDEX_SHIFT);
    uint64_t before = g_btn;
    if (act == AMOTION_EVENT_ACTION_CANCEL) for (int i = 0; i < 10; i++) g_ptr[i].id = -1;
    else if (act == AMOTION_EVENT_ACTION_DOWN || act == AMOTION_EVENT_ACTION_POINTER_DOWN) {
        size_t k = act == AMOTION_EVENT_ACTION_DOWN ? 0 : idx; float x = AMotionEvent_getX(e, k), y = AMotionEvent_getY(e, k);
        if (act == AMOTION_EVENT_ACTION_DOWN) for (int i = 0; i < 10; i++) g_ptr[i].id = -1;
        int t = target_at(x, y);
        if (t >= 0) for (int i = 0; i < 10; i++) if (g_ptr[i].id < 0) {
            if (t == C_STICK || t == C_CAM) for (int j = 0; j < 10; j++) if (g_ptr[j].id >= 0 && (g_ptr[j].target == C_STICK || g_ptr[j].target == C_CAM) && now_s() - g_ptr[j].t0 < 0.25) g_two_t = now_s();
            g_ptr[i].thr = g_ptr[i].lp = 0; g_ptr[i].id = AMotionEvent_getPointerId(e, k); g_ptr[i].target = t; g_ptr[i].x0 = g_ptr[i].x = g_ptr[i].lx = x; g_ptr[i].y0 = g_ptr[i].y = g_ptr[i].ly = y; g_ptr[i].lt = g_ptr[i].t0 = now_s();
            if (t >= C_A) g_held_until[t] = now_s() + 0.08;
            if (t == C_RSTICK || (t >= C_UP && t <= C_RIGHT)) g_inner_t = now_s();
            if (t == C_STICK) { float r = g_c[C_STICK].r; g_stick_on = 1;   // the base comes to the thumb, kept on screen
                (void)r; g_stick_x = g_c[C_STICK].cx; g_stick_y = g_c[C_STICK].cy; }   // fixed, as on a Joy-Con: the thumb's offset from the base is the tilt
            break; }
    } else if (act == AMOTION_EVENT_ACTION_UP || act == AMOTION_EVENT_ACTION_POINTER_UP) {
        int32_t id = AMotionEvent_getPointerId(e, act == AMOTION_EVENT_ACTION_UP ? 0 : idx);
        // gestures outside the controls (stick or camera area): a quick tap (under 0.25 s, barely moved) is a tap on a menu item while a menu shows (touch_menu.c),
        // else, on the camera side, A (talk, pick up, investigate); two fingers tapping together are B (back)
        for (int i = 0; i < 10; i++) if (g_ptr[i].id == id && (g_ptr[i].target == C_STICK || g_ptr[i].target == C_CAM) && !g_ptr[i].thr && now_s() - g_ptr[i].t0 < 0.3 &&
                                         fabsf(g_ptr[i].x - g_ptr[i].x0) < g_h * 0.02f && fabsf(g_ptr[i].y - g_ptr[i].y0) < g_h * 0.02f) {
            double t = now_s();
            if (t - g_two_t < 0.4) { g_two_t = 0; g_tap_mute = t + 0.3; dm_input_press(DM_BTN_B, 80000000ull); click_locked(); }
            else if (t >= g_tap_mute && !g_ptr[i].lp) { static int dg = -1; if (dg < 0) dg = getenv("DM_TOUCH_DIAG") != NULL; if (dg && !touch_menu_active()) touch_menu_tap(g_ptr[i].x0, g_ptr[i].y0, g_w, g_h);   // (diagnosis: what is under the tap)
                if (touch_menu_active()) touch_menu_tap(g_ptr[i].x0, g_ptr[i].y0, g_w, g_h); else if (GESTURES && g_ptr[i].target == C_CAM) { dm_input_press(DM_BTN_A, 80000000ull); click_locked(); } }
        }
        // quick swipes on the camera side (under 0.35 s, a tenth of the screen height or more): left / right = L / R (the item or Pokémon), down = X (items / Pokémon)
        for (int i = 0; i < 10; i++) if (GESTURES && g_ptr[i].id == id && g_ptr[i].target == C_CAM && !g_ptr[i].thr && !g_ptr[i].lp && now_s() - g_ptr[i].t0 < 0.35 && !touch_menu_active()) {
            float sx = g_ptr[i].x - g_ptr[i].x0, sy = g_ptr[i].y - g_ptr[i].y0; uint64_t sb = 0;
            if (fabsf(sx) > g_h * 0.10f && fabsf(sy) < fabsf(sx) * 0.6f) sb = sx > 0 ? DM_BTN_R : DM_BTN_L;
            else if (sy > g_h * 0.10f && fabsf(sx) < sy * 0.6f) sb = DM_BTN_X;
            if (sb) { dm_input_press(sb, 80000000ull); click_locked(); g_cam_vx = g_cam_vy = 0; }
        }
        for (int i = 0; i < 10; i++) if (g_ptr[i].id == id) { if (g_ptr[i].target == C_CAM) g_cam_vx = g_cam_vy = 0; g_ptr[i].id = -1; }
    }
    if (act == AMOTION_EVENT_ACTION_MOVE || act == AMOTION_EVENT_ACTION_DOWN || act == AMOTION_EVENT_ACTION_POINTER_DOWN)
        for (size_t k = 0; k < AMotionEvent_getPointerCount(e); k++) {
            int32_t id = AMotionEvent_getPointerId(e, k);
            for (int i = 0; i < 10; i++) if (g_ptr[i].id == id) {
                g_ptr[i].x = AMotionEvent_getX(e, k); g_ptr[i].y = AMotionEvent_getY(e, k);
                if (g_ptr[i].target == C_CAM && !g_ptr[i].thr && act == AMOTION_EVENT_ACTION_MOVE && GESTURES && now_s() - g_ptr[i].t0 < 0.3 && !touch_menu_active()) {   // a quick swipe up: a throw
                    float up = g_ptr[i].y0 - g_ptr[i].y; if (up > g_h * 0.10f && fabsf(g_ptr[i].x - g_ptr[i].x0) < up * 0.5f) { g_ptr[i].thr = 1; g_cam_vx = g_cam_vy = 0; click_locked(); }
                }
                if ((g_ptr[i].target == C_CAM || g_ptr[i].target == C_THROW) && !g_ptr[i].thr && act == AMOTION_EVENT_ACTION_MOVE) {   // the finger's speed, smoothed (the throw button's finger aims)
                    double t = now_s(), dt = t - g_ptr[i].lt;
                    if (dt > 0.001) { float vx = (g_ptr[i].x - g_ptr[i].lx) / (float)dt, vy = (g_ptr[i].y - g_ptr[i].ly) / (float)dt;
                        g_cam_vx = g_cam_vx * 0.5f + vx * 0.5f; g_cam_vy = g_cam_vy * 0.5f + vy * 0.5f; g_cam_t = t;
                        g_ptr[i].lx = g_ptr[i].x; g_ptr[i].ly = g_ptr[i].y; g_ptr[i].lt = t; }
                }
                if (g_ptr[i].target >= C_A && g_ptr[i].target <= C_Y)   // sliding across the face buttons
                    for (int c = C_A; c <= C_Y; c++) if (c != g_ptr[i].target && hit(c, g_ptr[i].x, g_ptr[i].y, 1.0f)) { g_ptr[i].target = c; g_held_until[c] = now_s() + 0.08; break; }
            }
        }
    recompute_locked();
    if (g_btn & ~before) click_locked();   // a new press: one click
    *btn = g_btn; s[0] = g_lx; s[1] = g_ly; s[2] = g_rx; s[3] = g_ry;
    pthread_mutex_unlock(&g_mu);
}
void touch_ui_hide(void) { pthread_mutex_lock(&g_mu); g_visible = 0; pthread_mutex_unlock(&g_mu); }
// GameActivity.java: a controller connected hides the controls at once (no input needed first); the last one disconnected shows them again
void touch_ui_pad(int connected) { pthread_mutex_lock(&g_mu); g_visible = !connected; if (!connected) g_touch_t = now_s(); pthread_mutex_unlock(&g_mu); }
#else
// Mac test hook (DM_OVL_SHOT): show control c as held, the stick pushed by (dx, dy) in units of its radius
void touch_ui_demo(int c, float dx, float dy) {
    pthread_mutex_lock(&g_mu); layout(1, 1); g_ptr[0].id = 1; g_ptr[0].target = c; g_btn = c >= C_A && c < NCTL ? g_c[c].btn : 0; g_w = g_h = 0; pthread_mutex_unlock(&g_mu);
    (void)dx; (void)dy;
}
#endif

// ---------- drawing ----------
// The front end's input push for what changes with time alone (a tap's hold running out, the camera coming to rest): called once a frame by the drawing
#ifdef __ANDROID__
static void frame_input_locked(void) {
    edit_holds_locked(); if (g_edit) return;   // (the editor: the game gets nothing)
    uint64_t b0 = g_btn; int32_t r0 = g_rx, r1 = g_ry;
    recompute_locked();
    if (g_btn != b0 || g_rx != r0 || g_ry != r1) dm_input_set(g_btn, g_lx, g_ly, g_rx, g_ry);
}
#else
static void frame_input_locked(void) {}
#endif
static dm_ovl_item *add(dm_ovl_item *it, int *n, int max, float cx, float cy, float r, int shape, int R, int G, int B, float a) {
    if (*n >= max) return NULL;
    dm_ovl_item *o = &it[(*n)++];
    *o = (dm_ovl_item){ cx - r, cy - r, 2 * r, 2 * r, clampf(R, 0, 255) / 255.0f, clampf(G, 0, 255) / 255.0f, clampf(B, 0, 255) / 255.0f, clampf(a, 0, 1), shape, 0.1f, 0.4f, OVL_G_NONE, 0.5f, 0 };
    return o;
}
// a glossy bubble: shadow, dark rim, body, glow below, a gloss spot, a bright rim; the label on top
static void bubble(dm_ovl_item *it, int *n, int max, const ctl *c, int on, int teal, float a) {
    float r = on ? c->r * 0.95f : c->r, x = c->cx, y = c->cy, th = g_h / 1080.0f;
    int br = teal ? 30 : 38, bg = teal ? 170 : 128, bb = teal ? 150 : 200;
    if (on) { br += 70; bg += 60; bb += 40; }
    add(it, n, max, x, y + r * 0.06f, r * 1.02f, OVL_DISC, 5, 30, 60, 0.35f * a);
    add(it, n, max, x, y, r, OVL_DISC, br / 2, bg / 2, bb / 2, 0.80f * a);
    add(it, n, max, x, y, r * 0.90f, OVL_DISC, br, bg, bb, (on ? 0.95f : 0.72f) * a);
    add(it, n, max, x, y + r * 0.22f, r * 0.62f, OVL_DISC, br + 30, bg + 40, bb + 30, 0.35f * a);
    add(it, n, max, x - r * 0.30f, y - r * 0.40f, r * 0.24f, OVL_DISC, 255, 255, 255, 0.28f * a);
    dm_ovl_item *o = add(it, n, max, x, y, r * 0.90f, OVL_RING, 185, 232, 255, 0.95f * a); if (o) o->ring = clampf(2.8f * th / (r * 0.9f), 0.02f, 0.2f);
    o = add(it, n, max, x, y, r, OVL_DISC, 0, 0, 0, 0); if (o) { o->glyph = c->glyph; o->gscale = c->glyph == OVL_G_ZL || c->glyph == OVL_G_ZR ? 0.62f : 0.55f; o->galpha = a; }
}
// the throw button: a Poké Ball (shadow, dark rim, red, a dark band, the white centre button), pressed = slightly smaller and brighter
static void pokeball(dm_ovl_item *it, int *n, int max, const ctl *c, int on, float a) {
    float r = on ? c->r * 0.93f : c->r, x = c->cx, y = c->cy, th = g_h / 1080.0f; int lift = on ? 40 : 0;
    add(it, n, max, x, y + r * 0.07f, r * 1.03f, OVL_DISC, 5, 20, 40, 0.35f * a);
    add(it, n, max, x, y, r, OVL_DISC, 30, 30, 36, 0.92f * a);
    add(it, n, max, x, y, r * 0.90f, OVL_DISC, 225 + lift / 3, 40 + lift, 45 + lift, 0.95f * a);
    add(it, n, max, x, y + r * 0.45f, r * 0.62f, OVL_DISC, 245, 245, 245, 0.55f * a);
    dm_ovl_item *o = add(it, n, max, x, y, r * 0.90f, OVL_RRECT, 30, 30, 36, 0.95f * a); if (o) { o->y = y - r * 0.09f; o->h = r * 0.18f; o->radius = 0.5f; }
    add(it, n, max, x, y, r * 0.30f, OVL_DISC, 30, 30, 36, 0.95f * a);
    add(it, n, max, x, y, r * 0.21f, OVL_DISC, 250, 250, 250, 0.98f * a);
    add(it, n, max, x - r * 0.32f, y - r * 0.45f, r * 0.18f, OVL_DISC, 255, 255, 255, 0.35f * a);
    o = add(it, n, max, x, y, r, OVL_RING, 210, 225, 240, 0.9f * a); if (o) o->ring = clampf(2.6f * th / r, 0.01f, 0.2f);
}
// the stick: a ring with an inner ring, and a glossy knob
static void compass(dm_ovl_item *it, int *n, int max, float a, float g_stick_x, float g_stick_y, float g_knob_x, float g_knob_y, float r) {   // (base, knob, radius)
    float th = g_h / 1080.0f, kr = r * 0.42f;
    add(it, n, max, g_stick_x, g_stick_y, r, OVL_DISC, 30, 100, 165, 0.45f * a);
    dm_ovl_item *o = add(it, n, max, g_stick_x, g_stick_y, r, OVL_RING, 190, 235, 255, 0.80f * a); if (o) o->ring = clampf(2.6f * th / r, 0.01f, 0.2f);
    o = add(it, n, max, g_stick_x, g_stick_y, r * 0.80f, OVL_RING, 190, 235, 255, 0.35f * a); if (o) o->ring = clampf(1.6f * th / (r * 0.8f), 0.01f, 0.2f);
    add(it, n, max, g_knob_x, g_knob_y + kr * 0.12f, kr, OVL_DISC, 0, 0, 0, 0.25f * a);
    add(it, n, max, g_knob_x, g_knob_y, kr, OVL_DISC, 70, 165, 225, 0.85f * a);
    add(it, n, max, g_knob_x, g_knob_y - kr * 0.32f, kr * 0.6f, OVL_DISC, 255, 255, 255, 0.14f * a);
    o = add(it, n, max, g_knob_x, g_knob_y, kr, OVL_RING, 220, 245, 255, 0.95f * a); if (o) o->ring = clampf(2.4f * th / kr, 0.02f, 0.3f);
}
// the player's settings (env.txt, set on the app's settings screen): DM_TOUCH_OPACITY 20-100 (%), DM_TOUCH_FADE seconds untouched before the controls go faint (0 = never)
static float g_opacity = -1, g_fade = 3.0f;
static void prefs(void) {
    if (g_opacity >= 0) return;
    const char *o = getenv("DM_TOUCH_OPACITY"), *f = getenv("DM_TOUCH_FADE");
    g_opacity = o ? clampf((float)atof(o) / 100.0f, 0.2f, 1.0f) : 1.0f; if (f) g_fade = fmaxf(0.0f, (float)atof(f));
}
int front_overlay(dm_ovl_item *it, int max, int width, int height) {
    pthread_mutex_lock(&g_mu); prefs();
    layout((float)width, (float)height);
    frame_input_locked();
    int n = 0;
    if (g_visible && g_edit) {   // the editor: every control at full strength with a yellow ring, the hidden ones as ghosts
        for (int c = 0; c < NCTL; c++) { if (c == C_CAM) continue; const ctl *k = &g_c[c];
            if (GESTURES_ON && !k->shown && !g_ov[c].set && c != C_PLUS && c != C_MINUS && c != C_STICK) continue;
            float a = k->shown ? 1.0f : 0.3f;
            if (c == C_STICK || c == C_RSTICK) compass(it, &n, max, a, k->cx, k->cy, k->cx, k->cy, k->r);
            else if (c == C_THROW) pokeball(it, &n, max, k, 0, a); else bubble(it, &n, max, k, 0, 0, a);
            dm_ovl_item *o = add(it, &n, max, k->cx, k->cy, k->r * 1.12f, OVL_RING, 255, 210, 60, 0.95f * a); if (o) o->ring = clampf(4.0f * (g_h / 1080.0f) / (k->r * 1.12f), 0.02f, 0.3f); }
    } else if (g_visible) {
        double t = now_s(); int idle = ((g_fade > 0 && t - g_touch_t > g_fade) || touch_menu_active()) && !g_btn && !g_stick_on && !g_rstick_on;   // (in a menu its items are tapped directly)   // nothing touched for 3 s: everything faint
        float ia = idle ? 0.22f : 1.0f;
        int faint = t - g_inner_t > 2.0;   // the D-pad and the right stick fade 2 s after their last touch
        for (int c = C_A; c < NCTL; c++) { if (!g_c[c].shown) continue;
            int on = (g_btn & g_c[c].btn) != 0; float al = c >= C_UP && c <= C_RIGHT && faint && !on ? 0.25f : 1.0f; if (idle) al = fminf(al, ia);
            if (c == C_THROW) { pokeball(it, &n, max, &g_c[c], on, al); continue; }
            bubble(it, &n, max, &g_c[c], on, 0, al);
            if (c >= C_UP && al == 1.0f) { float r = g_c[c].r, ox = c == C_LEFT ? -0.42f : c == C_RIGHT ? 0.42f : 0, oy = c == C_UP ? -0.42f : c == C_DOWN ? 0.42f : 0;
                add(it, &n, max, g_c[c].cx + ox * r, g_c[c].cy + oy * r, r * 0.22f, OVL_DISC, 255, 255, 255, 0.9f); } }
        compass(it, &n, max, g_stick_on ? 1.0f : idle ? ia : 0.75f, g_stick_x, g_stick_y, g_knob_x, g_knob_y, g_c[C_STICK].r);
        if (g_c[C_RSTICK].shown) compass(it, &n, max, g_rstick_on ? 1.0f : idle ? ia : faint ? 0.25f : 0.75f, g_c[C_RSTICK].cx,   // faint while unused: it and the D-pad sit over the dialogue box
             g_c[C_RSTICK].cy, g_rknob_x, g_rknob_y, g_c[C_RSTICK].r);
        for (int i = 0; i < n; i++) { it[i].a *= g_opacity; it[i].galpha *= g_opacity; }
    }
    pthread_mutex_unlock(&g_mu);
    return n;
}
