// Android app front end (NativeActivity + android_native_app_glue): game files from the app's external files folder, the host (dm_main) on a big-stack thread, the window handed to
// the Vulkan replay for presentation, controllers and touch into the game's controller state, lifecycle (focus, background) into the host.
// Files, under Android/data/<package>/files/ (copied there from a computer, see dm/android/README.md):
//   RomFS/            the game's RomFS
//   patched/          the code modules rewritten for Android (patch_nso.py, DM_PATCH_TARGET=android)
//   shaders.pak       the Vulkan shaders packed (dm/android/tools/spv_pak.py), or vk/b-<chain>/ with <key>.spv + <key>.vkr; shader_map.tsv beside them
//   font/             the shared fonts (standard.ttf, nintendo_ext.ttf)
//   env.txt           optional KEY=VALUE switches (as the iOS app's)
// Saves and the log (log.txt, the previous run in log.prev.txt) are written there too.
#include <android/log.h>
#include <android/native_activity.h>
#include <android/window.h>
#include <android/input.h>
#include <android_native_app_glue.h>
#include <pthread.h>
#include <sched.h>
#include <jni.h>
#include <sys/resource.h>
#include <dlfcn.h>
#include <signal.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <errno.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/mman.h>
#include <dirent.h>
#include <fcntl.h>
#include <time.h>
#include <unistd.h>
#include <math.h>
#include <ctype.h>
#include <strings.h>
#include <malloc.h>   // mallopt: the allocator's large-block cache (env.txt read)
#include <sys/system_properties.h>
#include "dm_input.h"
#include "front.h"
#include "game_profile.h"
#include "nvn_ovl.h"   // dm_aspect_mode (the in-game menu's picture setting)

int dm_main(int argc, char **argv);   // dm/host/main.c built with -Dmain=dm_main
void dm_focus_set(int st);
void dm_audio_suspend(int on);
#define ALOG(...) __android_log_print(ANDROID_LOG_INFO, "nxnative", __VA_ARGS__)

static struct android_app *g_app;
static _Atomic(ANativeWindow *) g_window;
static char g_files[1024], g_game[1024];
// The game this start is for: one app and one engine run the games of the profile's family (game.py DM_GAME_FAMILY: Pokémon Shield and Sword, the same engine build).
// The setup screen writes the title ID to <files>/current_game before starting the game; the profile's own game keeps the app's folder (as before: Shield players
// keep their setup and saves), another one has <files>/<title ID>/. Its program ID goes to DM_PROGRAM_ID, its name to the log files
static const struct { const char *id, *stem; } g_family[] = DM_GAME_FAMILY;
static const char *g_stem = DM_GAME_FILE_STEM;
static void choose_game(char *files, size_t n) {
    char p[1100], id[32] = ""; snprintf(p, sizeof p, "%s/current_game", files); FILE *f = fopen(p, "r");
    if (f) { if (fscanf(f, "%31s", id) != 1) id[0] = 0; fclose(f); }
    for (unsigned i = 0; i < sizeof g_family / sizeof *g_family; i++) if (id[0] && !strcasecmp(id, g_family[i].id)) {
        g_stem = g_family[i].stem; setenv("DM_PROGRAM_ID", g_family[i].id, 1);
        if (i) { size_t l = strlen(files); snprintf(files + l, n - l, "/%s", g_family[i].id); mkdir(files, 0755); }
        fprintf(stderr, "boot: game %s (%s)\n", g_family[i].id, g_family[i].stem); return; }
    setenv("DM_PROGRAM_ID", g_family[0].id, 1);
}
void *front_metal_layer(void) { return atomic_load(&g_window); }   // the window the Vulkan replay presents to (the name is the Apple front ends' interface)
void *front_android_window(void) { return atomic_load(&g_window); }
// The game's keyboard (its name entry): the app's Android dialog (app/java/com/nxnative/TextInput.java) with the phone's keyboard, on the UI thread; this game thread
// waits for it (hle_swkbd.c stops the game's clock meanwhile). 1 OK (out holds the text, UTF-8), 0 cancelled, -1 no dialog (then the game takes it as cancelled)
// The Java calls run on a thread of their own: the calling game thread runs on the game's stack, outside its pthread stack, and ART aborts when such a thread attaches
// (Thread::InitStack: "FindStackTop() > GetStackEnd()"), so the game thread only starts that thread and waits for it
extern int g_swkbd_mode, g_swkbd_numeric_max;   // hle_swkbd.c: what this request wants (numbers: the number pad)
static int text_input_jni(const char *title, const char *sub, const char *guide, const char *initial, int max_chars, char *out, int out_cap) {
    JavaVM *vm = g_app->activity->vm; JNIEnv *e; int attached = 0;
    if ((*vm)->GetEnv(vm, (void **)&e, JNI_VERSION_1_6) != JNI_OK) { if ((*vm)->AttachCurrentThread(vm, &e, NULL)) return -1; attached = 1; }
    int r = -1;
    jobject act = g_app->activity->clazz;
    jclass ac = (*e)->GetObjectClass(e, act);
    jobject cl = (*e)->CallObjectMethod(e, act, (*e)->GetMethodID(e, ac, "getClassLoader", "()Ljava/lang/ClassLoader;"));
    jclass clc = (*e)->FindClass(e, "java/lang/ClassLoader");
    jstring cn = (*e)->NewStringUTF(e, "com.nxnative.TextInput");
    jclass ti = (jclass)(*e)->CallObjectMethod(e, cl, (*e)->GetMethodID(e, clc, "loadClass", "(Ljava/lang/String;)Ljava/lang/Class;"), cn);
    if ((*e)->ExceptionCheck(e) || !ti) { (*e)->ExceptionClear(e); fprintf(stderr, "[RED] text input: the dialog class is missing\n"); goto done; }
    {
        jmethodID show = (*e)->GetStaticMethodID(e, ti, "show", "(Landroid/app/Activity;Ljava/lang/String;Ljava/lang/String;Ljava/lang/String;Ljava/lang/String;IZ)V");
        jmethodID st = (*e)->GetStaticMethodID(e, ti, "state", "()I"), tx = (*e)->GetStaticMethodID(e, ti, "text", "()Ljava/lang/String;");
        (*e)->CallStaticVoidMethod(e, ti, show, act, (*e)->NewStringUTF(e, title ? title : ""), (*e)->NewStringUTF(e, sub ? sub : ""), (*e)->NewStringUTF(e, guide ? guide : ""),
                                   (*e)->NewStringUTF(e, initial ? initial : ""), (jint)max_chars, (jboolean)(g_swkbd_mode == 1 || g_swkbd_numeric_max >= 0));
        if ((*e)->ExceptionCheck(e)) { (*e)->ExceptionDescribe(e); (*e)->ExceptionClear(e); goto done; }
        int s; while (!(s = (*e)->CallStaticIntMethod(e, ti, st))) usleep(50000);
        if (s == 1) { jstring js = (jstring)(*e)->CallStaticObjectMethod(e, ti, tx); const char *c = js ? (*e)->GetStringUTFChars(e, js, NULL) : NULL;
            snprintf(out, (size_t)out_cap, "%s", c ? c : ""); if (c) (*e)->ReleaseStringUTFChars(e, js, c); r = 1; }
        else r = 0;
        fprintf(stderr, "text input '%s': %s\n", title ? title : "", r == 1 ? "entered" : "cancelled");
    }
done:
    if (attached) (*vm)->DetachCurrentThread(vm);
    return r;
}
struct text_input_req { const char *title, *sub, *guide, *initial; int max_chars; char *out; int out_cap, r; };
static void *text_input_main(void *p) {
    struct text_input_req *q = p;
    q->r = text_input_jni(q->title, q->sub, q->guide, q->initial, q->max_chars, q->out, q->out_cap);
    return NULL;
}
int front_text_input(const char *title, const char *sub, const char *guide, const char *initial, int max_chars, char *out, int out_cap) {
    struct text_input_req q = { title, sub, guide, initial, max_chars, out, out_cap, -1 };
    pthread_t t; if (pthread_create(&t, NULL, text_input_main, &q)) { fprintf(stderr, "[RED] text input: no thread for the dialog\n"); return -1; }
    pthread_join(t, NULL);
    return q.r;
}
void front_save_failed(int err) { ALOG("save not written (errno %d)", err); }

// The in-game settings (app/java/com/nxnative/GameMenu.java, an Android dialog over the game): the touch controls' menu button (touch_ui.c), or holding − on a controller
// for 3 s (on_input). what: 0 = the menu, 1 = the controller buttons. Called from the input thread; the Java call runs on a thread of its own and returns at once
static void *open_menu_main(void *p) {
    int what = (int)(intptr_t)p; JavaVM *vm = g_app->activity->vm; JNIEnv *e;
    if ((*vm)->AttachCurrentThread(vm, &e, NULL)) return NULL;
    jobject act = g_app->activity->clazz; jclass ac = (*e)->GetObjectClass(e, act);
    jobject cl = (*e)->CallObjectMethod(e, act, (*e)->GetMethodID(e, ac, "getClassLoader", "()Ljava/lang/ClassLoader;"));
    jclass clc = (*e)->FindClass(e, "java/lang/ClassLoader");
    jclass gm = (jclass)(*e)->CallObjectMethod(e, cl, (*e)->GetMethodID(e, clc, "loadClass", "(Ljava/lang/String;)Ljava/lang/Class;"), (*e)->NewStringUTF(e, "com.nxnative.GameMenu"));
    if ((*e)->ExceptionCheck(e) || !gm) { (*e)->ExceptionClear(e); fprintf(stderr, "[RED] menu: GameMenu class missing\n"); }
    else { (*e)->CallStaticVoidMethod(e, gm, (*e)->GetStaticMethodID(e, gm, "show", "(Landroid/app/Activity;I)V"), act, (jint)what);
           if ((*e)->ExceptionCheck(e)) { (*e)->ExceptionDescribe(e); (*e)->ExceptionClear(e); } }
    (*vm)->DetachCurrentThread(vm);
    return NULL;
}
void front_open_menu(int what) {
    static _Atomic long long last_ms; struct timespec ts; clock_gettime(CLOCK_MONOTONIC, &ts); long long t = ts.tv_sec * 1000LL + ts.tv_nsec / 1000000;
    if (t - atomic_load(&last_ms) < 500) return;   // one dialog per press
    atomic_store(&last_ms, t);
    pthread_t th; if (!pthread_create(&th, NULL, open_menu_main, (void *)(intptr_t)what)) pthread_detach(th);
    fprintf(stderr, "menu: opened (%s)\n", what ? "controller buttons" : "settings");
}

// ---------- Input: controllers (key and motion events of gamepads / joysticks) and touch ----------
// Every controller keeps its own state and the game sees them merged into one: buttons of all of them, each stick axis from whichever controller moves it most. Android lists a
// pair of Joy-Cons as two devices (left: left stick, D-pad buttons, L / ZL / −; right: right stick, A B X Y, R / ZR / +), so held one in each hand they play as one controller
enum { NPAD = 8 };
static struct { int32_t dev; uint64_t btn, hat; int32_t lx, ly, rx, ry; } g_pads[NPAD];
static uint64_t g_pad_btn; static int32_t g_pad_lx, g_pad_ly, g_pad_rx, g_pad_ry;
static int pad_slot(int32_t dev) {
    for (int i = 0; i < NPAD; i++) if (g_pads[i].dev == dev + 2) return i;   // dev + 2: 0 marks a free slot, and the virtual keyboard's id is -1
    for (int i = 0; i < NPAD; i++) if (!g_pads[i].dev) { memset(&g_pads[i], 0, sizeof g_pads[i]); g_pads[i].dev = dev + 2; return i; }
    return NPAD - 1;
}
static int32_t strongest(int32_t a, int32_t b) { return abs(b) > abs(a) ? b : a; }
static void pads_merge(void) {
    g_pad_btn = 0; g_pad_lx = g_pad_ly = g_pad_rx = g_pad_ry = 0;
    for (int i = 0; i < NPAD; i++) if (g_pads[i].dev) { g_pad_btn |= g_pads[i].btn | g_pads[i].hat;
        g_pad_lx = strongest(g_pad_lx, g_pads[i].lx); g_pad_ly = strongest(g_pad_ly, g_pads[i].ly); g_pad_rx = strongest(g_pad_rx, g_pads[i].rx); g_pad_ry = strongest(g_pad_ry, g_pads[i].ry); }
    dm_pad_set(g_pad_btn, g_pad_lx, g_pad_ly, g_pad_rx, g_pad_ry);
}
// Controller buttons: an Android key code → Switch buttons table, made once. DM_PAD_LAYOUT=position (default) maps the face buttons by place, the bottom button of an
// Xbox-style pad (AKEYCODE_BUTTON_A) being the Switch's B as on a Switch pad; labels maps them by letter (A = the button marked A). DM_PAD_MAPPING=<button>:<key code>,...
// (the settings screen's remapping, PadMapping.java) then moves single buttons: A B X Y L R ZL ZR LS RS PLUS MINUS UP DOWN LEFT RIGHT
enum { KC_MAX = 512 };
static uint64_t g_kc_map[KC_MAX];
// the in-game menu's changes to DM_PAD_LAYOUT / DM_PAD_MAPPING (nativeSetting), read here instead of the environment once set; the input thread rebuilds the table
static pthread_mutex_t g_padset_mu = PTHREAD_MUTEX_INITIALIZER; static char g_padset_layout[16], g_padset_map[512]; static int g_padset_live; static _Atomic int g_padset_dirty;
static void pad_map_init(void) {
    memset(g_kc_map, 0, sizeof g_kc_map);
    char lay_live[16], map_live[512]; int live;
    pthread_mutex_lock(&g_padset_mu); live = g_padset_live; strcpy(lay_live, g_padset_layout); strcpy(map_live, g_padset_map); pthread_mutex_unlock(&g_padset_mu);
    static const struct { const char *name; uint64_t bit; int32_t pos, lab; } B[] = {
        { "A", DM_BTN_A, AKEYCODE_BUTTON_B, AKEYCODE_BUTTON_A }, { "B", DM_BTN_B, AKEYCODE_BUTTON_A, AKEYCODE_BUTTON_B },
        { "X", DM_BTN_X, AKEYCODE_BUTTON_Y, AKEYCODE_BUTTON_X }, { "Y", DM_BTN_Y, AKEYCODE_BUTTON_X, AKEYCODE_BUTTON_Y },
        { "L", DM_BTN_L, AKEYCODE_BUTTON_L1, AKEYCODE_BUTTON_L1 }, { "R", DM_BTN_R, AKEYCODE_BUTTON_R1, AKEYCODE_BUTTON_R1 },
        { "ZL", DM_BTN_ZL, AKEYCODE_BUTTON_L2, AKEYCODE_BUTTON_L2 }, { "ZR", DM_BTN_ZR, AKEYCODE_BUTTON_R2, AKEYCODE_BUTTON_R2 },
        { "LS", DM_BTN_LS, AKEYCODE_BUTTON_THUMBL, AKEYCODE_BUTTON_THUMBL }, { "RS", DM_BTN_RS, AKEYCODE_BUTTON_THUMBR, AKEYCODE_BUTTON_THUMBR },
        { "PLUS", DM_BTN_PLUS, AKEYCODE_BUTTON_START, AKEYCODE_BUTTON_START }, { "MINUS", DM_BTN_MINUS, AKEYCODE_BUTTON_SELECT, AKEYCODE_BUTTON_SELECT },
        { "UP", DM_BTN_UP, AKEYCODE_DPAD_UP, AKEYCODE_DPAD_UP }, { "DOWN", DM_BTN_DOWN, AKEYCODE_DPAD_DOWN, AKEYCODE_DPAD_DOWN },
        { "LEFT", DM_BTN_LEFT, AKEYCODE_DPAD_LEFT, AKEYCODE_DPAD_LEFT }, { "RIGHT", DM_BTN_RIGHT, AKEYCODE_DPAD_RIGHT, AKEYCODE_DPAD_RIGHT },
    };
    const char *lay = live ? lay_live : getenv("DM_PAD_LAYOUT"); int labels = lay && !strcmp(lay, "labels");
    for (unsigned i = 0; i < sizeof B / sizeof *B; i++) g_kc_map[labels ? B[i].lab : B[i].pos] |= B[i].bit;
    const char *m = live ? map_live : getenv("DM_PAD_MAPPING"); int moved = 0;
    while (m && *m) {
        char name[8]; int kc; int n = 0;
        if (sscanf(m, " %7[A-Za-z]:%d%n", name, &kc, &n) == 2 && kc > 0 && kc < KC_MAX)
            for (unsigned i = 0; i < sizeof B / sizeof *B; i++) if (!strcasecmp(name, B[i].name)) {
                for (int k = 0; k < KC_MAX; k++) g_kc_map[k] &= ~B[i].bit;   // the button leaves its old key
                g_kc_map[kc] |= B[i].bit; moved++; }
        const char *c = strchr(m, ','); if (!c) break; m = c + 1;
    }
    fprintf(stderr, "boot: controller buttons by %s%s\n", labels ? "label" : "position", moved ? ", some remapped (DM_PAD_MAPPING)" : "");
}
static uint64_t key_bit(int32_t kc) {
    static int init; if (!init || atomic_exchange(&g_padset_dirty, 0)) { init = 1; pad_map_init(); }   // (the input thread only)
    return kc > 0 && kc < KC_MAX ? g_kc_map[kc] : 0;
}
static int32_t axis(float v) { if (fabsf(v) < 0.12f) return 0; float s = (fabsf(v) - 0.12f) / 0.88f; if (s > 1) s = 1; return (int32_t)(copysignf(s, v) * DM_STICK_MAX); }
// Touch: the on-screen controls (touch_ui.c)
void touch_ui_init(ANativeActivity *a);
void touch_ui_event(const AInputEvent *e, float w, float h, uint64_t *btn, int32_t s[4]);
void touch_ui_hide(void);
void touch_ui_pad(int connected);
#include <jni.h>
JNIEXPORT void JNICALL Java_com_nxnative_GameActivity_nativePadConnected(JNIEnv *env, jclass c, jboolean on) { (void)env; (void)c; touch_ui_pad(on); }   // GameActivity.java
void touch_ui_setting(const char *key, const char *value);
void touch_ui_edit(void);
// GameMenu.java: a setting changed while playing (it also writes env.txt for the next start). The ones the game can take at once go to their owners here, never through
// setenv (other threads read the environment meanwhile); the rest wait for the next start. Returns whether it took effect now
static int g_bg_off = -1;   // DM_BACKGROUND=off: pause in the background (default: keep playing); the settings panel changes it at once
static int bg_play(void) { if (g_bg_off < 0) { const char *e = getenv("DM_BACKGROUND"); g_bg_off = e && !strcmp(e, "off"); } return !g_bg_off; }
extern int dm_fps_live __attribute__((weak));   // loader.c: the live frame-rate target (30 / 60), 0 when the start has none (another game, a fixed 90 / 120, DM_FPS_LIVE=0)
static void window_rate(ANativeWindow *w);
JNIEXPORT jboolean JNICALL Java_com_nxnative_GameMenu_nativeSetting(JNIEnv *env, jclass c, jstring jk, jstring jv) {
    (void)c; const char *k = (*env)->GetStringUTFChars(env, jk, NULL), *v0 = (*env)->GetStringUTFChars(env, jv, NULL); const char *v = *v0 ? v0 : NULL; int now = 1;
    if (!strcmp(k, "DM_BACKGROUND")) g_bg_off = v && !strcmp(v, "off");
    else if (!strcmp(k, "DM_FPS_TARGET")) {   // 30 / 60 at once with the live frame rate (the hold limit, the window's rate, the display pacing); otherwise the next start
        int t = v ? atoi(v) : 0; if (!v) { const char *a = getenv("DM_AUTO_FPS"); t = a && atoi(a) >= 60 ? 60 : 30; }   // (Auto: the phone profile's)
        if (&dm_fps_live && __atomic_load_n(&dm_fps_live, __ATOMIC_RELAXED) && (t == 30 || t == 60)) {
            __atomic_store_n(&dm_fps_live, t, __ATOMIC_RELAXED); window_rate(atomic_load(&g_window));
            fprintf(stderr, "frame rate: %d fps, switched while playing\n", t);
        } else now = 0;
    }
    else if (!strcmp(k, "DM_TOUCH_OPACITY") || !strcmp(k, "DM_TOUCH_FADE") || !strcmp(k, "DM_TOUCH_HAPTIC") || !strcmp(k, "DM_TOUCH_L3R3")) touch_ui_setting(k, v);
    else if (!strcmp(k, "DM_RUMBLE")) { touch_ui_setting(k, v); now = 0; }   // (the touch click follows it at once; the game's motors are GameActivity's, GameMenu decides)
    else if (!strcmp(k, "DM_ASPECT")) __atomic_store_n(&dm_aspect_mode, !v ? DM_ASPECT_FIT : !strcmp(v, "20:9") ? DM_ASPECT_20_9 : !strcmp(v, "fill") ? DM_ASPECT_FILL : DM_ASPECT_FIT, __ATOMIC_RELAXED);
    else if (!strcmp(k, "DM_SCALER")) { extern volatile int dm_scaler_mode; if (!v || !*v) v = getenv("DM_AUTO_SCALER"); dm_scaler_mode = v && !strcmp(v, "sgsr") ? 1 : v && !strcmp(v, "smooth") ? 2 : 0; }   // (Auto: the app's pick)
    else if (!strcmp(k, "DM_FRAMEGEN")) { extern volatile int dm_framegen_mode; dm_framegen_mode = v && (!strcmp(v, "on") || !strcmp(v, "1")) ? 1 : v && !strcmp(v, "debug") ? 2 : v && !strcmp(v, "lsfg") ? 3 : 0;   // nvn_vk.c present_window_locked
        window_rate(atomic_load(&g_window)); }   // (Android is told the doubled rate, or the game's own again)
    else if (!strcmp(k, "DM_LSFG_MULT")) { extern volatile int dm_lsfg_mult; int m = v ? atoi(v) : 0; dm_lsfg_mult = m >= 2 && m <= 4 ? m : 0; }   // nvn_vk.c lsfg_mult: 0 auto
    else if (!strcmp(k, "DM_SCALER_SHARPNESS")) { extern volatile float dm_scaler_sharpness; dm_scaler_sharpness = !v || !strcmp(v, "sharp") ? 0.7f : !strcmp(v, "smooth") ? -1.0f : (float)atof(v); }   // nvn_vk.c scale_draw
    else if (!strcmp(k, "DM_PAD_LAYOUT") || !strcmp(k, "DM_PAD_MAPPING")) {
        pthread_mutex_lock(&g_padset_mu);
        if (!g_padset_live) { const char *l = getenv("DM_PAD_LAYOUT"), *m = getenv("DM_PAD_MAPPING"); snprintf(g_padset_layout, sizeof g_padset_layout, "%s", l ? l : ""); snprintf(g_padset_map, sizeof g_padset_map, "%s", m ? m : ""); g_padset_live = 1; }
        if (!strcmp(k, "DM_PAD_LAYOUT")) snprintf(g_padset_layout, sizeof g_padset_layout, "%s", v ? v : ""); else snprintf(g_padset_map, sizeof g_padset_map, "%s", v ? v : "");
        pthread_mutex_unlock(&g_padset_mu); atomic_store(&g_padset_dirty, 1);
    } else now = 0;
    fprintf(stderr, "menu: %s=%s (%s)\n", k, v ? v : "(default)", now ? "now" : "from the next start");
    (*env)->ReleaseStringUTFChars(env, jk, k); (*env)->ReleaseStringUTFChars(env, jv, v0);
    return now ? JNI_TRUE : JNI_FALSE;
}
JNIEXPORT void JNICALL Java_com_nxnative_GameMenu_nativeEditTouch(JNIEnv *env, jclass c) { (void)env; (void)c; touch_ui_edit(); }   // "Move the touch controls"
// Holding − on a controller for 3 s opens the controller buttons (front_open_menu(1)): a press starts a timer thread that checks the same press is still held
uint64_t dm_nvn_frame_count(void) __attribute__((weak));   // (nvn: frames presented)
static _Atomic int g_minus_seq, g_minus_held;
// − and + held together for 1 s: the whole menu (front_open_menu(0)), for controllers and handhelds with built-in controls, where the touch ☰ is hidden
static _Atomic int g_combo_seq, g_combo_held;
static void *combo_hold_main(void *p) {
    int seq = (int)(intptr_t)p; usleep(1000000);
    if (atomic_load(&g_combo_held) && atomic_load(&g_combo_seq) == seq) { atomic_fetch_add(&g_minus_seq, 1); front_open_menu(0); }   // (the − hold's own timer stops)
    return NULL;
}
JNIEXPORT jlong JNICALL Java_com_nxnative_GameActivity_nativeFrames(JNIEnv *env, jclass c) { (void)env; (void)c; return dm_nvn_frame_count ? (jlong)dm_nvn_frame_count() : -1; }   // the performance overlay's fps
static void *minus_hold_main(void *p) {
    int seq = (int)(intptr_t)p; usleep(3000000);
    if (atomic_load(&g_minus_held) && atomic_load(&g_minus_seq) == seq) front_open_menu(1);
    return NULL;
}
static uint64_t g_touch_btn; static int32_t g_touch_s[4];
static void touch_update(AInputEvent *e) {
    float w = (float)ANativeWindow_getWidth(g_app->window), h = (float)ANativeWindow_getHeight(g_app->window);
    touch_ui_event(e, w, h, &g_touch_btn, g_touch_s);
    dm_input_set(g_touch_btn, g_touch_s[0], g_touch_s[1], g_touch_s[2], g_touch_s[3]);
}
static int32_t on_input(struct android_app *app, AInputEvent *e) {
    (void)app; int32_t src = AInputEvent_getSource(e), type = AInputEvent_getType(e);
    if (type == AINPUT_EVENT_TYPE_KEY) {
        int32_t kc = AKeyEvent_getKeyCode(e); uint64_t b = key_bit(kc);
        if (kc == AKEYCODE_BACK && !b) { uint64_t m = AKeyEvent_getAction(e) == AKEY_EVENT_ACTION_DOWN ? DM_BTN_B : 0; dm_input_set(g_touch_btn | m, g_touch_s[0], g_touch_s[1], g_touch_s[2], g_touch_s[3]); return 1; }   // the system back = B (the game never closes on back)
        if (!b) return 0;
        int k = pad_slot(AInputEvent_getDeviceId(e));
        if (AKeyEvent_getAction(e) == AKEY_EVENT_ACTION_DOWN) g_pads[k].btn |= b; else if (AKeyEvent_getAction(e) == AKEY_EVENT_ACTION_UP) g_pads[k].btn &= ~b;
        if (b & DM_BTN_MINUS) {   // − held 3 s: the controller buttons (minus_hold_main)
            if (AKeyEvent_getAction(e) == AKEY_EVENT_ACTION_DOWN && AKeyEvent_getRepeatCount(e) == 0) { int seq = atomic_fetch_add(&g_minus_seq, 1) + 1; atomic_store(&g_minus_held, 1);
                pthread_t th; if (!pthread_create(&th, NULL, minus_hold_main, (void *)(intptr_t)seq)) pthread_detach(th); }
            else if (AKeyEvent_getAction(e) == AKEY_EVENT_ACTION_UP) atomic_store(&g_minus_held, 0);
        }
        if (b & (DM_BTN_MINUS | DM_BTN_PLUS)) {   // − and + together (combo_hold_main)
            int both = (g_pads[k].btn & (DM_BTN_MINUS | DM_BTN_PLUS)) == (DM_BTN_MINUS | DM_BTN_PLUS);
            if (both && !atomic_load(&g_combo_held)) { int seq = atomic_fetch_add(&g_combo_seq, 1) + 1; atomic_store(&g_combo_held, 1);
                pthread_t th; if (!pthread_create(&th, NULL, combo_hold_main, (void *)(intptr_t)seq)) pthread_detach(th); }
            else if (!both) atomic_store(&g_combo_held, 0);
        }
        touch_ui_hide(); pads_merge(); return 1;
    }
    if (type == AINPUT_EVENT_TYPE_MOTION && (src & (AINPUT_SOURCE_JOYSTICK | AINPUT_SOURCE_GAMEPAD))) {
        int k = pad_slot(AInputEvent_getDeviceId(e));
        float rx = AMotionEvent_getAxisValue(e, AMOTION_EVENT_AXIS_Z, 0), ry = AMotionEvent_getAxisValue(e, AMOTION_EVENT_AXIS_RZ, 0);
        if (rx == 0 && ry == 0) { rx = AMotionEvent_getAxisValue(e, AMOTION_EVENT_AXIS_RX, 0); ry = AMotionEvent_getAxisValue(e, AMOTION_EVENT_AXIS_RY, 0); }   // right stick as RX / RY on some drivers
        g_pads[k].lx = axis(AMotionEvent_getAxisValue(e, AMOTION_EVENT_AXIS_X, 0)); g_pads[k].ly = axis(-AMotionEvent_getAxisValue(e, AMOTION_EVENT_AXIS_Y, 0));
        g_pads[k].rx = axis(rx); g_pads[k].ry = axis(-ry);
        float hx = AMotionEvent_getAxisValue(e, AMOTION_EVENT_AXIS_HAT_X, 0), hy = AMotionEvent_getAxisValue(e, AMOTION_EVENT_AXIS_HAT_Y, 0);
        float lt = AMotionEvent_getAxisValue(e, AMOTION_EVENT_AXIS_LTRIGGER, 0), rt = AMotionEvent_getAxisValue(e, AMOTION_EVENT_AXIS_RTRIGGER, 0);
        if (lt == 0) lt = AMotionEvent_getAxisValue(e, AMOTION_EVENT_AXIS_BRAKE, 0); if (rt == 0) rt = AMotionEvent_getAxisValue(e, AMOTION_EVENT_AXIS_GAS, 0);
        touch_ui_hide();
        g_pads[k].hat = (hx < -0.5f ? DM_BTN_LEFT : 0) | (hx > 0.5f ? DM_BTN_RIGHT : 0) | (hy < -0.5f ? DM_BTN_UP : 0) | (hy > 0.5f ? DM_BTN_DOWN : 0) | (lt > 0.5f ? DM_BTN_ZL : 0) | (rt > 0.5f ? DM_BTN_ZR : 0);
        pads_merge(); return 1;
    }
    if (type == AINPUT_EVENT_TYPE_MOTION && (src & AINPUT_SOURCE_TOUCHSCREEN)) { touch_update(e); return 1; }
    return 0;
}

// ---------- Host start ----------
static int g_started;
// ---------- Crash-aware starts: <files>/boot_state says how far a run got (start → device: the graphics device is up → gone after 30 s of play), and "crashed" when
// the crash handler ran (crash.c dm_crash_note). A crashed run's stage decides what the next start does:
//   · crashed before 30 s of play on the phone's own driver, with the device not up or no frames yet: the app offers a driver package for this GPU (notice.txt,
//     SetupActivity reads it and does not start the game straight away); not when the pc was in the game's own code (main / sdk: a driver would not help)
//   · crashed with mods on: the mods are switched off (each folder renamed with a leading "-", loose .pchtxt files to .pchtxt.off), and the player is told
// A run Android ends (swiped away, low memory) writes no "crashed": nothing is changed then
static char g_boot_path[1100]; static int g_boot_fd = -1;
static _Atomic int g_dev_noted;   // "device" written to boot_state (nvn_vk.c right after the device is made, else boot_guard_main within a second)
static int g_safe_advanced;       // this start is one safe_start level up (no driver dialog then: the safer settings are tried first)
void dm_boot_device_up(void) { if (!atomic_exchange(&g_dev_noted, 1) && g_boot_fd >= 0) { static const char m[] = "device\n"; (void)!write(g_boot_fd, m, sizeof m - 1); } }
static char g_crash_src[1200], g_crash_dl[256];   // log.txt and its copy in Download/ (host_start), for dm_crash_log_copy
void dm_crash_log_copy(void) {   // from the crash handler: open / read / write only
    if (!g_crash_src[0] || !g_crash_dl[0]) return;
    int in = open(g_crash_src, O_RDONLY), out = in >= 0 ? open(g_crash_dl, O_WRONLY | O_CREAT | O_TRUNC, 0644) : -1; char b[16384]; ssize_t n;
    if (in >= 0 && out >= 0) while ((n = read(in, b, sizeof b)) > 0) if (write(out, b, (size_t)n) != n) break;
    if (in >= 0) close(in); if (out >= 0) close(out);
}
void dm_crash_note(int in_game) { if (g_boot_fd >= 0) { static const char m[] = "crashed\n", g[] = "crashed in the game's code\n"; if (in_game) (void)!write(g_boot_fd, g, sizeof g - 1); else (void)!write(g_boot_fd, m, sizeof m - 1); } }
const char *nvn_vk_gpu_name(void) __attribute__((weak));
static void notice(const char *kind, const char *text) {
    char p[1200]; snprintf(p, sizeof p, "%s/notice.txt", g_files); FILE *f = fopen(p, "a"); if (f) { fprintf(f, "%s\t%s\n", kind, text); fclose(f); }
    fprintf(stderr, "boot: notice %s: %s\n", kind, text);
}
static int mods_off(const char *mods) {   // every mod switched off; returns how many
    struct dirent **ents = NULL; int n = scandir(mods, &ents, NULL, alphasort), off = 0; if (n < 0) return 0;
    for (int i = 0; i < n; i++) { const char *nm = ents[i]->d_name; char a[1200], b[1200]; struct stat st; size_t l = strlen(nm);
        snprintf(a, sizeof a, "%s/%s", mods, nm);
        if (nm[0] != '.' && nm[0] != '-' && !stat(a, &st) && S_ISDIR(st.st_mode)) { snprintf(b, sizeof b, "%s/-%s", mods, nm); if (!rename(a, b)) off++; }
        else if (l > 7 && !strcasecmp(nm + l - 7, ".pchtxt")) { snprintf(b, sizeof b, "%s.off", a); if (!rename(a, b)) off++; }
        free(ents[i]); }
    free(ents); return off;
}
static int mods_on(const char *mods) {   // how many mods are on (what mods_romfs and the loader will use)
    struct dirent **ents = NULL; int n = scandir(mods, &ents, NULL, alphasort), on = 0; if (n < 0) return 0;
    for (int i = 0; i < n; i++) { const char *nm = ents[i]->d_name; char a[1200]; struct stat st; size_t l = strlen(nm); snprintf(a, sizeof a, "%s/%s", mods, nm);
        if ((nm[0] != '.' && nm[0] != '-' && !stat(a, &st) && S_ISDIR(st.st_mode)) || (l > 7 && !strcasecmp(nm + l - 7, ".pchtxt"))) on++; free(ents[i]); }
    free(ents); return on;
}
static void boot_guard(const char *mods) {
    snprintf(g_boot_path, sizeof g_boot_path, "%s/boot_state", g_files);
    char prev[512] = ""; FILE *f = fopen(g_boot_path, "r"); if (f) { size_t k = fread(prev, 1, sizeof prev - 1, f); prev[k] = 0; fclose(f); }
    if (prev[0] && strstr(prev, "crashed")) {
        int had_mods = strstr(prev, "mods 0") == NULL && strstr(prev, "mods ") != NULL, device = strstr(prev, "device") != NULL;
        const char *drv = getenv("DM_VK_DRIVER"); int stock = !drv || !*drv;
        fprintf(stderr, "boot: the last run crashed (%s)\n", device ? "after the graphics device came up" : "before the graphics device came up");
        if (device) setenv("DM_LAST_CRASH_DEVICE", "1", 1);   // (nvn_vk.c: a start that went without the dma-buf heap route and crashed anyway sends the next one back to it)
        if (had_mods) { int n = mods_off(mods); if (n) { char t[200]; snprintf(t, sizeof t, "The game closed while %d mod%s were on, so they are switched off. Switch them back on in the menu, Mods.", n, n == 1 ? "" : "s"); notice("mods", t); } }
        else if (stock && !strstr(prev, "playing") && !strstr(prev, "in the game's code") && !g_safe_advanced) {   // (a crash in the game's own code is the engine's to fix, not the driver's: no driver dialog; nor while safe_start has a safer level to try)
            char gpu[256] = ""; char gp[1200]; snprintf(gp, sizeof gp, "%s/gpu.txt", g_files); FILE *g = fopen(gp, "r"); if (g) { if (!fgets(gpu, sizeof gpu, g)) gpu[0] = 0; fclose(g); }
            char *nl = strchr(gpu, '\n'); if (nl) *nl = 0;
            notice("driver", *gpu ? gpu : "unknown");
        }
    }
    { char n[16]; snprintf(n, sizeof n, "%d", mods_on(mods)); setenv("DM_MODS_ACTIVE", n, 1); }   // (hle_ldn.c: local play is off while mods are on)
    g_boot_fd = open(g_boot_path, O_WRONLY | O_CREAT | O_TRUNC, 0660);
    if (g_boot_fd >= 0) { char t[160]; int k = snprintf(t, sizeof t, "start driver %s mods %d\n", getenv("DM_VK_DRIVER") && *getenv("DM_VK_DRIVER") ? "custom" : "stock", mods_on(mods)); (void)!write(g_boot_fd, t, k); }
}
static void *boot_guard_main(void *p) {
    (void)p; int dev = 0; time_t t0 = time(NULL);
    for (;;) { sleep(1);
        const char *gpu = nvn_vk_gpu_name ? nvn_vk_gpu_name() : NULL;
        if (gpu && !dev) { dev = 1; dm_boot_device_up();
            char gp[1200]; snprintf(gp, sizeof gp, "%s/gpu.txt", g_files); FILE *g = fopen(gp, "w"); if (g) { fprintf(g, "%s\n", gpu); fclose(g); } }
        uint64_t fr = dm_nvn_frame_count ? dm_nvn_frame_count() : 0;
        if (dev && fr >= 300 && time(NULL) - t0 >= 30) {   // played: the run is known good
            if (g_boot_fd >= 0) { static const char m[] = "playing\n"; (void)!write(g_boot_fd, m, sizeof m - 1); }
            unlink(g_boot_path); fprintf(stderr, "boot: 30 s of play: this start is good\n"); return NULL; }
    }
}
// The game's frame rate told to Android (30, or DM_FPS_TARGET with the 60 fps patch), as a fixed source: a 90 / 120 Hz phone then runs the display at a rate the
// frames divide evenly (60 fps on a 90 Hz screen would otherwise show frames for 1 and 2 refreshes in turn, an uneven judder). Presentation never tears (FIFO /
// MAILBOX swapchains); this keeps the frames evenly spaced
static void window_rate(ANativeWindow *w) {
    if (!w) return;
    const char *e = getenv("DM_FPS_TARGET"); int fps = e ? atoi(e) : 0; float rate = fps >= 60 && fps <= 120 ? (float)fps : 30.0f;
    { int lf = &dm_fps_live ? __atomic_load_n(&dm_fps_live, __ATOMIC_RELAXED) : 0; if (lf) rate = (float)lf; }   // (the live target, once the game is loaded)
    // frame generation (DM_FRAMEGEN, nvn_vk.c) shows two pictures a game frame: told as 30, Android showed this app only every other one (measured on a Galaxy S25+:
    //   300 of 600 pictures in 10 s, 33.4 ms apart)
    { extern volatile int dm_framegen_mode; int fg = dm_framegen_mode; const char *f = getenv("DM_FRAMEGEN");
      if (fg < 0) fg = f && (!strcmp(f, "on") || !strcmp(f, "1") || !strcmp(f, "debug") || !strcmp(f, "lsfg"));
      if (fg) rate = 120.0f; }   // (not the doubled rate itself: at 60 Hz every picture must make its own refresh with no slack, and a tenth of them missed it; at 120 each has a timed slot)
    // libnativewindow: setFrameRateWithChangeStrategy (Android 12+, ALWAYS: the display switches modes even when the switch is not seamless, so a 144 Hz screen goes
    // to 120 for 30 / 60 fps), else setFrameRate (Android 11); looked up, so a system without them only skips this
    typedef int32_t (*set_rate_fn)(ANativeWindow *, float, int8_t); typedef int32_t (*set_rate2_fn)(ANativeWindow *, float, int8_t, int8_t);
    void *h = dlopen("libnativewindow.so", RTLD_NOW);
    set_rate2_fn r2 = h ? (set_rate2_fn)dlsym(h, "ANativeWindow_setFrameRateWithChangeStrategy") : NULL; set_rate_fn r1 = h ? (set_rate_fn)dlsym(h, "ANativeWindow_setFrameRate") : NULL;
    int r = r2 ? r2(w, rate, ANATIVEWINDOW_FRAME_RATE_COMPATIBILITY_FIXED_SOURCE, 1 /* ANATIVEWINDOW_CHANGE_FRAME_RATE_ALWAYS */) : r1 ? r1(w, rate, ANATIVEWINDOW_FRAME_RATE_COMPATIBILITY_FIXED_SOURCE) : -1;
    fprintf(stderr, "boot: window frame rate %.0f fps (fixed source)%s\n", rate, r ? " refused by the system" : "");
}
// RomFS mods: every <mods>/<mod>/romfs folder (and <mods>/romfs) as a RomFS overlay root (hle_fs.c rom_overlay_path: the first root that has a file wins), in the load
// order of <mods>/order.txt (the in-game menu's: one name a line, first wins), then by name; a mod's exefs/*.pchtxt code patches are found by the loader (loader.c
// pchtxt_dir). A mod folder named with a leading "-" or "." is switched off
static char g_roots[8192]; static size_t g_roots_len; static int g_roots_n;
// HD texture packs (DM_HD_TEXTURES: "" Auto = the phone profile's DM_AUTO_HD, "on", "off"; the menu's Display → HD textures): a mod whose RomFS is mostly textures,
//   100 MB or more with at least 3/4 of its bytes under a path naming "texture" (retexture mods such as the HD mod for Sword and Shield), is left out on a phone that
//   should not carry it, as if switched off: more texture memory and slower area loads would cost a weaker phone its 30 fps
static int has_texture_word(const char *s) { for (; *s; s++) if (!strncasecmp(s, "texture", 7)) return 1; return 0; }
static void tex_walk(const char *dir, int intex, uint64_t *all, uint64_t *tex, int depth) {
    DIR *d = depth > 16 ? NULL : opendir(dir); if (!d) return;
    struct dirent *e; char p[1200]; struct stat st;
    while ((e = readdir(d))) {
        if (e->d_name[0] == '.' || snprintf(p, sizeof p, "%s/%s", dir, e->d_name) >= (int)sizeof p || stat(p, &st)) continue;
        int t = intex || has_texture_word(e->d_name);
        if (S_ISDIR(st.st_mode)) tex_walk(p, t, all, tex, depth + 1); else { *all += (uint64_t)st.st_size; if (t) *tex += (uint64_t)st.st_size; }
    }
    closedir(d);
}
static int texture_pack(const char *romfs) { uint64_t all = 0, tex = 0; tex_walk(romfs, 0, &all, &tex, 0); return all >= (100ull << 20) && tex * 4 >= all * 3; }
static int hd_textures_on(const char **why) {
    const char *e = getenv("DM_HD_TEXTURES"); *why = "HD textures in the menu";
    if (!e || !*e) { e = getenv("DM_AUTO_HD"); *why = "HD textures on Auto: this phone's memory and GPU"; }
    return !e || strcmp(e, "off");   // (no profile: an older app, as before: on)
}
static void mod_root(const char *mods, const char *nm) {
    char d[1200]; struct stat st; static char skipped[2048];
    if (nm[0] == '.' || nm[0] == '-' || !strcmp(nm, "romfs") || strchr(nm, '/')) return;
    snprintf(d, sizeof d, "%s/%s/romfs", mods, nm); size_t dl = strlen(d);
    if (stat(d, &st) || !S_ISDIR(st.st_mode) || strchr(d, ':') || g_roots_len + dl + 2 >= sizeof g_roots) return;
    for (const char *q = g_roots; (q = strstr(q, d)); q += dl) if ((q == g_roots || q[-1] == ':') && (q[dl] == ':' || !q[dl])) return;   // (listed already)
    { char key[300]; snprintf(key, sizeof key, "/%s/", nm); if (strstr(skipped, key)) return;   // (left out already: order.txt, then the folder scan)
      const char *why; if (!hd_textures_on(&why) && texture_pack(d)) {
          if (strlen(skipped) + strlen(key) < sizeof skipped) strcat(skipped, key);
          fprintf(stderr, "boot: mod %s: an HD texture pack, left out (%s)\n", nm, why); return; } }
    g_roots_len += snprintf(g_roots + g_roots_len, sizeof g_roots - g_roots_len, "%s%s", g_roots_len ? ":" : "", d); g_roots_n++;
    fprintf(stderr, "boot: mod %s: RomFS files (load order %d)\n", nm, g_roots_n);
}
static void mods_romfs(const char *mods) {
    if (getenv("DM_ROMFS_OVERLAY")) return;   // env.txt chose
    struct stat st; char d[1200];
    // Trinity games: the pack index with the mods' paths taken out (setup_game.py trinity_index), first, so it is the arc/data.trpfd the game reads
    snprintf(d, sizeof d, "%s/.trinity_index/romfs", mods); if (!stat(d, &st) && S_ISDIR(st.st_mode)) { g_roots_len += snprintf(g_roots, sizeof g_roots, "%s", d); g_roots_n++; }
    snprintf(d, sizeof d, "%s/romfs", mods); if (!stat(d, &st) && S_ISDIR(st.st_mode)) { g_roots_len += snprintf(g_roots + g_roots_len, sizeof g_roots - g_roots_len, "%s%s", g_roots_len ? ":" : "", d); g_roots_n++; }
    snprintf(d, sizeof d, "%s/order.txt", mods); FILE *f = fopen(d, "r");
    if (f) { char ln[512]; while (fgets(ln, sizeof ln, f)) { ln[strcspn(ln, "\r\n")] = 0; if (*ln) mod_root(mods, ln); } fclose(f); }
    struct dirent **ents = NULL; int n = scandir(mods, &ents, NULL, alphasort);
    for (int k = 0; k < n; k++) { mod_root(mods, ents[k]->d_name); free(ents[k]); }
    free(ents);
    if (g_roots_n) { setenv("DM_ROMFS_OVERLAY", g_roots, 1); fprintf(stderr, "boot: %d RomFS mod folder(s)\n", g_roots_n); }
}
static void env_file(const char *path) {   // KEY=VALUE lines, # comments; set only when not already set
    FILE *f = fopen(path, "r"); char ln[512]; if (!f) return;
    while (fgets(ln, sizeof ln, f)) { char *p = ln; while (*p == ' ' || *p == '\t') p++; if (*p == '#' || !*p) continue; char *eq = strchr(p, '='); if (!eq || eq == p) continue;
        *eq = 0; char *v = eq + 1; char *e = strpbrk(v, "\r\n"); if (e) *e = 0; if (!getenv(p)) { setenv(p, v, 0); fprintf(stderr, "boot: env %s=%s\n", p, v); } }
    fclose(f);
}
// Device profiles: defaults by chip, from its model (ro.soc.manufacturer / ro.soc.model, Android 12+: SM8750, MT6989, s5e9945 ...). Set without overwriting, after env.txt,
// so the player's own settings (the settings screen writes env.txt) always win; DM_PROFILE=<name> below picks one by hand, DM_PROFILE=none none.
// Mali GPUs (MediaTek, Tensor, Exynos before the 2000 series) have no BC texture support: the game's textures are decoded on upload to 4 to 8 times their size, and the
// game's memory is mirrored for the GPU (nvn_vk.c memory pools), so their profiles keep textures at half size sooner than Adreno's
//   sd8   Snapdragon 8 series (SM8xxx)                                    nothing changed: the defaults these phones were tuned on (TV mode, 8x filtering, full textures)
//   sd8old older Snapdragon 8 series (SM8150-SM8475: 855, 865, 870, 888, 8 Gen 1, 8+ Gen 1)  TV mode, handheld mode while hot; 4x filtering; full textures
//   sd7   Snapdragon 7 series from 2021 (SM73xx and later)                TV mode, handheld mode while hot; 4x filtering; full textures
//   sd6   Snapdragon 6 and 4 series, 7 series before 2021 (SM4xxx-SM72xx) handheld mode (720p); 2x filtering; half textures
//   high  Dimensity 9000 series, Exynos 2000 series (Xclipse)             TV mode, handheld mode while hot; 4x filtering; full textures
//   mid   Dimensity 8000 series and 1000-1200, Google Tensor              handheld mode; 4x filtering; half textures
//   low   other MediaTek (Dimensity 7000 / 6000, Helio), Exynos 1000 series and older Exynos   handheld mode; 2x filtering; half textures
// Other chips get no profile (as before)
static const struct prof { const char *name, *what, *graphics, *aniso, *tex; } g_profiles[] = {
    { "sd8", "Snapdragon 8 series", NULL, NULL, NULL }, { "sd8old", "older Snapdragon 8 series (855 to 8+ Gen 1)", "auto", "4", "full" }, { "sd7", "Snapdragon 7 series", "auto", "4", "full" }, { "sd6", "Snapdragon 6 / 4 series or older 7 series", "low", "2", "half" },
    { "high", "Dimensity 9000 / Exynos 2000 series", "auto", "4", "full" }, { "mid", "Dimensity 8000 series / Google Tensor", "low", "4", "half" },
    { "low", "MediaTek Dimensity 7000 and below / Exynos 1000 series", "low", "2", "half" },
};
static const struct prof *profile_of(const char *maker, const char *model) {
    char m[PROP_VALUE_MAX]; size_t k = 0; for (; model[k] && k + 1 < sizeof m; k++) m[k] = (char)tolower((unsigned char)model[k]); m[k] = 0;
    const char *name = NULL; int num;
    if ((!strncmp(m, "sm", 2) && isdigit((unsigned char)m[2])) || (!strncmp(m, "sdm", 3) && isdigit((unsigned char)m[3]))) {   // Qualcomm
        num = atoi(m + (m[1] == 'd' ? 3 : 2)); name = num >= 8000 && num < 8500 ? "sd8old" : num >= 8500 && num < 9000 ? "sd8" : num >= 7300 && num < 8000 ? "sd7" : num >= 4000 && num < 7300 ? "sd6" : num > 0 && num < 1000 ? "sd6" : NULL;   // (SDM845 and the like: before the SMxxxx names)
    } else if (!strncmp(m, "mt", 2) && isdigit((unsigned char)m[2])) {   // MediaTek: MT6983 / 6985 / 6989 / 6991 Dimensity 9000-9400; MT6889-6899 Dimensity 1000-1200 and 8000-8400
        num = atoi(m + 2); name = num == 6983 || num == 6985 || num == 6989 || num == 6991 || num == 6993 ? "high" : num >= 6889 && num <= 6899 ? "mid" : "low";
    } else if (!strncmp(m, "s5e", 3) || strstr(m, "exynos")) {   // Exynos: s5e99xx and "Exynos 2xxx" are the 2000 series (Xclipse); s5e88xx, Exynos 1xxx and 9xx Mali
        const char *d = m; while (*d && !isdigit((unsigned char)*d)) d++; if (!strncmp(m, "s5e", 3)) d = m + 3;
        num = atoi(d); name = (!strncmp(m, "s5e", 3) && num >= 9900 && num < 10000) || (strncmp(m, "s5e", 3) && num >= 2000 && num < 3000) ? "high" : "low";
    } else if (strstr(m, "tensor") || (!strncasecmp(maker, "google", 6) && *m)) name = "mid";
    for (unsigned i = 0; name && i < sizeof g_profiles / sizeof *g_profiles; i++) if (!strcmp(g_profiles[i].name, name)) return &g_profiles[i];
    return NULL;
}
// Mesa PanVK (FristOneRR, chosen as the driver: a libvulkan_panfrost package) on the chips its compatibility table marks as needing PANVK_ATOM_STRIDE=56 to start
// (Exynos 2100 = s5e9840, Exynos 1380 = s5e8835): set for them, unless env.txt (the setup screen's checkbox) already says
static void panvk_defaults(const char *model) {
    const char *drv = getenv("DM_VK_DRIVER"); if (!drv || !strstr(drv, "panfrost")) return;
    char m[PROP_VALUE_MAX]; size_t k = 0; for (; model[k] && k + 1 < sizeof m; k++) m[k] = (char)tolower((unsigned char)model[k]); m[k] = 0;
    if ((strstr(m, "s5e9840") || strstr(m, "s5e8835") || strstr(m, "exynos2100") || strstr(m, "exynos1380")) && !getenv("PANVK_ATOM_STRIDE")) {
        setenv("PANVK_ATOM_STRIDE", "56", 0); fprintf(stderr, "boot: PanVK on %s: PANVK_ATOM_STRIDE=56 (the driver's own table: needed to start on this chip)\n", model); }
}
static void device_profile(void) {
    char maker[PROP_VALUE_MAX] = "", model[PROP_VALUE_MAX] = ""; __system_property_get("ro.soc.manufacturer", maker); __system_property_get("ro.soc.model", model);
    panvk_defaults(model);
    const char *force = getenv("DM_PROFILE"); const struct prof *p = NULL;
    // The app measured the phone (GameSettings.java profile: CPU, GPU and memory tiers, the display's modes) and handed the result over as DM_AUTO_*: those are the
    // defaults, set without overwriting, so the player's own choices (env.txt, read before this) win. The table below stays for a start without them (an older app)
    const char *ag = getenv("DM_AUTO_GRAPHICS");
    if (ag && !force) {
        const char *at = getenv("DM_AUTO_TEX"), *aa = getenv("DM_AUTO_ANISO"), *af = getenv("DM_AUTO_FPS");
        setenv("DM_GRAPHICS", ag, 0); if (aa) setenv("DM_VK_MAX_ANISO", aa, 0);
        if (at && !strcmp(at, "half") && !getenv("DM_VK_TEX_QUALITY")) { setenv("DM_VK_TEX_QUALITY", "half", 1); setenv("DM_VK_TEX_QUALITY_AUTO", "1", 1); }   // (the profile's half: nvn_vk.c may keep full on a driver that crashed with it)
        if (af && atoi(af) >= 60) setenv("DM_FPS_TARGET", af, 0);   // (60 only when the app found a top phone and a display that 60 divides)
        { const char *as = getenv("DM_AUTO_SCALER"); if (as && *as) setenv("DM_SCALER", as, 0); }   // (SGSR on a mid or top GPU, the lighter Sharp on a low one)
        const char *g = getenv("DM_GRAPHICS"), *tq = getenv("DM_VK_TEX_QUALITY"), *an = getenv("DM_VK_MAX_ANISO"), *ft = getenv("DM_FPS_TARGET"), *w = getenv("DM_AUTO_WHAT"), *hz = getenv("DM_DISPLAY_HZ");
        fprintf(stderr, "boot: this phone: %s (chip %s %s; display modes %s Hz): graphics %s, anisotropic filtering %sx, textures %s, frame rate %s\n", w ? w : "?", *maker ? maker : "?",
                *model ? model : "unknown", hz ? hz : "?", g ? g : "default", an ? an : "default", tq ? tq : "full", ft ? ft : "30");
        return;
    }
    if (force) { for (unsigned i = 0; i < sizeof g_profiles / sizeof *g_profiles; i++) if (!strcmp(g_profiles[i].name, force)) p = &g_profiles[i]; }
    else p = profile_of(maker, model);
    // Then the phone's memory, for every phone (GameSettings.java auto(), the same rule): the game's textures are the largest use of memory and a phone that runs out
    // ends the game. Under 8 GB: half textures whatever the chip; under 6 GB also handheld mode. 12 GB or more: full textures unless the chip is a weak one (sd6, low)
    long pages = sysconf(_SC_PHYS_PAGES), psz = sysconf(_SC_PAGESIZE); double gb = pages > 0 && psz > 0 ? (double)pages * psz / 1e9 : 0;
    const char *graphics = p ? p->graphics : NULL, *tex = p ? p->tex : NULL, *aniso = p ? p->aniso : NULL;   // NULL: the defaults (TV mode, 8x, full)
    int weak = p && (!strcmp(p->name, "sd6") || !strcmp(p->name, "low"));
    if (gb > 0 && gb < 7.5) tex = "half";
    else if (gb >= 11.5 && !weak) tex = NULL;
    if (gb > 0 && gb < 5.5) graphics = "low";
    if (graphics) setenv("DM_GRAPHICS", graphics, 0);
    if (aniso) setenv("DM_VK_MAX_ANISO", aniso, 0);
    if (tex && !getenv("DM_VK_TEX_QUALITY")) { setenv("DM_VK_TEX_QUALITY", tex, 1); if (!strcmp(tex, "half")) setenv("DM_VK_TEX_QUALITY_AUTO", "1", 1); }
    const char *g = getenv("DM_GRAPHICS"), *tq = getenv("DM_VK_TEX_QUALITY"), *an = getenv("DM_VK_MAX_ANISO");
    fprintf(stderr, "boot: device profile %s, %s (chip %s %s%s, %.1f GB memory): graphics %s, anisotropic filtering %sx, textures %s\n", p ? p->name : "none", p ? p->what : "chip not in the table",
            *maker ? maker : "?", *model ? model : "unknown", force ? ", DM_PROFILE" : "", gb, g ? g : "default", an ? an : "default", tq ? tq : "full");
}
static pid_t g_game_tid;   // the game's main thread (performance hints below)
static void *host_thread(void *a) {
    g_game_tid = gettid();
    (void)a; static char alt[65536]; stack_t ss = {.ss_sp = alt, .ss_size = sizeof alt}; sigaltstack(&ss, 0);
    char patched[1100]; snprintf(patched, sizeof patched, "%s/patched", g_game);
    char *argv[3] = { "dmhost", patched, NULL };
    int rc = dm_main(2, argv);
    fprintf(stderr, "boot: dm_main returned %d\n", rc);
    return NULL;
}
// The log where the player can reach it: Android/data is out of reach of the phone's Files app on recent Android versions, Download is not. At each start the previous
// run's log (the one that ended in a crash, if one did) goes to Download/<game>-log-previous.txt (PokemonShield-…); while playing, Download/<game>-log.txt is refreshed every
// 5 seconds, with a memory line (a run the system ends for memory leaves no crash report, only memory that kept rising)
#define DL "/storage/emulated/0/Download"
static void copy_file(const char *a, const char *b) {
    int i = open(a, O_RDONLY), o = i < 0 ? -1 : open(b, O_WRONLY | O_CREAT | O_TRUNC, 0644); char buf[65536]; ssize_t n;
    if (o >= 0) while ((n = read(i, buf, sizeof buf)) > 0) if (write(o, buf, (size_t)n) != n) break;
    if (i >= 0) close(i); if (o >= 0) close(o);
}
static long kb_of(const char *path, const char *key) {
    FILE *f = fopen(path, "r"); char ln[256]; long v = -1; size_t kl = strlen(key);
    while (f && fgets(ln, sizeof ln, f)) if (!strncmp(ln, key, kl)) { v = atol(ln + kl); break; }
    if (f) fclose(f); return v;
}
// The phone's heat (Android 11+ thermal status, 12+ headroom: 1.0 = where throttling starts), looked up at run time, for the log line: a slowdown from heat shows as heat
static void *g_th_mgr; static int (*g_th_st)(void *); static float (*g_th_hr)(void *, int);
static const char *thermal_text(char *buf, size_t n) {
    static void *mgr; static int (*st)(void *); static float (*hr)(void *, int); static int tried;
    if (!tried++) { void *lib = dlopen("libandroid.so", RTLD_NOW); void *(*acq)(void) = lib ? (void *(*)(void))dlsym(lib, "AThermal_acquireManager") : NULL;
        st = lib ? (int (*)(void *))dlsym(lib, "AThermal_getCurrentThermalStatus") : NULL; hr = lib ? (float (*)(void *, int))dlsym(lib, "AThermal_getThermalHeadroom") : NULL; mgr = acq ? acq() : NULL; }
    g_th_mgr = mgr; g_th_st = st; g_th_hr = hr;
    if (!mgr || !st) return "unknown";
    static const char *names[] = { "none", "light", "moderate", "severe", "critical", "emergency", "shutdown" }; int v = st(mgr);
    snprintf(buf, n, "%s, headroom %.2f", v >= 0 && v <= 6 ? names[v] : "?", hr ? hr(mgr, 10) : -1.0f);   // headroom forecast 10 s ahead
    return buf;
}
// Graphics "auto": TV mode while the phone keeps cool; handheld mode (720p, lighter effects) once it is hot (thermal status severe or worse, or throttling forecast
// within 10 s), back to TV mode after a minute below "moderate" with headroom to spare. The game switches as when a Switch leaves or enters its dock
void dm_set_opmode(int m);   // host/hle_oe.c: tells the game the console mode changed
// Frame-rate guard (every frame-rate target, 30 included: the project's floor is a locked 30 on every phone; with Graphics on Auto or Adaptive; DM_FPS_GUARD=0 turns
//   it off): the frames presented each 5 s against the target. Two
//   checks in a row short of it (under 93 %) drop to handheld mode (720p, the game's lighter settings), which holds the rate where TV mode cannot; TV mode is tried
//   again after a calm, steady spell, a minute the first time and twice as long after each fall back (up to 16 minutes), so a scene that cannot hold it does not
//   flip between the two. Game speed stays right meanwhile (hle_nvn.c vstep_tick). Graphics on High, Low or Lowest chosen by hand is left alone
// Games whose render size is fixed at start (Pokémon Sword / Shield: a mode change while playing does not change it, hle_resscale.c) step the game's own 3D render
//   scale instead, one step at a time: 100, 85, 70, 60 % of what the game asks, in width and height (72, 49, 36 % of the pixels). Down after the same two checks short
//   of the target, or while hot at most every 30 s (heat follows slowly); up one step after the same calm, steady spell, with the same doubling wait. Every step down is
//   checked 10 s later, as a lower scale does not always help: on a strong GPU its governor lowers the clock with the load and frames can get longer (Galaxy S25+: 43
//   against 36 ms walking at 60 %, hle_resscale.c). A step for a short frame rate that gained under 1 fps (the slowdown is not the GPU's: the CPU, a clock cap while
//   charging or warm, the replay's own work), or a step for heat that cost over 1 fps, is taken back, and that reason steps nothing for 5 minutes
// A scene that keeps the game's full size (the menus showing Pokémon models: the game's own mode, hle_resscale.c) is drawn at full size whatever the step: no step is
//   taken or judged there. A step on trial when one opens is kept without a verdict, and the short-frame count starts again
int dm_res_scale_ready(void), dm_res_scale_applies(void); void dm_res_factor_set(float f);   // host/hle_resscale.c
static const float k_res_steps[] = { 1.0f, 0.85f, 0.7f, 0.6f };
#define NRES ((int)(sizeof k_res_steps / sizeof k_res_steps[0]))
static int g_fps_guard;   // set at start (fps_guard_init): the target, 0 off
static void fps_guard_init(void) {
    const char *t = getenv("DM_FPS_TARGET"), *off = getenv("DM_FPS_GUARD"), *g = getenv("DM_GRAPHICS");
    if (!t || !*t) t = getenv("DM_AUTO_FPS");   // (Frame rate on Auto: what the app chose for this phone)
    int fps = t ? atoi(t) : 0; if (fps <= 0) fps = 30;   // (no target set: the game's own 30)
    if (fps < 30 || (off && !strcmp(off, "0")) || (g && (!strcmp(g, "high") || !strcmp(g, "low") || !strcmp(g, "lowest")))) return;   // (read before the phone's profile fills DM_GRAPHICS in)
    g_fps_guard = fps; fprintf(stderr, "fps guard: %d fps target: a lighter picture whenever it falls short of 30 or the phone runs hot\n", fps);
}
static void graphics_auto(void) {
    static int mode = 1, cool, slow, held, wait = 12, primed, lv, since; static uint64_t last_fr;
    const char *g = getenv("DM_GRAPHICS");
    if (!(g && !strcmp(g, "auto") && g_th_mgr && g_th_st) && !g_fps_guard) return;
    int hot = 0, calm = 1;
    if (g_th_mgr && g_th_st) { int st = g_th_st(g_th_mgr); float hr = g_th_hr ? g_th_hr(g_th_mgr, 10) : -1.0f;
        hot = st >= 3 || hr >= 0.95f; calm = st <= 1 && (hr < 0 || hr < 0.75f); }
    double fps = -1;
    if (g_fps_guard && dm_nvn_frame_count) { uint64_t fr = dm_nvn_frame_count(); if (primed) fps = (fr - last_fr) / 5.0; last_fr = fr; primed = 1; }
    // decisions on an average, not one 5-s reading: on a hot phone the readings swung 25.6 / 15.4 / 21.6, and the 15.4 made the next reading look like a gain, so the
    //   resolution went 85 → 70 → 60 % in ten seconds without a frame gained (a Galaxy S25+ held back by its CPU)
    static double fps_avg; if (fps > 1) fps_avg = fps_avg > 1 ? 0.5 * fps_avg + 0.5 * fps : fps;
    // the floor is 30: frames are paced at 30 or 60 (hle_nvn.c vstep_tick / dyn_tick), so 720p comes in only when even 30 is missed
    int short_ = fps > 1 && fps_avg < 30 * 0.93, full = fps >= 30 * 0.97;   // (no frames at all: a load or a pause, not a slow scene)
    slow = short_ ? slow + 1 : 0;
    cool = calm && (!g_fps_guard || full) ? cool + 1 : 0;
    if (dm_res_scale_ready()) {   // the render scale (a cap set by hand, DM_RES_CAP or Lowest, is left alone)
        static int probe, probe_hot, nogain, noheat; static double fps_was;
        if (getenv("DM_RES_CAP")) return;
        since++;
        if (nogain > 0) nogain--;
        if (noheat > 0) noheat--;
        if (!dm_res_scale_applies()) { probe = 0; slow = 0; return; }   // (a scene at the game's full size: see above)
        // a step is tried for 15 s, then kept only if the average gained clearly (1.5 fps, or 6 %): a lower resolution only helps a GPU that is behind, not a CPU
        double gain_need = fps_was * 0.06 > 1.5 ? fps_was * 0.06 : 1.5;
        if (probe && !--probe && fps_avg > 1 && fps_was > 1 && lv > 0 && (probe_hot ? fps_avg < fps_was - 1.0 : fps_avg < fps_was + gain_need)) {   // the step did not help
            lv--; dm_res_factor_set(k_res_steps[lv]); slow = 0; since = 0;
            if (probe_hot) noheat = 60; else nogain = 60;
            fprintf(stderr, "%s: the lighter picture %s (%.1f → %.1f fps on average): back to 3D render scale %d %%; %s for 5 minutes\n", probe_hot ? "graphics auto" : "fps guard",
                    probe_hot ? "cost frames" : "did not help", fps_was, fps_avg, (int)(k_res_steps[lv] * 100 + 0.5f),
                    probe_hot ? "heat lowers it no further" : "the slowdown is not the GPU's, so a short frame rate lowers it no further");
            return;
        }
        if (full) held++;
        if (held >= 24) wait = 12;   // two minutes held at this step: the next step down waits a minute again
        int for_heat = hot && since >= 6 && !noheat && !probe, for_rate = slow >= 3 && !nogain && !probe;   // (one step at a time: the last one judged first)
        if ((for_heat || for_rate) && lv < NRES - 1) {
            if (!for_heat && held < 24 && wait < 192) wait *= 2;   // short again soon after a step up: wait twice as long next time
            probe = 3; probe_hot = for_heat; fps_was = fps_avg > 1 ? fps_avg : fps;
            lv++; dm_res_factor_set(k_res_steps[lv]); cool = 0; held = 0; slow = 0; since = 0;
            if (for_heat) fprintf(stderr, "graphics auto: the phone is hot: 3D render scale %d %%\n", (int)(k_res_steps[lv] * 100 + 0.5f));
            else fprintf(stderr, "fps guard: %.1f fps, short of %d: 3D render scale %d %%; a step up after %d s steady\n", fps, g_fps_guard, (int)(k_res_steps[lv] * 100 + 0.5f), wait * 5);
        } else if (lv > 0 && cool >= wait) {
            lv--; dm_res_factor_set(k_res_steps[lv]); cool = 0; held = 0; slow = 0; since = 0; probe = 0;
            fprintf(stderr, "graphics auto: %s: 3D render scale %d %%\n", g_fps_guard ? "steady and cool" : "cooled down", (int)(k_res_steps[lv] * 100 + 0.5f));
        }
        return;
    }
    if (mode == 1) {
        if (full) held++;
        if (held >= 24) wait = 12;   // two minutes held in TV mode: the next fall back waits a minute again
        if (hot || slow >= 2) {
            if (!hot && held < 24 && wait < 192) wait *= 2;   // fell back soon after trying TV mode again: wait twice as long next time
            mode = 0; dm_set_opmode(0); cool = 0; held = 0;
            if (hot) fprintf(stderr, "graphics auto: the phone is hot: handheld mode (720p)\n");
            else fprintf(stderr, "fps guard: %.1f fps, short of %d: handheld mode (720p); TV mode again after %d s steady\n", fps, g_fps_guard, wait * 5);
        }
    } else if (cool >= wait) {
        mode = 1; dm_set_opmode(1); slow = 0; held = 0;
        fprintf(stderr, "graphics auto: %s: TV mode (1080p)\n", g_fps_guard ? "steady and cool" : "cooled down");
    }
}
static void pin_update(void);
static void *log_mirror(void *p) {
    (void)p; char src[1200]; snprintf(src, sizeof src, "%s/log.txt", g_files);
    for (int n = 0;; n++) {
        sleep(5);
        char th[64];
        if (!n) { char t0[64]; thermal_text(t0, sizeof t0); }   // looks the thermal functions up
        graphics_auto(); pin_update();
        if (n % 6 == 0) fprintf(stderr, "[mem] app %ld MB resident (%ld MB swapped), phone %ld MB available of %ld MB; heat %s\n", kb_of("/proc/self/status", "VmRSS:") / 1024,
                                kb_of("/proc/self/status", "VmSwap:") / 1024, kb_of("/proc/meminfo", "MemAvailable:") / 1024, kb_of("/proc/meminfo", "MemTotal:") / 1024, thermal_text(th, sizeof th));
        { char d[256]; snprintf(d, sizeof d, DL "/%s-log.txt", g_stem); copy_file(src, d); }
    }
    return NULL;
}
// ---------- Memory inspector (development): request files in the files folder, answered every second while the game runs ----------
//   scan.req   lines of 64-bit hex values: every 8-byte-aligned occurrence in the process's private writable memory → scan.out (address, value)
//   peek.req   lines "<hex address> <hex length>": the bytes, as hex lines of 32 → peek.out
//   tree.req   lines "<hex node> <hex bytes per node>": the UI node tree under the node (first child at +0x98, next sibling at +0x90; name hash at +0x68),
//              each node's bytes as in peek.out, depth-first, at most 4000 nodes → tree.out
// Reads go through process_vm_readv on this process, so a bad address is an error, not a crash. Used to map the game's UI node trees (the touch driver)
#include <sys/uio.h>
static ssize_t safe_read(uintptr_t a, void *buf, size_t n) {
    struct iovec l = { buf, n }, r = { (void *)a, n }; return process_vm_readv(getpid(), &l, 1, &r, 1, 0);
}
static void inspector_scan(FILE *req, FILE *out) {
    uint64_t want[64]; int nw = 0; char ln[128];
    while (nw < 64 && fgets(ln, sizeof ln, req)) if (*ln != '#' && *ln > ' ') want[nw++] = strtoull(ln, NULL, 16);
    FILE *m = fopen("/proc/self/maps", "r"); static uint64_t buf[1 << 17]; long hits = 0;
    while (m && fgets(ln, sizeof ln, m) && hits < 20000) { unsigned long s, e; char pr[8] = "";
        if (sscanf(ln, "%lx-%lx %7s", &s, &e, pr) != 3 || pr[0] != 'r' || pr[1] != 'w' || pr[3] != 'p') continue;
        for (unsigned long a = s; a < e; a += sizeof buf) { size_t n = e - a < sizeof buf ? e - a : sizeof buf; if (safe_read(a, buf, n) != (ssize_t)n) continue;
            for (size_t i = 0; i < n / 8; i++) for (int k = 0; k < nw; k++) if (buf[i] == want[k]) { fprintf(out, "%lx %016llx\n", a + i * 8, (unsigned long long)want[k]); hits++; } } }
    if (m) fclose(m); fprintf(out, "# %ld hits\n", hits);
}
static void inspector_peek(FILE *req, FILE *out) {
    char ln[128]; unsigned char b[32];
    while (fgets(ln, sizeof ln, req)) { unsigned long a, n; if (sscanf(ln, "%lx %lx", &a, &n) != 2) continue; if (n > 1 << 20) n = 1 << 20;
        fprintf(out, "@ %lx %lx\n", a, n);
        for (unsigned long o = 0; o < n; o += 32) { size_t k = n - o < 32 ? n - o : 32; if (safe_read(a + o, b, k) != (ssize_t)k) { fprintf(out, "%lx unreadable\n", a + o); continue; }
            fprintf(out, "%lx", a + o); for (size_t i = 0; i < k; i++) fprintf(out, "%s%02x", i % 8 ? "" : " ", b[i]); fputc('\n', out); } }
}
static void tree_walk(FILE *out, uintptr_t n, unsigned long per, int depth, int *count) {
    for (int sib = 0; n && *count < 4000 && sib < 2000; sib++) {
        uint64_t h = 0, child = 0, next = 0; unsigned char b[32];
        if (safe_read(n + 0x68, &h, 8) != 8 || safe_read(n + 0x98, &child, 8) != 8 || safe_read(n + 0x90, &next, 8) != 8) { fprintf(out, "%*s! %lx unreadable\n", depth, "", (unsigned long)n); return; }
        (*count)++; fprintf(out, "%*s@ %lx name %016llx depth %d\n", depth, "", (unsigned long)n, (unsigned long long)h, depth);
        for (unsigned long o = 0; o < per; o += 32) { if (safe_read(n + o, b, 32) != 32) break;
            fprintf(out, "%lx", (unsigned long)(n + o)); for (int i = 0; i < 32; i++) fprintf(out, "%s%02x", i % 8 ? "" : " ", b[i]); fputc('\n', out); }
        if (child && depth < 40) tree_walk(out, (uintptr_t)child, per, depth + 1, count);
        n = (uintptr_t)next;
    }
}
static void inspector_tree(FILE *req, FILE *out) {
    char ln[128]; while (fgets(ln, sizeof ln, req)) { unsigned long a, per; if (sscanf(ln, "%lx %lx", &a, &per) != 2) continue; if (per > 0x1000) per = 0x1000;
        int count = 0; fprintf(out, "# tree %lx\n", a); tree_walk(out, a, per, 0, &count); fprintf(out, "# %d nodes\n", count); }
}
static void *inspector_main(void *p) {
    (void)p; char q[1200], o[1200], t[1200];
    for (;;) { sleep(1);
        static const char *kind[3] = { "scan", "peek", "tree" };
        for (int k = 0; k < 3; k++) { snprintf(q, sizeof q, "%s/%s.req", g_files, kind[k]); FILE *r = fopen(q, "r"); if (!r) continue;
            snprintf(t, sizeof t, "%s/%s.tmp", g_files, kind[k]); snprintf(o, sizeof o, "%s/%s.out", g_files, kind[k]); FILE *w = fopen(t, "w");
            if (w) { if (k == 2) inspector_tree(r, w); else if (k) inspector_peek(r, w); else inspector_scan(r, w); fclose(w); rename(t, o); } fclose(r); unlink(q); } }
    return NULL;
}
#include "safe_start.h"   // safe_start(): one level safer after a start that crashed in the graphics driver (tests/t_safe_start.c)
static void host_start(void) {
    if (g_started++) return;
    const char *f = g_app->activity->externalDataPath; snprintf(g_files, sizeof g_files, "%s", f ? f : "/sdcard/nxnative");
    mkdir(g_files, 0755); choose_game(g_files, sizeof g_files);
    { void touch_ui_set_dir(const char *dir); touch_ui_set_dir(g_files); }   // the player's control layout (touch_ui.c's editor)
    char p[1200], q[1200];
    snprintf(p, sizeof p, "%s/log.txt", g_files); snprintf(q, sizeof q, "%s/log.prev.txt", g_files); rename(p, q);
    if (freopen(p, "w", stderr)) setvbuf(stderr, NULL, _IOLBF, 0);
    // the log's first lines: the credits (the app is GPL-3.0-or-later, its older code also MIT: its copyright notice stays with every copy; the full text is in the menu, About) and, once env.txt is
    //   read, whether this is an official build (BuildSigner's DM_BUILD_SIGNER), so a log attached to an issue says at once whether it comes from a repack
    { const char *v = getenv("DM_APP_VERSION");
      fprintf(stderr, "boot: app version code %s. Copyright (c) 2026 jhaizhou (d2r-mobile, nxnative) and the Galar Native (formerly PKShield Android) contributors. "
                      "GPL-3.0-or-later, older parts also MIT (menu, About). Source and official downloads: github.com/simipoB/Galar-Native-Android/releases\n", v && *v ? v : "?"); }
    { char d[256]; snprintf(d, sizeof d, DL "/%s-log-previous.txt", g_stem); copy_file(q, d); }
    snprintf(g_crash_src, sizeof g_crash_src, "%s/log.txt", g_files); snprintf(g_crash_dl, sizeof g_crash_dl, DL "/%s-log.txt", g_stem);
    { pthread_t lt; if (!pthread_create(&lt, NULL, log_mirror, NULL)) pthread_detach(lt); }
    { pthread_t it; if (!pthread_create(&it, NULL, inspector_main, NULL)) pthread_detach(it); }   // the memory inspector (scan.req / peek.req)
    snprintf(p, sizeof p, "%s/env.txt", g_files); env_file(p);
    // The allocator's cache of large blocks, made big enough for the GPU driver's: on a Galaxy S25+ (Adreno 830) a third of the game thread's time went to the driver
    //   mapping and unmapping memory, calloc → mmap and free → munmap in every vkCmdBeginRendering (about 250 a frame) and in every command pool reset, because its
    //   blocks were larger than scudo's secondary cache keeps (simpleperf, 2026-11-09). Cached, they are reused; the pages go back to the system on Android's own
    //   timer (M_DECAY_TIME 1) instead of at once. DM_MALLOC_TUNE=0: the system's defaults
    { const char *mt = getenv("DM_MALLOC_TUNE");
      if (!(mt && !strcmp(mt, "0"))) {
          int a = mallopt(M_DECAY_TIME, 1), b = mallopt(M_CACHE_COUNT_MAX, 128), c = mallopt(M_CACHE_SIZE_MAX, 64 << 20);
          fprintf(stderr, "boot: allocator: large blocks cached for reuse (decay %s, cache count %s, cache size 64 MB %s)\n", a ? "on" : "refused", b ? "128" : "refused", c ? "set" : "refused");
      } }
    { const char *s = getenv("DM_BUILD_SIGNER");
      if (s && !strcmp(s, "official")) fprintf(stderr, "boot: build: official (signed with the release key)\n");
      else fprintf(stderr, "boot: build: UNOFFICIAL (%s): not a release from github.com/simipoB/Galar-Native-Android, it may have been changed\n", s && *s ? s : "signer unknown"); }
    g_safe_advanced = safe_start(g_files);   // (after env.txt: the player's settings win; before the phone profile: its defaults do not)
    // why Android ended the previous run (GameActivity, ApplicationExitInfo): reason 2 a signal, 3 low memory, 4 / 5 a crash, 6 not responding, 10 / 11 the player /
    //   the app, 13 other (Samsung's game manager, heat), 14 frozen
    { const char *le = getenv("DM_LAST_EXIT"); if (le && *le) fprintf(stderr, "boot: Android says the previous run ended: %s\n", le); }
    fps_guard_init();   // (before the phone profile: Graphics chosen by hand only)
    device_profile();   // the chip tier's defaults, under env.txt
    // Game files: inside the installed APKs (the all-in-one .apks bundle, read through apkfs.c as /nxapk/...), else copied into the files folder
    struct stat st; int inapk = !stat("/nxapk/RomFS", &st);
    snprintf(g_game, sizeof g_game, "%s", inapk ? "/nxapk" : g_files);
    #define SETG(k, sub) do { snprintf(p, sizeof p, "%s/%s", g_game, sub); setenv(k, p, 0); } while (0)
    #define SETP(k, sub) do { snprintf(p, sizeof p, "%s/%s", g_files, sub); setenv(k, p, 0); } while (0)
    SETP("DM_TEX_CACHE", "texcache");
    { const char *r = g_app->activity->externalDataPath; snprintf(p, sizeof p, "%s/lsfg", r ? r : g_files); setenv("DM_LSFG_DIR", p, 0); }
    // (the player's Lossless Scaling shaders, converted from their Lossless.dll by the app's import, one folder for every game: DM_FRAMEGEN=lsfg, nvn_lsfg.h)
    // shaders translated while the game last ran (nvn_vk.c live_check): the pack has them now (setup_game.py merged them first), so the folder starts empty
    // the fast setup's shaders translated after it (setup_game.py --rest): kept across starts while rest.json says some are still to come, cleared once they are all in the pack
    SETP("DM_REST_SPV", "rest");
    { char rj[1200]; struct stat rs; snprintf(rj, sizeof rj, "%s/rest.json", g_files);
      if (stat(rj, &rs)) { char rd[1200]; snprintf(rd, sizeof rd, "%s/rest", g_files); DIR *d1 = opendir(rd); struct dirent *e1;
          if (d1) { while ((e1 = readdir(d1))) if (e1->d_name[0] != '.') { char sd[1500]; snprintf(sd, sizeof sd, "%s/%s", rd, e1->d_name); DIR *d2 = opendir(sd); struct dirent *e2;
                        if (d2) { while ((e2 = readdir(d2))) if (e2->d_name[0] != '.') { char f2[1800]; snprintf(f2, sizeof f2, "%s/%s", sd, e2->d_name); unlink(f2); } closedir(d2); } rmdir(sd); }
                    closedir(d1); rmdir(rd); } } }
    SETP("DM_LIVE_SPV", "live");
    { DIR *ld = opendir(getenv("DM_LIVE_SPV")); struct dirent *e; char f[1200];
      if (ld) { while ((e = readdir(ld))) if (e->d_name[0] != '.') { snprintf(f, sizeof f, "%s/%s", getenv("DM_LIVE_SPV"), e->d_name); unlink(f); } closedir(ld); } }   // textures converted for a GPU without BC, kept for the next start (tex_cache.h)
    setenv("DM_AUDIO", "1", 0);
    // graphics (the app's settings screen): high = the console's TV mode (1080p, full effects), low = handheld mode (720p, the game's own lighter settings),
    // auto = TV mode, made lighter while the phone is hot or slow (graphics_auto below), lowest = handheld mode with the game's 3D render scale capped at 75 % (960x540 in
    // Pokémon Sword / Shield, hle_resscale.c; other games: as low). DM_OPMODE and DM_RES_CAP in env.txt still win
    { const char *g = getenv("DM_GRAPHICS"); if (g && (!strcmp(g, "low") || !strcmp(g, "lowest"))) setenv("DM_OPMODE", "0", 0); else if (g && (!strcmp(g, "high") || !strcmp(g, "auto"))) setenv("DM_OPMODE", "1", 0);
      if (g && !strcmp(g, "lowest")) setenv("DM_RES_CAP", "0.75", 0); }
#if defined(DM_GAME_ANDROID_OPMODE) && DM_GAME_ANDROID_OPMODE >= 0
    { char m[4]; snprintf(m, sizeof m, "%d", DM_GAME_ANDROID_OPMODE); setenv("DM_OPMODE", m, 0); }   // the profile's android_op_mode (env.txt DM_OPMODE wins)
#endif   // real sound output (AAudio, compat/aaudio_unit.c); env.txt can set 0
    // pipelines: every one the game makes is recorded here and built ahead at the next start, the list shipped with the game too, and the driver's compiled code kept
    SETP("DM_MODS_DIR", "mods");   // IPSwitch .pchtxt code mods, applied when their build ID matches (loader.c); env.txt may point elsewhere
    snprintf(p, sizeof p, "%s/mods", g_files); mkdir(p, 0770);   // made by the app itself, so it can read what is put there (a folder another user made, adb's for one, it cannot)
    boot_guard(p);   // (before the mods are read: a crash with mods switches them off)
    mods_romfs(p);
    SETP("DM_VK_PSO_RECORD", "vk_pipelines.bin"); SETP("DM_VK_PCACHE", "vk_pipeline_cache.bin"); SETG("DM_VK_PSO_SEED", "vk_pipelines_seed.bin");
    snprintf(q, sizeof q, "%s/shaders.pak", g_game); if (!stat(q, &st)) setenv("DM_SPV_PAK", q, 0);   // the packed shader set, else vk/b-<chain>/
    { snprintf(p, sizeof p, "%s/aoc", g_game); struct stat as; if (!stat(p, &as)) setenv("DM_AOC_DIR", p, 0); }   // the DLC the player owns (Shield: the Expansion Pass keys; its content is in the update)
    SETP("DM_NVN_CAPTURE", "nvn_capture");   // shaders the game builds in memory, saved for the setup to translate (nvn_vk.c mtl_shader_seen)
    SETG("DM_ROMFS", "RomFS"); SETG("DM_SPV_DIR", "vk"); SETG("DM_NVN_SHMAP", "shader_map.tsv"); SETG("DM_FONT_DIR", "font");
    SETP("DM_SAVE", "save"); SETP("DM_TEMP", "temp");
    // A save archive imported by SetupActivity is staged as save_import/. Rename the directories on the same
    // filesystem so nested save layouts survive and the old save remains intact if the import cannot be installed.
    { char imp[1200], old[1300]; struct stat is, ss;
      snprintf(imp, sizeof imp, "%s/save_import", g_files);
      snprintf(p, sizeof p, "%s/save", g_files);
      if (!lstat(imp, &is) && S_ISDIR(is.st_mode)) {
          int moved_old = 0, can_install = 1;
          int save_status = stat(p, &ss);
          if (!save_status) {
              snprintf(old, sizeof old, "%s/save.before-import-%ld-%d", g_files, (long)time(NULL), (int)getpid());
              if (!rename(p, old)) moved_old = 1;
              else { fprintf(stderr, "[RED] save import: could not preserve the current save: %s\n", strerror(errno)); can_install = 0; }
          } else if (errno != ENOENT) {
              fprintf(stderr, "[RED] save import: could not inspect the current save: %s\n", strerror(errno)); can_install = 0;
          }
          if (can_install && !rename(imp, p)) {
              fprintf(stderr, "boot: imported save archive; previous save backup: %s\n", moved_old ? old : "none");
          } else {
              if (moved_old) rename(old, p);
              fprintf(stderr, "[RED] save import: could not install imported save; it remains in %s\n", imp);
          }
      } }
    snprintf(p, sizeof p, "%s/save", g_files); mkdir(p, 0755); snprintf(p, sizeof p, "%s/temp", g_files); mkdir(p, 0755);
    snprintf(p, sizeof p, "%s/RomFS", g_game);
    if (stat(p, &st)) { fprintf(stderr, "boot: game files not found: %s (install the bundle, or copy RomFS, patched, shaders.pak, shader_map.tsv and font there)\n", p); ALOG("game files missing in %s", g_files); return; }
    chdir(g_files);
    fprintf(stderr, "boot: game files %s%s\n", g_game, inapk ? " (inside the installed APKs)" : "");
    pthread_attr_t at; pthread_attr_init(&at); pthread_attr_setstacksize(&at, 64u << 20);
    pthread_t t; pthread_create(&t, &at, host_thread, NULL); pthread_detach(t);
    { pthread_t g; if (!pthread_create(&g, NULL, boot_guard_main, NULL)) pthread_detach(g); }
}
// ---------- The fastest cores for the threads a frame waits on. Left alone, the scheduler kept Violet's main thread moving over the middle cores of a Galaxy S25+ (2.7 GHz) with
// its two fastest cores idle at 1 GHz (pinned, the replay's CPU cost fell from 21 to 14 ms a frame). "Fastest" is each core's clock limit right now, looked at again every 5 s
// (log_mirror): a hot phone can cap its prime cores below the others (1.69 GHz seen against 2.9), and then the threads move. A phone whose cores are all alike is left alone.
// DM_CPU_PIN=off leaves the scheduler to it
static pid_t g_pin_tid[2]; static int g_pin_n; static long g_pin_top;
static void pin_update(void) {
    const char *e = getenv("DM_CPU_PIN"); if ((e && !strcmp(e, "off")) || !g_pin_n) return;
    long top = 0, low = 0, f[32]; cpu_set_t set; CPU_ZERO(&set); int n = 0;
    for (int c = 0; c < 32; c++) { char p[96]; snprintf(p, sizeof p, "/sys/devices/system/cpu/cpu%d/cpufreq/scaling_max_freq", c); FILE *fp = fopen(p, "r"); f[c] = 0;
        if (fp) { if (fscanf(fp, "%ld", &f[c]) != 1) f[c] = 0; fclose(fp); } if (f[c] > top) top = f[c]; if (f[c] && (!low || f[c] < low)) low = f[c]; }
    if (!top) return;
    if (top == low) { if (!g_pin_top) return; for (int c = 0; c < 32; c++) if (f[c]) { CPU_SET(c, &set); n++; } }   // all alike now: every core
    else for (int c = 0; c < 32; c++) if (f[c] == top) { CPU_SET(c, &set); n++; }
    if (top == g_pin_top && !(top == low)) { static cpu_set_t last; if (CPU_EQUAL(&last, &set)) return; last = set; }
    g_pin_top = top;
    for (int i = 0; i < g_pin_n; i++) sched_setaffinity(g_pin_tid[i], sizeof set, &set);
    fprintf(stderr, "[cpu] main threads on %d core%s limited to %.2f GHz\n", n, n == 1 ? "" : "s", top / 1e6);
}
// ---------- Performance hints (Android 13+): one session for the game's main thread and the presenting thread, told every frame how long the presenting thread
// worked against the frame's period (nvn_vk.c mtl_present). That is what brings the fastest cores and higher clocks when the game needs them, and lower ones
// when it does not (LAS Recomp's Android port: the prime core otherwise stays parked). The presenting thread also gets the most urgent priority an app may set
static void *g_hint; static int g_hint_failed; static int64_t g_hint_target;
static void *(*p_hint_mgr)(void); static void *(*p_hint_create)(void *, const int32_t *, size_t, int64_t);
static int (*p_hint_target)(void *, int64_t); static int (*p_hint_report)(void *, int64_t);
void front_frame_work(uint64_t busy_ns, uint64_t target_ns) {
    if (g_hint_failed) return;
    if (!g_hint) {
        static int tried; if (tried++) return;
        setpriority(PRIO_PROCESS, (id_t)gettid(), -10);
        void *lib = dlopen("libandroid.so", RTLD_NOW); if (!lib) { g_hint_failed = 1; return; }
        p_hint_mgr = (void *(*)(void))dlsym(lib, "APerformanceHint_getManager");
        p_hint_create = (void *(*)(void *, const int32_t *, size_t, int64_t))dlsym(lib, "APerformanceHint_createSession");
        p_hint_target = (int (*)(void *, int64_t))dlsym(lib, "APerformanceHint_updateTargetWorkDuration");
        p_hint_report = (int (*)(void *, int64_t))dlsym(lib, "APerformanceHint_reportActualWorkDuration");
        void *m = p_hint_mgr ? p_hint_mgr() : NULL;
        int32_t t[2 + 8]; size_t n = 0; t[n++] = gettid(); if (g_game_tid && g_game_tid != gettid()) t[n++] = g_game_tid;
        size_t pinned = n;   // (only these two are pinned to the top cores: the workers below share the frame's work and would crowd them)
        { extern int thr_frame_tids(int *out, int max) __attribute__((weak)); int w[8]; int k = thr_frame_tids ? thr_frame_tids(w, 8) : 0;
          for (int i = 0; i < k; i++) if (w[i] != t[0] && (pinned < 2 || w[i] != t[1])) t[n++] = w[i]; }
        g_hint = m && p_hint_create ? p_hint_create(m, t, n, (int64_t)target_ns) : NULL;
        if (!g_hint && n > pinned && m && p_hint_create) g_hint = p_hint_create(m, t, n = pinned, (int64_t)target_ns);   // (refused with the workers: the two threads as before)
        if (!g_hint || !p_hint_report) { g_hint_failed = 1; fprintf(stderr, "[cpu] no performance hint session\n"); return; }
        g_hint_target = (int64_t)target_ns; for (size_t i = 0; i < pinned; i++) g_pin_tid[i] = t[i]; g_pin_n = (int)pinned; pin_update();
        fprintf(stderr, "[cpu] performance hint session: %zu threads (%zu of them the game's frame workers), target %.1f ms; presenting thread at priority -10\n", n, n - pinned, target_ns / 1e6);
    }
    if ((int64_t)target_ns != g_hint_target && p_hint_target) p_hint_target(g_hint, g_hint_target = (int64_t)target_ns);
    if (busy_ns) p_hint_report(g_hint, (int64_t)busy_ns);
}
// The host's front_run (main.c): the guest already runs on our big-stack host thread
void front_run(void (*guest)(void)) { guest(); }

// The system bars hidden (app/java/com/nxnative/Immersive.java), from the app's event thread (an ordinary thread, attached to Java once and kept attached)
static void immersive(void) {
    static JNIEnv *e; static jclass im; static jmethodID ap; static int failed;
    if (failed) return;
    if (!e && (*g_app->activity->vm)->AttachCurrentThread(g_app->activity->vm, &e, NULL)) { failed = 1; return; }
    if (!im) {
        jobject act = g_app->activity->clazz; jclass ac = (*e)->GetObjectClass(e, act);
        jobject cl = (*e)->CallObjectMethod(e, act, (*e)->GetMethodID(e, ac, "getClassLoader", "()Ljava/lang/ClassLoader;"));
        jclass clc = (*e)->FindClass(e, "java/lang/ClassLoader"); jstring cn = (*e)->NewStringUTF(e, "com.nxnative.Immersive");
        jclass c = (jclass)(*e)->CallObjectMethod(e, cl, (*e)->GetMethodID(e, clc, "loadClass", "(Ljava/lang/String;)Ljava/lang/Class;"), cn);
        if ((*e)->ExceptionCheck(e) || !c) { (*e)->ExceptionClear(e); failed = 1; fprintf(stderr, "[RED] immersive: the class is missing\n"); return; }
        im = (jclass)(*e)->NewGlobalRef(e, c); ap = (*e)->GetStaticMethodID(e, im, "apply", "(Landroid/app/Activity;)V");
    }
    (*e)->CallStaticVoidMethod(e, im, ap, g_app->activity->clazz);
    if ((*e)->ExceptionCheck(e)) { (*e)->ExceptionDescribe(e); (*e)->ExceptionClear(e); }
}
// Back from the background: the system may have swapped the game's memory out (Samsung's RAM Plus did, 586 MB), and every swapped page then came back on its own
// fault in the middle of play (battles stuttered: frames of 100–160 ms). One thread asks for all of it back at once, ahead of the game (MADV_WILLNEED on the
// private writable mappings: swapped pages are read back, pages never touched stay untouched)
static void *swapin_main(void *p) {
    (void)p; long before = kb_of("/proc/self/status", "VmSwap:"); FILE *f = fopen("/proc/self/maps", "r"); char ln[512]; double mb = 0;
    while (f && fgets(ln, sizeof ln, f)) { unsigned long s, e; char pr[8] = "";
        if (sscanf(ln, "%lx-%lx %7s", &s, &e, pr) == 3 && pr[0] == 'r' && pr[1] == 'w' && pr[3] == 'p') { madvise((void *)s, e - s, MADV_WILLNEED); mb += (e - s) / 1048576.0; } }
    if (f) fclose(f);
    sleep(3); fprintf(stderr, "[mem] back from the background: swap-in asked over %.0f MB of mappings, swapped %ld → %ld MB\n", mb, before / 1024, kb_of("/proc/self/status", "VmSwap:") / 1024);
    return NULL;
}
static void swapin_start(void) { if (kb_of("/proc/self/status", "VmSwap:") < 32 * 1024) return; pthread_t t; if (!pthread_create(&t, NULL, swapin_main, NULL)) pthread_detach(t); }
__attribute__((weak)) void nvn_vk_cache_save(void);
__attribute__((weak)) void nvn_vk_heap_alive(void);   // nvn_vk.c: the dma-buf heap route lived to the background, so its crash marker goes
__attribute__((weak)) void nvn_vk_window_term(void);   // releases the swapchain while the window still exists (nvn_vk.c)   // the pipeline cache file (nvn_vk.c), written when the app goes to the background
static void on_cmd(struct android_app *app, int32_t cmd) {
    switch (cmd) {
        case APP_CMD_INIT_WINDOW: atomic_store(&g_window, app->window); immersive(); host_start(); window_rate(app->window); break;   // (after host_start: env.txt and the Auto settings decide the rate)
        case APP_CMD_TERM_WINDOW: atomic_store(&g_window, NULL); if (g_started && nvn_vk_window_term) nvn_vk_window_term(); break;
        case APP_CMD_GAINED_FOCUS: immersive(); if (g_started) { dm_focus_set(1); dm_audio_suspend(0); } break;
        // In the background (recents, home, another app) the game keeps running with its sound, as a Switch game does under the HOME menu with a suspend-free
        //   setting: it is not told it lost focus (it would pause itself), and frames are simply not shown while there is no window. DM_BACKGROUND=off pauses it as before
        case APP_CMD_LOST_FOCUS: if (g_started && !bg_play()) { dm_focus_set(2); } break;
        case APP_CMD_PAUSE: if (g_started && nvn_vk_heap_alive) nvn_vk_heap_alive(); if (g_started) { if (!bg_play()) { dm_focus_set(3); dm_audio_suspend(1); } if (nvn_vk_cache_save) nvn_vk_cache_save(); } break;
        case APP_CMD_RESUME: if (g_started) { dm_focus_set(1); dm_audio_suspend(0); swapin_start(); } break;
    }
}
void android_main(struct android_app *app) {
    g_app = app; app->onAppCmd = on_cmd; app->onInputEvent = on_input;
    ANativeActivity_setWindowFlags(app->activity, AWINDOW_FLAG_KEEP_SCREEN_ON | AWINDOW_FLAG_FULLSCREEN, 0);
    touch_ui_init(app->activity);
    for (;;) {
        int events; struct android_poll_source *src;
        // the game never started (GameActivity handed over to the setup screen before it had a window): the process goes on for that screen
        while (ALooper_pollOnce(-1, NULL, &events, (void **)&src) >= 0) { if (src) src->process(app, src); if (app->destroyRequested) { if (g_started) _exit(0); return; } }
    }
}
