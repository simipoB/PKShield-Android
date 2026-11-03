// BC → ETC2 / EAC / ASTC transcoding (etc_encode.h): each BC block decoded to texels (bc_decode.c), then encoded again, block rows split over worker threads.
// ETC2 colour: ETC1's individual and differential modes (valid ETC2: a differential pair is kept inside 0..31, which ETC2 would read as its T / H / planar
// modes otherwise), both sub-block splits and all 8 intensity tables tried, the least squared error kept. EAC: per modifier table, the multiplier and base
// around the block's range, each texel's nearest modifier. ASTC 4x4 (BC7): one partition, endpoints along the block's principal axis refined by least squares.
// Checked against an independent decoder (dm/gfx/tex/tests/etc_check.py)
#include "etc_encode.h"
#include "bc_decode.h"
#include <math.h>
#include <pthread.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

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
// one EAC block for 16 targets in the decoder's own range: mode 0 alpha (0..255), 1 R11 (0..2047), 2 R11 signed (-1023..1023)
static void eac_encode(const int v[16], int mode, uint8_t out[8]) {
    int lo = v[0], hi = v[0]; for (int i = 1; i < 16; i++) { if (v[i] < lo) lo = v[i]; if (v[i] > hi) hi = v[i]; }
    long best = 1L << 40; uint64_t best_blk = 0; int r11 = mode != 0, scale = r11 ? 8 : 1, bot = mode == 2 ? -1023 : 0, top = mode == 1 ? 2047 : mode == 2 ? 1023 : 255;
    int bmin = mode == 2 ? -127 : 0, bmax = mode == 2 ? 127 : 255, off = mode == 1 ? 4 : 0;   // (signed: the base is a two's-complement byte, no +4)
    for (int t = 0; t < 16; t++) {
        int tmin = EAC_TAB[t][3], tmax = EAC_TAB[t][7], span = tmax - tmin;
        int m0 = (hi - lo + (span * scale) / 2) / (span * scale); if (m0 < 1) m0 = 1;
        for (int m = m0 - 1; m <= m0 + 1; m++) {
            if (m < (r11 ? 0 : 1) || m > 15) continue;   // (ETC2 alpha: a multiplier of 0 is not a defined block)
            int unit = m ? m * scale : 1;                  // (R11 with multiplier 0: the modifier counts in 1 / 8 steps)
            int mid = (lo + hi) / 2 - ((tmin + tmax) * unit) / 2, b0 = r11 ? (int)floor((mid - off) / 8.0 + 0.5) : mid;
            for (int b = b0 - 1; b <= b0 + 1; b++) {
                if (b < bmin || b > bmax) continue;
                long err = 0; uint64_t idx = 0;
                for (int p = 0; p < 16 && err < best; p++) {   // pixel p (column-major: x * 4 + y) of target v[y * 4 + x]
                    int x = p >> 2, y = p & 3, want = v[y * 4 + x], be = 1 << 30, bi = 0;
                    for (int i = 0; i < 8; i++) { int got = clampi((r11 ? b * 8 + off : b) + EAC_TAB[t][i] * unit, bot, top), d = got - want; d *= d; if (d < be) { be = d; bi = i; } }
                    err += be; idx |= (uint64_t)bi << (45 - 3 * p);
                }
                if (err < best) { best = err; best_blk = (uint64_t)(uint8_t)b << 56 | (uint64_t)m << 52 | (uint64_t)t << 48 | idx; }
            }
        }
    }
    put64(out, best_blk);
}
void eac_encode_a8(const uint8_t v[16], uint8_t out[8]) { int t[16]; for (int i = 0; i < 16; i++) t[i] = v[i]; eac_encode(t, 0, out); }
void eac_encode_r11(const uint8_t v[16], uint8_t out[8]) { int t[16]; for (int i = 0; i < 16; i++) t[i] = (v[i] * 2047 + 127) / 255; eac_encode(t, 1, out); }
void eac_encode_r11s(const int8_t v[16], uint8_t out[8]) {
    int t[16]; for (int i = 0; i < 16; i++) { int s = v[i] < -127 ? -127 : v[i]; t[i] = (s * 1023 + (s < 0 ? -63 : 63)) / 127; } eac_encode(t, 2, out);
}

// ---------- ASTC 4x4 (LDR, one partition, one weight plane) ----------
// opaque blocks: RGB direct endpoints (CEM 8), 8 weight levels (3 bits); others: RGBA direct (CEM 12), 6 weight levels (a trit and a bit). Both leave room for
// endpoints at the full 8 bits (the decoder takes the finest endpoint range that fits what is left), so the endpoints need no quantization table
static const int W8[8] = { 0, 9, 18, 27, 37, 46, 55, 64 }, W6[6] = { 0, 64, 12, 52, 25, 39 };   // the decoder's weights for each coded value
static uint8_t TRIT_ENC[243]; static pthread_once_t trit_once = PTHREAD_ONCE_INIT;
static void trit_init(void) {   // the inverse of the specification's trit-block decode, built by decoding every 8-bit code
    for (int T = 255; T >= 0; T--) {
        int t[5], C;
        if (((T >> 2) & 7) == 7) { C = ((T >> 5) & 7) << 2 | (T & 3); t[4] = t[3] = 2; }
        else { C = T & 31; if (((T >> 5) & 3) == 3) { t[4] = 2; t[3] = T >> 7 & 1; } else { t[4] = T >> 7 & 1; t[3] = T >> 5 & 3; } }
        if ((C & 3) == 3) { t[2] = 2; t[1] = C >> 4 & 1; t[0] = (C >> 3 & 1) << 1 | ((C >> 2 & 1) & ~(C >> 3) & 1); }
        else if (((C >> 2) & 3) == 3) { t[2] = 2; t[1] = 2; t[0] = C & 3; }
        else { t[2] = C >> 4 & 1; t[1] = C >> 2 & 3; t[0] = (C >> 1 & 1) << 1 | ((C & 1) & ~(C >> 1) & 1); }
        TRIT_ENC[t[0] + 3 * t[1] + 9 * t[2] + 27 * t[3] + 81 * t[4]] = (uint8_t)T;   // (counting down: the smallest code wins)
    }
}
static void bits_put(uint8_t blk[16], int pos, int n, uint32_t v, int rev) {   // n bits at pos (LSB first); rev: the weight stream, which runs down from bit 127
    for (int i = 0; i < n; i++) if (v >> i & 1) { int b = rev ? 127 - (pos + i) : pos + i; blk[b >> 3] |= (uint8_t)(1u << (b & 7)); }
}
// error of the block for endpoints e[2][4] with each texel's nearest weight (written to w); nc channels, lv the weight table
static long astc_fit(const uint8_t px[16][4], int nc, const int e[2][4], const int *lv, int nl, int w[16]) {
    long err = 0;
    for (int i = 0; i < 16; i++) {
        long be = 1L << 40; int bi = 0;
        for (int j = 0; j < nl; j++) {
            long d2 = 0;
            for (int c = 0; c < nc; c++) { int got = ((e[0][c] * 257) * (64 - lv[j]) + (e[1][c] * 257) * lv[j] + 32) >> 14, d = got - px[i][c]; d2 += d * d; }
            if (d2 < be) { be = d2; bi = j; }
        }
        err += be; w[i] = bi;
    }
    return err;
}
void astc_encode_rgba(const uint8_t px[16][4], uint8_t out[16]) {
    int alpha = 0; for (int i = 0; i < 16; i++) alpha |= px[i][3] != 255;
    int nc = alpha ? 4 : 3, nl = alpha ? 6 : 8; const int *lv = alpha ? W6 : W8;
    double mean[4] = { 0 }, cov[4][4] = { { 0 } }, ax[4] = { 1, 1, 1, 1 };
    for (int i = 0; i < 16; i++) for (int c = 0; c < nc; c++) mean[c] += px[i][c] / 16.0;
    for (int i = 0; i < 16; i++) for (int a = 0; a < nc; a++) for (int b = 0; b < nc; b++) cov[a][b] += (px[i][a] - mean[a]) * (px[i][b] - mean[b]);
    for (int it = 0; it < 8; it++) {   // the principal axis, by power iteration
        double n[4] = { 0 }, l = 0; for (int a = 0; a < nc; a++) for (int b = 0; b < nc; b++) n[a] += cov[a][b] * ax[b];
        for (int a = 0; a < nc; a++) l += n[a] * n[a];
        if (l < 1e-9) break;
        l = sqrt(l); for (int a = 0; a < nc; a++) ax[a] = n[a] / l;
    }
    double tlo = 0, thi = 0; for (int i = 0; i < 16; i++) { double t = 0; for (int c = 0; c < nc; c++) t += (px[i][c] - mean[c]) * ax[c]; if (t < tlo) tlo = t; if (t > thi) thi = t; }
    int e[2][4] = { { 0, 0, 0, 255 }, { 0, 0, 0, 255 } }, w[16], be[2][4], bw[16];
    for (int c = 0; c < nc; c++) { e[0][c] = clampi((int)lround(mean[c] + tlo * ax[c]), 0, 255); e[1][c] = clampi((int)lround(mean[c] + thi * ax[c]), 0, 255); }
    long best = astc_fit(px, nc, e, lv, nl, w); memcpy(be, e, sizeof e); memcpy(bw, w, sizeof w);
    for (int it = 0; it < 2; it++) {   // least squares endpoints for the chosen weights, then the weights again
        double s00 = 0, s01 = 0, s11 = 0, r0[4] = { 0 }, r1[4] = { 0 };
        for (int i = 0; i < 16; i++) { double f = lv[w[i]] / 64.0, g = 1 - f; s00 += g * g; s01 += g * f; s11 += f * f; for (int c = 0; c < nc; c++) { r0[c] += g * px[i][c]; r1[c] += f * px[i][c]; } }
        double det = s00 * s11 - s01 * s01; if (fabs(det) < 1e-6) break;
        for (int c = 0; c < nc; c++) { e[0][c] = clampi((int)lround((r0[c] * s11 - r1[c] * s01) / det), 0, 255); e[1][c] = clampi((int)lround((r1[c] * s00 - r0[c] * s01) / det), 0, 255); }
        long err = astc_fit(px, nc, e, lv, nl, w); if (err >= best) break;
        best = err; memcpy(be, e, sizeof e); memcpy(bw, w, sizeof w);
    }
    if (be[1][0] + be[1][1] + be[1][2] < be[0][0] + be[0][1] + be[0][2]) {   // (the decoder reads that order as blue contraction: the endpoints swap, the weights turn round)
        for (int c = 0; c < 4; c++) { int t = be[0][c]; be[0][c] = be[1][c]; be[1][c] = t; }
        for (int i = 0; i < 16; i++) bw[i] = alpha ? bw[i] ^ 1 : 7 - bw[i];
    }
    memset(out, 0, 16);
    bits_put(out, 0, 11, alpha ? 0x43 : 0x53, 0);   // block mode: a 4x4 weight grid, weight range 6 (0x43) or 8 (0x53), one plane
    bits_put(out, 13, 4, alpha ? 12 : 8, 0);        // (bits 11-12: one partition) the endpoint mode
    for (int c = 0; c < nc; c++) { bits_put(out, 17 + 16 * c, 8, (uint32_t)be[0][c], 0); bits_put(out, 25 + 16 * c, 8, (uint32_t)be[1][c], 0); }
    if (!alpha) { for (int i = 0; i < 16; i++) bits_put(out, 3 * i, 3, (uint32_t)bw[i], 1); return; }
    pthread_once(&trit_once, trit_init);
    static const int seq[10][2] = { { -1, 0 }, { 0, 2 }, { -1, 1 }, { 2, 2 }, { -1, 2 }, { 4, 1 }, { -1, 3 }, { 5, 2 }, { -1, 4 }, { 7, 1 } };
    for (int g = 0, pos = 0; g < 4; g++) {   // groups of five: a trit code T spread between the values' low bits (m0 T1:0 m1 T3:2 m2 T4 m3 T6:5 m4 T7), cut at 42 bits
        int t = 0, k = 1, m[5] = { 0 };
        for (int j = 0; j < 5 && g * 5 + j < 16; j++) { int v = bw[g * 5 + j]; m[j] = v & 1; t += (v >> 1) * k; k *= 3; }
        k = TRIT_ENC[t];
        for (int q = 0; q < 10 && pos < 42; q++) {
            if (seq[q][0] < 0) bits_put(out, pos++, 1, (uint32_t)m[seq[q][1]], 1);
            else { int n = seq[q][1]; if (pos + n > 42) n = 42 - pos; bits_put(out, pos, n, (uint32_t)(k >> seq[q][0]), 1); pos += n; }
        }
    }
}

// ---------- whole images ----------
uint32_t bc_transcode_vk(int kind, int srgb, int astc) {
    switch (kind) {
    case 1: case 2: case 3: return srgb ? 152u : 151u;   // VK_FORMAT_ETC2_R8G8B8A8_SRGB_BLOCK / _UNORM_BLOCK
    case 4: return 153u; case -4: return 154u;          // VK_FORMAT_EAC_R11_UNORM_BLOCK / _SNORM_BLOCK
    case 5: return 155u; case -5: return 156u;          // VK_FORMAT_EAC_R11G11_UNORM_BLOCK / _SNORM_BLOCK
    case 6: return 122u;                                // VK_FORMAT_B10G11R11_UFLOAT_PACK32 (texels, not blocks: half of RGBA16F)
    case 7: return astc ? (srgb ? 158u : 157u) : 0;     // VK_FORMAT_ASTC_4x4_SRGB_BLOCK / _UNORM_BLOCK
    default: return 0;                                  // (BC6H signed: no compact format every GPU samples keeps negative values; it stays RGBA16F)
    }
}
int bc_transcode_blocks(int kind) { return kind != 6; }
int bc_transcode_bytes(int kind) { return kind == 6 ? 4 : kind == 4 || kind == -4 ? 8 : 16; }
static uint32_t h2uf(uint16_t h, int mb) {   // half float → unsigned float with mb mantissa bits (6 or 5), rounded; negative → 0
    if (h & 0x8000) return 0;
    if ((h >> 10 & 31) == 31) return (31u << mb) | (h & 0x3ff ? 1 : 0);
    uint32_t v = ((uint32_t)h + (1u << (9 - mb))) >> (10 - mb);
    return v >> mb >= 31 ? (30u << mb) | ((1u << mb) - 1) : v;
}
typedef struct { int kind, mode; const uint8_t *src; size_t sp; int w, h; uint8_t *out; size_t op; int y0, y1; } job_t;   // mode 0 decode, 1 transcode; block rows y0..y1
static void job_rows(const job_t *j) {
    if (!j->mode) {
        int y = j->y0 * 4, h = (j->y1 * 4 < j->h ? j->y1 * 4 : j->h) - y;
        if (h > 0) bc_decode_image(j->kind, j->src + j->y0 * j->sp, j->sp, j->w, h, j->out + y * j->op, j->op);
        return;
    }
    int kind = j->kind, ib = bc_block_bytes(kind), tb = bc_texel_bytes(kind), ob = bc_transcode_bytes(kind);
    for (int by = j->y0; by < j->y1; by++) for (int bx = 0; bx < (j->w + 3) / 4; bx++) {
        uint8_t tex[16 * 8] = { 0 }; bc_decode_block(kind, j->src + by * j->sp + bx * ib, tex, 4 * (size_t)tb);
        uint8_t *o = j->out + by * j->op + bx * ob;
        switch (kind) {
        case 4: case 5: for (int c = 0; c < kind - 3; c++) { uint8_t v[16]; for (int i = 0; i < 16; i++) v[i] = tex[i * tb + c]; eac_encode_r11(v, o + 8 * c); } break;
        case -4: case -5: for (int c = 0; c < -kind - 3; c++) { int8_t v[16]; for (int i = 0; i < 16; i++) v[i] = (int8_t)tex[i * tb + c]; eac_encode_r11s(v, o + 8 * c); } break;
        case 6:   // texel rows: block row by is texel rows 4 by .. 4 by + 3 (out pitch: one texel row)
            for (int y = 0; y < 4 && by * 4 + y < j->h; y++) for (int x = 0; x < 4 && bx * 4 + x < j->w; x++) {
                const uint8_t *t = tex + (y * 4 + x) * 8; uint16_t r = (uint16_t)(t[0] | t[1] << 8), g = (uint16_t)(t[2] | t[3] << 8), b = (uint16_t)(t[4] | t[5] << 8);
                uint32_t p = h2uf(r, 6) | h2uf(g, 6) << 11 | h2uf(b, 5) << 22; memcpy(j->out + (size_t)(by * 4 + y) * j->op + (size_t)(bx * 4 + x) * 4, &p, 4);
            }
            break;
        case 7: { uint8_t px[16][4]; memcpy(px, tex, 64); astc_encode_rgba(px, o); break; }
        default: { uint8_t px[16][4], a[16]; for (int i = 0; i < 16; i++) { memcpy(px[i], tex + i * 4, 4); a[i] = tex[i * 4 + 3]; } eac_encode_a8(a, o); etc2_encode_rgb(px, o + 8); }
        }
    }
}
static void *job_run(void *a) { job_rows(a); return NULL; }
int bc_threads(void) {   // worker threads for one image: DM_VK_TRANSCODE_THREADS, else half the cores (the game keeps the rest), at most 8
    static int n; if (n) return n;
    const char *e = getenv("DM_VK_TRANSCODE_THREADS"); long c = sysconf(_SC_NPROCESSORS_ONLN);
    int v = e && atoi(e) > 0 ? atoi(e) : (int)(c / 2); n = v < 1 ? 1 : v > 8 ? 8 : v; return n;
}
static void run_image(int kind, int mode, const uint8_t *src, size_t sp, int w, int h, uint8_t *out, size_t op) {
    int rows = (h + 3) / 4, n = bc_threads(); long blocks = (long)rows * ((w + 3) / 4);
    if (blocks < 1024) n = 1;   // (small images: a thread costs more than it saves)
    if (n > rows) n = rows > 0 ? rows : 1;
    if (mode && kind == 7) pthread_once(&trit_once, trit_init);
    job_t j[8]; pthread_t t[8]; int started[8] = { 0 };
    for (int i = 0; i < n; i++) j[i] = (job_t){ kind, mode, src, sp, w, h, out, op, rows * i / n, rows * (i + 1) / n };
    for (int i = 1; i < n; i++) started[i] = pthread_create(&t[i], NULL, job_run, &j[i]) == 0;
    job_rows(&j[0]);
    for (int i = 1; i < n; i++) { if (started[i]) pthread_join(t[i], NULL); else job_rows(&j[i]); }   // (a thread that could not start: its rows done here)
}
void bc_transcode_image(int kind, const uint8_t *src, size_t sp, int w, int h, uint8_t *out, size_t op) { run_image(kind, 1, src, sp, w, h, out, op); }
void bc_decode_image_mt(int kind, const uint8_t *src, size_t sp, int w, int h, uint8_t *out, size_t op) { run_image(kind, 0, src, sp, w, h, out, op); }
