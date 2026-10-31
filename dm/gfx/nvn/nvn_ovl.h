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

// Presentation aspect ratio (DM_ASPECT: fit | 20:9 | crop | stretch), shared by nvn_vk.c and touch_menu.c
enum { DM_ASPECT_FIT = 0, DM_ASPECT_20_9 = 1, DM_ASPECT_CROP = 2, DM_ASPECT_STRETCH = 3 };
static inline int dm_aspect_parse(const char *e) {
    if (!e || !*e || !strcmp(e, "fit") || !strcmp(e, "16:9")) return DM_ASPECT_FIT;
    if (!strcmp(e, "20:9")) return DM_ASPECT_20_9;
    if (!strcmp(e, "crop") || !strcmp(e, "zoom")) return DM_ASPECT_CROP;
    if (!strcmp(e, "stretch") || !strcmp(e, "full")) return DM_ASPECT_STRETCH;
    return DM_ASPECT_FIT;
}
static inline int dm_aspect_mode(void) {
    static int m = -1;
    if (m < 0) m = dm_aspect_parse(getenv("DM_ASPECT"));
    return m;
}
static inline void dm_present_rect(int mode, int32_t tw, int32_t th, const int crop[4], int32_t ww, int32_t wh, int32_t src[4], int32_t dst[4]) {
    int32_t sx = crop ? crop[0] : 0, sy = crop ? crop[1] : 0;
    int32_t sw = crop && crop[2] ? crop[2] : tw, sh = crop && crop[3] ? crop[3] : th;
    if (sx < 0) sx = 0;
    if (sy < 0) sy = 0;
    if (sw < 1 || sx + sw > tw) sw = tw - sx > 0 ? tw - sx : 1;
    if (sh < 1 || sy + sh > th) sh = th - sy > 0 ? th - sy : 1;
    if (ww < 1) ww = 1;
    if (wh < 1) wh = 1;
    int32_t dx = 0, dy = 0, dw = ww, dh = wh;
    if (mode == DM_ASPECT_20_9) {
        double sc = fmin((double)ww / 20.0, (double)wh / 9.0);
        dw = (int32_t)(20.0 * sc + 0.5); if (dw > ww) dw = ww; if (dw < 1) dw = 1;
        dh = (int32_t)(9.0 * sc + 0.5);  if (dh > wh) dh = wh; if (dh < 1) dh = 1;
        dx = (ww - dw) / 2; dy = (wh - dh) / 2;
    } else if (mode == DM_ASPECT_CROP) {
        if ((int64_t)ww * sh > (int64_t)wh * sw) {
            int32_t nh = (int32_t)(((int64_t)sw * wh + ww / 2) / ww);
            if (nh < 1) nh = 1;
            if (nh > sh) nh = sh;
            sy += (sh - nh) / 2; sh = nh;
        } else if ((int64_t)ww * sh < (int64_t)wh * sw) {
            int32_t nw = (int32_t)(((int64_t)sh * ww + wh / 2) / wh);
            if (nw < 1) nw = 1;
            if (nw > sw) nw = sw;
            sx += (sw - nw) / 2; sw = nw;
        }
    } else if (mode != DM_ASPECT_STRETCH) {
        double sc = fmin((double)ww / sw, (double)wh / sh);
        dw = (int32_t)(sw * sc); if (dw < 1) dw = 1;
        dh = (int32_t)(sh * sc); if (dh < 1) dh = 1;
        dx = (ww - dw) / 2; dy = (wh - dh) / 2;
    }
    src[0] = sx; src[1] = sy; src[2] = sw; src[3] = sh;
    dst[0] = dx; dst[1] = dy; dst[2] = dw; dst[3] = dh;
}
static inline int dm_menu_tap_layout(int mode, float x, float y, float ww, float wh, float *lx, float *ly) {
    int32_t s[4], d[4]; dm_present_rect(mode, 1920, 1080, NULL, (int32_t)ww, (int32_t)wh, s, d);
    if (x < (float)d[0] || x > (float)(d[0] + d[2]) || y < (float)d[1] || y > (float)(d[1] + d[3])) return 0;
    *lx = ((float)s[0] + (x - (float)d[0]) / (float)d[2] * (float)s[2]) - 960.0f;
    *ly = 540.0f - ((float)s[1] + (y - (float)d[1]) / (float)d[3] * (float)s[3]);
    return *lx >= -960.0f && *lx <= 960.0f && *ly >= -540.0f && *ly <= 540.0f;
}
