// The touch overlay the Vulkan replay draws over the game picture each present (nvn_vk.c ovl_*): the front end (dm/android/app/jni/android_main.c) says what to draw,
// in pixels of the window surface, origin top-left
#pragma once
#include <math.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
enum { OVL_DISC = 0, OVL_RING = 1, OVL_RRECT = 2 };
enum { OVL_G_A, OVL_G_B, OVL_G_X, OVL_G_Y, OVL_G_L, OVL_G_R, OVL_G_ZL, OVL_G_ZR, OVL_G_PLUS, OVL_G_MINUS, OVL_G_NONE = -1 };
typedef struct {
    float x, y, w, h;          // rectangle, pixels
    float r, g, b, a;          // fill colour (straight alpha)
    int shape; float ring;     // OVL_*; ring width as a fraction of the radius (OVL_RING)
    float radius;              // OVL_RRECT corner radius, fraction of the height
    int glyph; float gscale, galpha;   // OVL_G_* label, its size as a fraction of the control, its opacity
} dm_ovl_item;
// Fills up to max items for a surface of width x height pixels; returns how many (0: nothing drawn)
int front_overlay(dm_ovl_item *it, int max, int width, int height);

// Where the game picture goes in a ww x wh window: box = x, y, width, height in window pixels. By default the picture keeps its own aspect (sw x sh), fitted and centred;
// DM_ASPECT=20:9 (opt-in, for a 20:9 .pchtxt mod that widens the game's camera) gives it a 20:9 box instead, the 16:9 picture stretched into it (nvn_vk.c present_window, touch_menu.c)
static inline int dm_aspect_20_9(void) { static int m = -1; if (m < 0) { const char *e = getenv("DM_ASPECT"); m = e && !strcmp(e, "20:9"); } return m; }
static inline void dm_picture_box(int32_t ww, int32_t wh, int32_t sw, int32_t sh, int32_t box[4]) {
    double ar = dm_aspect_20_9() ? 20.0 / 9.0 : sh > 0 ? (double)sw / sh : 16.0 / 9.0, bh = fmin((double)wh, ww / ar), bw = bh * ar;
    box[2] = (int32_t)bw > 0 ? (int32_t)bw : 1; box[3] = (int32_t)bh > 0 ? (int32_t)bh : 1; box[0] = (ww - box[2]) / 2; box[1] = (wh - box[3]) / 2;
}
