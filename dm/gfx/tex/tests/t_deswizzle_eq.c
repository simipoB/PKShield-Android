// The GOB-by-GOB deswizzle (../nvn_deswizzle.c move) against addressing every 16-byte segment on its own through nvn_bl_offset (the way it was done before):
// both directions, every format the layout knows, 1D / 2D / 3D / arrays / cube maps / rectangles, odd sizes, every mip level and layer, automatic and packaged
// block heights, row pitches wider than the level. Byte for byte, or the test fails.
// Build: cc -O2 -Wall -o t_deswizzle_eq t_deswizzle_eq.c ../nvn_deswizzle.c ../../nvn/nvn_tex.c -lm
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "../nvn_deswizzle.h"
#include "../../nvn/nvn_tex.h"

static uint64_t rng = 0x9e3779b97f4a7c15ull;
static uint32_t rnd(void) { rng ^= rng << 13; rng ^= rng >> 7; rng ^= rng << 17; return (uint32_t)rng; }

// the previous implementation, as it was
static int ref_move(const nvn_texdesc *d, unsigned level, unsigned layer, uint8_t *bl, uint8_t *lin, size_t pitch, int to_linear) {
    uint32_t row_bytes, rows, slices, bpb; uint64_t base; unsigned th, td;
    if (nvn_deswizzle_geom(d, level, layer, &row_bytes, &rows, &slices, &base, &th, &td, &bpb)) return -1;
    if (pitch < row_bytes) return -1;
    bl += base;
    for (uint32_t z = 0; z < slices; z++)
        for (uint32_t y = 0; y < rows; y++) {
            uint8_t *row = lin + ((size_t)z * rows + y) * pitch;
            for (uint32_t x = 0; x < row_bytes; x += 16) {
                uint32_t n = row_bytes - x < 16 ? row_bytes - x : 16;
                uint8_t *p = bl + nvn_bl_offset(x, y, z, row_bytes, rows, th, td);
                if (to_linear) memcpy(row + x, p, n); else memcpy(p, row + x, n);
            }
        }
    return 0;
}

int main(void) {
    static const int targets[] = { 0, 1, 2, 3, 4, 7, 8, 9 };
    int fmts[256], nf = 0;
    for (int f = 1; f < 256; f++) { int bw, bh; if (nvn_fmt_bpb(f, &bw, &bh)) fmts[nf++] = f; }
    long cases = 0, fails = 0;
    for (int it = 0; it < 1200; it++) {   // (about 45 000 level / layer cases: 1.5 min, the reference being slow; 6000 rounds checked once, 228 701 cases, 0 failures)
        nvn_texdesc d; memset(&d, 0, sizeof d);
        d.target = targets[rnd() % (sizeof targets / sizeof *targets)]; d.format = fmts[rnd() % nf];
        uint32_t big = it % 10 == 0 ? 1024 : 300;
        d.width = 1 + rnd() % big; d.height = (d.target == 0 || d.target == 3) ? 1 : 1 + rnd() % big;
        if (d.target == 8) d.height = d.width;   // cube faces are square
        d.depth = d.target == 2 ? 1 + rnd() % 40 : (d.target == 3 || d.target == 4) ? 1 + rnd() % 6 : d.target == 9 ? 6 * (1 + rnd() % 2) : d.target == 8 ? 6 : 1;
        unsigned maxl = 1; for (uint32_t s = d.width > d.height ? d.width : d.height; s > 1; s >>= 1) maxl++;
        d.levels = 1 + rnd() % maxl;
        d.pkg_block_h = rnd() % 3 ? 0 : 1 + rnd() % 6;
        nvn_texlayout L; nvn_tex_layout(&d, &L);
        if (!L.size || L.size > (96u << 20)) continue;
        uint8_t *st = malloc(L.size), *st2 = malloc(L.size), *st3 = malloc(L.size);
        for (uint64_t i = 0; i < L.size; i++) st[i] = (uint8_t)rnd();
        for (unsigned lv = 0; lv < d.levels; lv++)
            for (unsigned ly = 0; ly < L.layers; ly++) {
                uint32_t rb, rows, sl, bpb; uint64_t base; unsigned th, td;
                if (nvn_deswizzle_geom(&d, lv, ly, &rb, &rows, &sl, &base, &th, &td, &bpb)) continue;
                size_t pitch = rb + (rnd() % 3 ? 0 : 16 * (rnd() % 4)), n = pitch * rows * sl;
                uint8_t *a = malloc(n), *b = malloc(n);
                memset(a, 0xab, n); memset(b, 0xab, n);
                int ra = ref_move(&d, lv, ly, st, a, pitch, 1), rbv = nvn_deswizzle(&d, lv, ly, st, b, pitch);
                cases++;
                if (ra != rbv || memcmp(a, b, n)) { if (fails++ < 10) printf("FAIL deswizzle: target %d format %#x %ux%ux%u levels %u level %u layer %u pkg_bh %d\n", d.target, d.format, d.width, d.height, d.depth, d.levels, lv, ly, d.pkg_block_h); }
                // the other way: the same linear data into two copies of the storage
                memcpy(st2, st, L.size); memcpy(st3, st, L.size);
                for (size_t i = 0; i < n; i++) a[i] = (uint8_t)rnd();
                ref_move(&d, lv, ly, st2, a, pitch, 0); nvn_swizzle(&d, lv, ly, a, pitch, st3);
                if (memcmp(st2, st3, L.size)) { if (fails++ < 10) printf("FAIL swizzle: target %d format %#x %ux%ux%u level %u layer %u\n", d.target, d.format, d.width, d.height, d.depth, lv, ly); }
                free(a); free(b);
            }
        free(st); free(st2); free(st3);
    }
    printf("%ld level/layer cases over %d formats: %ld failures\n", cases, nf, fails);
    return fails ? 1 : 0;
}
