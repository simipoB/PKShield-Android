// BC → ETC2 / EAC transcoding (etc_encode.h): each BC block decoded to texels (bc_decode.c), then encoded again.
// ETC2 colour: ETC1's individual and differential modes (valid ETC2: a differential pair is kept inside 0..31, which ETC2 would read as its T / H / planar
// modes otherwise), both sub-block splits and all 8 intensity tables tried, the least squared error kept. EAC: per modifier table, the multiplier and base
// around the block's range, each texel's nearest modifier. Checked against an independent decoder (dm/gfx/tex/tests/etc_check.py)
#include "etc_encode.h"
#include "bc_decode.h"
#include <string.h>

static const int ETC_TAB[8][2] = { { 2, 8 }, { 5, 17 }, { 9, 29 }, { 13, 42 }, { 18, 60 }, { 24, 80 }, { 33, 106 }, { 47, 183 } };
static const int EAC_TAB[16][8] = {
    { -3, -6, -9, -15, 2, 5, 8, 14 }, { -3, -7, -10, -13, 2, 6, 9, 12 }, { -2, -5, -8, -13, 1, 4, 7, 12 }, { -2, -4, -6, -13, 1, 3, 5, 12 },
    { -3, -6, -8, -12, 2, 5, 7, 11 }, { -3, -7, -9, -11, 2, 6, 8, 10 }, { -4, -7, -8, -11, 3, 6, 7, 10 }, { -3, -5, -8, -11, 2, 4, 7, 10 },
    { -2, -6, -8, -10, 1, 5, 7, 9 }, { -2, -5, -8, -10, 1, 4, 7, 9 }, { -2, -4, -8, -10, 1, 3, 7, 9 }, { -2, -5, -7, -10, 1, 4, 6, 9 },
    { -3, -4, -7, -10, 2, 3, 6, 9 }, { -1, -2, -3, -10, 0, 1, 2, 9 }, { -4, -6, -8, -9, 3, 5, 7, 8 }, { -3, -5, -7, -9, 2, 4, 6, 8 } };
static int clampi(int v, int lo, int hi) { return v < lo ? lo : v > hi ? hi : v; }
static void put64(uint8_t out[8], uint64_t v) { for (int i = 0; i < 8; i++) out[i] = (uint8_t)(v >> (56 - 8 * i)); }   // blocks are stored most significant byte first

// ---------- ETC2 colour (ETC1 individual / differential) ----------
// texel (x, y) belongs to sub-block 0 when x < 2 (flip 0, side by side) or y < 2 (flip 1, one above the other); its index bits sit at pixel number x * 4 + y
static int etc_sub(int x, int y, int flip) { return flip ? y >= 2 : x >= 2; }
// the best table and indices for one sub-block around base colour c (8 bits a channel); returns the squared error
static int etc_fit(const uint8_t px[16][4], int flip, int s, const int c[3], int *table, uint32_t *msb, uint32_t *lsb) {
    int best = 1 << 30;
    for (int t = 0; t < 8; t++) {
        int err = 0; uint32_t m = 0, l = 0;
        for (int y = 0; y < 4; y++) for (int x = 0; x < 4; x++) {
            if (etc_sub(x, y, flip) != s) continue;
            const uint8_t *p = px[y * 4 + x]; int be = 1 << 30, bi = 0;
            for (int i = 0; i < 4; i++) {   // index (msb, lsb): 0 → +small, 1 → +large, 2 → -small, 3 → -large
                int mod = (i & 1 ? ETC_TAB[t][1] : ETC_TAB[t][0]) * (i & 2 ? -1 : 1), e = 0;
                for (int k = 0; k < 3; k++) { int d = clampi(c[k] + mod, 0, 255) - p[k]; e += d * d; }
                if (e < be) { be = e; bi = i; }
            }
            err += be; int n = x * 4 + y; if (bi & 2) m |= 1u << n; if (bi & 1) l |= 1u << n;
        }
        if (err < best) { best = err; *table = t; *msb = m; *lsb = l; }
    }
    return best;
}
void etc2_encode_rgb(const uint8_t px[16][4], uint8_t out[8]) {
    uint64_t best_blk = 0; long best = 1L << 40;
    for (int flip = 0; flip < 2; flip++) {
        int avg[2][3] = { { 0 } };
        for (int y = 0; y < 4; y++) for (int x = 0; x < 4; x++) for (int k = 0; k < 3; k++) avg[etc_sub(x, y, flip)][k] += px[y * 4 + x][k];
        for (int s = 0; s < 2; s++) for (int k = 0; k < 3; k++) avg[s][k] = (avg[s][k] + 4) / 8;
        for (int diff = 0; diff < 2; diff++) {
            int q[2][3], c[2][3], ok = 1;
            for (int s = 0; s < 2; s++) for (int k = 0; k < 3; k++) {
                if (diff) { q[s][k] = clampi((avg[s][k] * 31 + 127) / 255, 0, 31); c[s][k] = (q[s][k] << 3) | (q[s][k] >> 2); }
                else { q[s][k] = clampi((avg[s][k] * 15 + 127) / 255, 0, 15); c[s][k] = (q[s][k] << 4) | q[s][k]; }
            }
            if (diff) for (int k = 0; k < 3; k++) { int d = q[1][k] - q[0][k]; if (d < -4 || d > 3) ok = 0; }   // (the second colour as base + delta, inside 0..31)
            if (!ok) continue;
            int t0, t1; uint32_t m0, l0, m1, l1;
            long err = etc_fit(px, flip, 0, c[0], &t0, &m0, &l0) + etc_fit(px, flip, 1, c[1], &t1, &m1, &l1);
            if (err >= best) continue;
            uint64_t b = 0;
            if (diff) for (int k = 0; k < 3; k++) b |= (uint64_t)q[0][k] << (59 - 8 * k) | (uint64_t)((q[1][k] - q[0][k]) & 7) << (56 - 8 * k);
            else for (int k = 0; k < 3; k++) b |= (uint64_t)q[0][k] << (60 - 8 * k) | (uint64_t)q[1][k] << (56 - 8 * k);
            b |= (uint64_t)t0 << 37 | (uint64_t)t1 << 34 | (uint64_t)diff << 33 | (uint64_t)flip << 32 | (uint64_t)((m0 | m1) & 0xffff) << 16 | ((l0 | l1) & 0xffff);
            best = err; best_blk = b;
        }
    }
    put64(out, best_blk);
}

// ---------- EAC ----------
// one EAC block for 16 targets in the decoder's own range (0..255 for alpha, 0..2047 for R11): value(base, mult, mod) is the decoder's reconstruction
static void eac_encode(const int v[16], int r11, uint8_t out[8]) {
    int lo = v[0], hi = v[0]; for (int i = 1; i < 16; i++) { if (v[i] < lo) lo = v[i]; if (v[i] > hi) hi = v[i]; }
    long best = 1L << 40; uint64_t best_blk = 0; int scale = r11 ? 8 : 1, top = r11 ? 2047 : 255;
    for (int t = 0; t < 16; t++) {
        int tmin = EAC_TAB[t][3], tmax = EAC_TAB[t][7], span = tmax - tmin;
        int m0 = (hi - lo + (span * scale) / 2) / (span * scale); if (m0 < 1) m0 = 1;
        for (int m = m0 - 1; m <= m0 + 1; m++) {
            if (m < (r11 ? 0 : 1) || m > 15) continue;   // (ETC2 alpha: a multiplier of 0 is not a defined block)
            int unit = m ? m * scale : 1;                  // (R11 with multiplier 0: the modifier counts in 1 / 8 steps)
            int mid = (lo + hi) / 2 - ((tmin + tmax) * unit) / 2, b0 = r11 ? (mid - 4) / 8 : mid;
            for (int b = b0 - 1; b <= b0 + 1; b++) {
                if (b < 0 || b > 255) continue;
                long err = 0; uint64_t idx = 0;
                for (int p = 0; p < 16 && err < best; p++) {   // pixel p (column-major: x * 4 + y) of target v[y * 4 + x]
                    int x = p >> 2, y = p & 3, want = v[y * 4 + x], be = 1 << 30, bi = 0;
                    for (int i = 0; i < 8; i++) { int got = clampi((r11 ? b * 8 + 4 : b) + EAC_TAB[t][i] * unit, 0, top), d = got - want; d *= d; if (d < be) { be = d; bi = i; } }
                    err += be; idx |= (uint64_t)bi << (45 - 3 * p);
                }
                if (err < best) { best = err; best_blk = (uint64_t)b << 56 | (uint64_t)m << 52 | (uint64_t)t << 48 | idx; }
            }
        }
    }
    put64(out, best_blk);
}
void eac_encode_a8(const uint8_t v[16], uint8_t out[8]) { int t[16]; for (int i = 0; i < 16; i++) t[i] = v[i]; eac_encode(t, 0, out); }
void eac_encode_r11(const uint8_t v[16], uint8_t out[8]) { int t[16]; for (int i = 0; i < 16; i++) t[i] = (v[i] * 2047 + 127) / 255; eac_encode(t, 1, out); }

// ---------- whole images ----------
uint32_t bc_transcode_vk(int kind, int srgb) {
    switch (kind) {
    case 1: case 2: case 3: return srgb ? 152u : 151u;   // VK_FORMAT_ETC2_R8G8B8A8_SRGB_BLOCK / _UNORM_BLOCK
    case 4: return 153u;                                // VK_FORMAT_EAC_R11_UNORM_BLOCK
    case 5: return 155u;                                // VK_FORMAT_EAC_R11G11_UNORM_BLOCK
    default: return 0;
    }
}
int bc_transcode_block_bytes(int kind) { return kind == 4 ? 8 : 16; }
void bc_transcode_image(int kind, const uint8_t *src, size_t src_pitch, int w, int h, uint8_t *out, size_t out_pitch) {
    int ib = bc_block_bytes(kind), tb = bc_texel_bytes(kind), ob = bc_transcode_block_bytes(kind);
    for (int by = 0; by < (h + 3) / 4; by++) for (int bx = 0; bx < (w + 3) / 4; bx++) {
        uint8_t tex[16 * 4] = { 0 }; bc_decode_block(kind, src + by * src_pitch + bx * ib, tex, 4 * (size_t)tb);
        uint8_t *o = out + by * out_pitch + bx * ob;
        if (kind == 4 || kind == 5) {
            for (int c = 0; c < (kind == 5 ? 2 : 1); c++) { uint8_t v[16]; for (int i = 0; i < 16; i++) v[i] = tex[i * tb + c]; eac_encode_r11(v, o + 8 * c); }
        } else {
            uint8_t px[16][4], a[16]; for (int i = 0; i < 16; i++) { memcpy(px[i], tex + i * 4, 4); a[i] = tex[i * 4 + 3]; }
            eac_encode_a8(a, o); etc2_encode_rgb(px, o + 8);
        }
    }
}
