// etc_encode.c against an independent decoder (etc_check.py): writes test images and their ETC2 RGBA8 / EAC R11 encodings for the checker to decode and compare.
//   etc_test <out dir>   → <dir>/img<N>.rgba (W x H RGBA8), <dir>/img<N>.etc2 (ETC2 RGBA8 blocks), <dir>/img<N>.r11 (EAC R11 blocks of the red channel),
//   <dir>/img<N>.r11s (EAC R11 signed blocks of red - 128), <dir>/img<N>.astc (ASTC 4x4 blocks)
#include "../etc_encode.h"
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
enum { W = 64, H = 64 };
static uint32_t rnd = 12345; static uint32_t rnext(void) { rnd = rnd * 1103515245u + 12345u; return rnd >> 8; }
static void make(int n, uint8_t *img) {
    for (int y = 0; y < H; y++) for (int x = 0; x < W; x++) {
        uint8_t *p = img + (y * W + x) * 4;
        switch (n) {
        case 0: p[0] = (uint8_t)(x * 4); p[1] = (uint8_t)(y * 4); p[2] = (uint8_t)((x + y) * 2); p[3] = 255; break;                      // smooth gradients
        case 1: p[0] = p[1] = p[2] = ((x / 8 + y / 8) & 1) ? 230 : 20; p[3] = (uint8_t)(x * 4); break;                                // hard edges, alpha ramp
        case 2: p[0] = (uint8_t)rnext(); p[1] = (uint8_t)rnext(); p[2] = (uint8_t)rnext(); p[3] = (uint8_t)rnext(); break;              // noise (worst case)
        default: { double d = hypot(x - 32, y - 32); p[0] = (uint8_t)(128 + 120 * sin(d / 4)); p[1] = (uint8_t)(128 + 120 * cos(d / 5));  // rings, a soft alpha edge
                   p[2] = (uint8_t)(x * 3); p[3] = d < 24 ? 255 : d > 28 ? 0 : (uint8_t)((28 - d) * 64); }
        }
    }
}
int main(int argc, char **argv) {
    if (argc < 2) return 2;
    for (int n = 0; n < 4; n++) {
        uint8_t img[W * H * 4], etc[(W / 4) * (H / 4) * 16], r11[(W / 4) * (H / 4) * 8], r11s[(W / 4) * (H / 4) * 8], astc[(W / 4) * (H / 4) * 16];
        make(n, img);
        for (int by = 0; by < H / 4; by++) for (int bx = 0; bx < W / 4; bx++) {
            uint8_t px[16][4], a[16], r[16]; int8_t rs[16];
            for (int y = 0; y < 4; y++) for (int x = 0; x < 4; x++) { const uint8_t *p = img + ((by * 4 + y) * W + bx * 4 + x) * 4; memcpy(px[y * 4 + x], p, 4); a[y * 4 + x] = p[3]; r[y * 4 + x] = p[0]; rs[y * 4 + x] = (int8_t)(p[0] - 128); }
            uint8_t *o = etc + (by * (W / 4) + bx) * 16; eac_encode_a8(a, o); etc2_encode_rgb(px, o + 8);
            eac_encode_r11(r, r11 + (by * (W / 4) + bx) * 8); eac_encode_r11s(rs, r11s + (by * (W / 4) + bx) * 8);
            astc_encode_rgba(px, astc + (by * (W / 4) + bx) * 16);
        }
        char f[512]; FILE *o;
        snprintf(f, sizeof f, "%s/img%d.rgba", argv[1], n); o = fopen(f, "wb"); fwrite(img, 1, sizeof img, o); fclose(o);
        snprintf(f, sizeof f, "%s/img%d.etc2", argv[1], n); o = fopen(f, "wb"); fwrite(etc, 1, sizeof etc, o); fclose(o);
        snprintf(f, sizeof f, "%s/img%d.r11", argv[1], n); o = fopen(f, "wb"); fwrite(r11, 1, sizeof r11, o); fclose(o);
        snprintf(f, sizeof f, "%s/img%d.r11s", argv[1], n); o = fopen(f, "wb"); fwrite(r11s, 1, sizeof r11s, o); fclose(o);
        snprintf(f, sizeof f, "%s/img%d.astc", argv[1], n); o = fopen(f, "wb"); fwrite(astc, 1, sizeof astc, o); fclose(o);
    }
    puts("written");
    return 0;
}
