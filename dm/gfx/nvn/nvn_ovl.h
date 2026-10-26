// The touch overlay the Vulkan replay draws over the game picture each present (nvn_vk.c ovl_*): the front end (dm/android/app/jni/android_main.c) says what to draw,
// in pixels of the window surface, origin top-left
#pragma once
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
