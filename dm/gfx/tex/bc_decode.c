// BCn block decoding on the CPU (see bc_decode.h). Written from the format definitions (Direct3D 11 block compression, the BPTC extension for BC6H / BC7);
// checked block by block against an independent decoder (tests/bc_decode_check.py).
#include "bc_decode.h"
#include <string.h>

int bc_block_bytes(int kind) { return kind == 1 || kind == 4 || kind == -4 ? 8 : 16; }
int bc_texel_bytes(int kind) { return kind == 4 || kind == -4 ? 1 : kind == 5 || kind == -5 ? 2 : kind == 6 || kind == -6 ? 8 : 4; }

// ---------- BC1–BC5 ----------
static void rgb565(uint16_t c, uint8_t *o) {
    int r = c >> 11 & 31, g = c >> 5 & 63, b = c & 31;
    o[0] = (uint8_t)(r << 3 | r >> 2); o[1] = (uint8_t)(g << 2 | g >> 4); o[2] = (uint8_t)(b << 3 | b >> 2); o[3] = 255;
}
// the colour half of BC1 / BC2 / BC3: two RGB565 endpoints and 2-bit indices; BC2 / BC3 always use the four-colour mode
static void bc1_color(const uint8_t *b, uint8_t *out, size_t stride, int four) {
    uint8_t p[4][4]; uint16_t c0 = (uint16_t)(b[0] | b[1] << 8), c1 = (uint16_t)(b[2] | b[3] << 8);
    rgb565(c0, p[0]); rgb565(c1, p[1]);
    if (c0 > c1 || four) {
        for (int k = 0; k < 3; k++) { p[2][k] = (uint8_t)((2 * p[0][k] + p[1][k]) / 3); p[3][k] = (uint8_t)((p[0][k] + 2 * p[1][k]) / 3); }
        p[2][3] = p[3][3] = 255;
    } else {
        for (int k = 0; k < 3; k++) p[2][k] = (uint8_t)((p[0][k] + p[1][k]) / 2);
        p[2][3] = 255; memset(p[3], 0, 4);
    }
    uint32_t idx = (uint32_t)b[4] | (uint32_t)b[5] << 8 | (uint32_t)b[6] << 16 | (uint32_t)b[7] << 24;
    for (int i = 0; i < 16; i++) memcpy(out + (i >> 2) * stride + (i & 3) * 4, p[idx >> (2 * i) & 3], 4);
}
// one BC4 channel (BC3's alpha too): two 8-bit endpoints, 3-bit indices; out: every step bytes
static void bc4_channel(const uint8_t *b, int sgn, uint8_t *out, size_t stride, int step) {
    int v[8];
    if (!sgn) {
        int r0 = b[0], r1 = b[1]; v[0] = r0; v[1] = r1;
        if (r0 > r1) for (int i = 1; i <= 6; i++) v[i + 1] = ((7 - i) * r0 + i * r1) / 7;
        else { for (int i = 1; i <= 4; i++) v[i + 1] = ((5 - i) * r0 + i * r1) / 5; v[6] = 0; v[7] = 255; }
    } else {
        int raw0 = (int8_t)b[0], raw1 = (int8_t)b[1], r0 = raw0 == -128 ? -127 : raw0, r1 = raw1 == -128 ? -127 : raw1; v[0] = r0; v[1] = r1;
        if (raw0 > raw1) for (int i = 1; i <= 6; i++) v[i + 1] = ((7 - i) * r0 + i * r1) / 7;
        else { for (int i = 1; i <= 4; i++) v[i + 1] = ((5 - i) * r0 + i * r1) / 5; v[6] = -127; v[7] = 127; }
    }
    uint64_t idx = 0; for (int i = 0; i < 6; i++) idx |= (uint64_t)b[2 + i] << (8 * i);
    for (int i = 0; i < 16; i++) out[(i >> 2) * stride + (i & 3) * step] = (uint8_t)v[idx >> (3 * i) & 7];
}

// ---------- BC6H / BC7 shared ----------
typedef struct { const uint8_t *b; int pos; } bitrd;
static uint32_t rd(bitrd *s, int n) {
    uint32_t v = 0;
    for (int i = 0; i < n; i++) { int p = s->pos + i; v |= (uint32_t)(s->b[p >> 3] >> (p & 7) & 1) << i; }
    s->pos += n; return v;
}
// partition of a 4x4 block into 2 subsets: bit i = subset of texel i
static const uint16_t part2[64] = {
    0xCCCC, 0x8888, 0xEEEE, 0xECC8, 0xC880, 0xFEEC, 0xFEC8, 0xEC80, 0xC800, 0xFFEC, 0xFE80, 0xE800, 0xFFE8, 0xFF00, 0xFFF0, 0xF000,
    0xF710, 0x008E, 0x7100, 0x08CE, 0x008C, 0x7310, 0x3100, 0x8CCE, 0x088C, 0x3110, 0x6666, 0x366C, 0x17E8, 0x0FF0, 0x718E, 0x399C,
    0xAAAA, 0xF0F0, 0x5A5A, 0x33CC, 0x3C3C, 0x55AA, 0x9696, 0xA55A, 0x73CE, 0x13C8, 0x324C, 0x3BDC, 0x6996, 0xC33C, 0x9966, 0x0660,
    0x0272, 0x04E4, 0x4E40, 0x2720, 0xC936, 0x936C, 0x39C6, 0x639C, 0x9336, 0x9CC6, 0x817E, 0xE718, 0xCCF0, 0x0FCC, 0x7744, 0xEE22 };
// into 3 subsets: two bits per texel, texel 0 lowest
static const uint8_t part3[64][16] = {
    {0,0,1,1,0,0,1,1,0,2,2,1,2,2,2,2}, {0,0,0,1,0,0,1,1,2,2,1,1,2,2,2,1}, {0,0,0,0,2,0,0,1,2,2,1,1,2,2,1,1}, {0,2,2,2,0,0,2,2,0,0,1,1,0,1,1,1},
    {0,0,0,0,0,0,0,0,1,1,2,2,1,1,2,2}, {0,0,1,1,0,0,1,1,0,0,2,2,0,0,2,2}, {0,0,2,2,0,0,2,2,1,1,1,1,1,1,1,1}, {0,0,1,1,0,0,1,1,2,2,1,1,2,2,1,1},
    {0,0,0,0,0,0,0,0,1,1,1,1,2,2,2,2}, {0,0,0,0,1,1,1,1,1,1,1,1,2,2,2,2}, {0,0,0,0,1,1,1,1,2,2,2,2,2,2,2,2}, {0,0,1,2,0,0,1,2,0,0,1,2,0,0,1,2},
    {0,1,1,2,0,1,1,2,0,1,1,2,0,1,1,2}, {0,1,2,2,0,1,2,2,0,1,2,2,0,1,2,2}, {0,0,1,1,0,1,1,2,1,1,2,2,1,2,2,2}, {0,0,1,1,2,0,0,1,2,2,0,0,2,2,2,0},
    {0,0,0,1,0,0,1,1,0,1,1,2,1,1,2,2}, {0,1,1,1,0,0,1,1,2,0,0,1,2,2,0,0}, {0,0,0,0,1,1,2,2,1,1,2,2,1,1,2,2}, {0,0,2,2,0,0,2,2,0,0,2,2,1,1,1,1},
    {0,1,1,1,0,1,1,1,0,2,2,2,0,2,2,2}, {0,0,0,1,0,0,0,1,2,2,2,1,2,2,2,1}, {0,0,0,0,0,0,1,1,0,1,2,2,0,1,2,2}, {0,0,0,0,1,1,0,0,2,2,1,0,2,2,1,0},
    {0,1,2,2,0,1,2,2,0,0,1,1,0,0,0,0}, {0,0,1,2,0,0,1,2,1,1,2,2,2,2,2,2}, {0,1,1,0,1,2,2,1,1,2,2,1,0,1,1,0}, {0,0,0,0,0,1,1,0,1,2,2,1,1,2,2,1},
    {0,0,2,2,1,1,0,2,1,1,0,2,0,0,2,2}, {0,1,1,0,0,1,1,0,2,0,0,2,2,2,2,2}, {0,0,1,1,0,1,2,2,0,1,2,2,0,0,1,1}, {0,0,0,0,2,0,0,0,2,2,1,1,2,2,2,1},
    {0,0,0,0,0,0,0,2,1,1,2,2,1,2,2,2}, {0,2,2,2,0,0,2,2,0,0,1,2,0,0,1,1}, {0,0,1,1,0,0,1,2,0,0,2,2,0,2,2,2}, {0,1,2,0,0,1,2,0,0,1,2,0,0,1,2,0},
    {0,0,0,0,1,1,1,1,2,2,2,2,0,0,0,0}, {0,1,2,0,1,2,0,1,2,0,1,2,0,1,2,0}, {0,1,2,0,2,0,1,2,1,2,0,1,0,1,2,0}, {0,0,1,1,2,2,0,0,1,1,2,2,0,0,1,1},
    {0,0,1,1,1,1,2,2,2,2,0,0,0,0,1,1}, {0,1,0,1,0,1,0,1,2,2,2,2,2,2,2,2}, {0,0,0,0,0,0,0,0,2,1,2,1,2,1,2,1}, {0,0,2,2,1,1,2,2,0,0,2,2,1,1,2,2},
    {0,0,2,2,0,0,1,1,0,0,2,2,0,0,1,1}, {0,2,2,0,1,2,2,1,0,2,2,0,1,2,2,1}, {0,1,0,1,2,2,2,2,2,2,2,2,0,1,0,1}, {0,0,0,0,2,1,2,1,2,1,2,1,2,1,2,1},
    {0,1,0,1,0,1,0,1,0,1,0,1,2,2,2,2}, {0,2,2,2,0,1,1,1,0,2,2,2,0,1,1,1}, {0,0,0,2,1,1,1,2,0,0,0,2,1,1,1,2}, {0,0,0,0,2,1,1,2,2,1,1,2,2,1,1,2},
    {0,2,2,2,0,1,1,1,0,1,1,1,0,2,2,2}, {0,0,0,2,1,1,1,2,1,1,1,2,0,0,0,2}, {0,1,1,0,0,1,1,0,0,1,1,0,2,2,2,2}, {0,0,0,0,0,0,0,0,2,1,1,2,2,1,1,2},
    {0,1,1,0,0,1,1,0,2,2,2,2,2,2,2,2}, {0,0,2,2,0,0,1,1,0,0,1,1,0,0,2,2}, {0,0,2,2,1,1,2,2,1,1,2,2,0,0,2,2}, {0,0,0,0,0,0,0,0,0,0,0,0,2,1,1,2},
    {0,0,0,2,0,0,0,1,0,0,0,2,0,0,0,1}, {0,2,2,2,1,2,2,2,0,2,2,2,1,2,2,2}, {0,1,0,1,2,2,2,2,2,2,2,2,2,2,2,2}, {0,1,1,1,2,0,1,1,2,2,0,1,2,2,2,0} };
// anchor texels (their index has one bit less): of subset 1 in a 2-subset partition; of subsets 1 and 2 in a 3-subset partition
static const uint8_t anchor2[64] = {
    15,15,15,15,15,15,15,15, 15,15,15,15,15,15,15,15, 15, 2, 8, 2, 2, 8, 8,15, 2, 8, 2, 2, 8, 8, 2, 2,
    15,15, 6, 8, 2, 8,15,15, 2, 8, 2, 2, 2,15,15, 6, 6, 2, 6, 8,15,15, 2, 2, 15,15,15,15,15, 2, 2,15 };
static const uint8_t anchor3a[64] = {
     3, 3,15,15, 8, 3,15,15, 8, 8, 6, 6, 6, 5, 3, 3, 3, 3, 8,15, 3, 3, 6,10, 5, 8, 8, 6, 8, 5,15,15,
     8,15, 3, 5, 6,10, 8,15,15, 3,15, 5,15,15,15,15, 3,15, 5, 5, 5, 8, 5,10, 5,10, 8,13,15,12, 3, 3 };
static const uint8_t anchor3b[64] = {
    15, 8, 8, 3,15,15, 3, 8,15,15,15,15,15,15,15, 8,15, 8,15, 3,15, 8,15, 8, 3,15, 6,10,15,15,10, 8,
    15, 3,15,10,10, 8, 9,10, 6,15, 8,15, 3, 6, 6, 8,15, 3,15,15,15,15,15,15,15,15,15,15, 3,15,15, 8 };
static const uint8_t w2[4] = {0, 21, 43, 64}, w3[8] = {0, 9, 18, 27, 37, 46, 55, 64}, w4[16] = {0, 4, 9, 13, 17, 21, 26, 30, 34, 38, 43, 47, 51, 55, 60, 64};
static const uint8_t *weights(int bits) { return bits == 2 ? w2 : bits == 3 ? w3 : w4; }
static int subset_of(int ns, int part, int i) { return ns == 1 ? 0 : ns == 2 ? (part2[part] >> i & 1) : part3[part][i]; }
static int is_anchor(int ns, int part, int i) {
    return i == 0 || (ns == 2 && i == anchor2[part]) || (ns == 3 && (i == anchor3a[part] || i == anchor3b[part]));
}

// ---------- BC7 ----------
static void bc7(const uint8_t *b, uint8_t *out, size_t stride) {
    static const uint8_t NS[8] = {3, 2, 3, 2, 1, 1, 1, 2}, PB[8] = {4, 6, 6, 6, 0, 0, 0, 6}, RB[8] = {0, 0, 0, 0, 2, 2, 0, 0}, ISB[8] = {0, 0, 0, 0, 1, 0, 0, 0},
                         CB[8] = {4, 6, 5, 7, 5, 7, 7, 5}, AB[8] = {0, 0, 0, 0, 6, 8, 7, 5}, EPB[8] = {1, 0, 0, 1, 0, 0, 1, 1}, SPB[8] = {0, 1, 0, 0, 0, 0, 0, 0},
                         IB[8] = {3, 3, 2, 2, 2, 2, 4, 2}, IB2[8] = {0, 0, 0, 0, 3, 2, 0, 0};
    int m = 0; while (m < 8 && !(b[0] >> m & 1)) m++;
    if (m == 8) { for (int y = 0; y < 4; y++) memset(out + y * stride, 0, 16); return; }   // reserved mode: transparent black
    bitrd s = {b, m + 1};
    int ns = NS[m], part = (int)rd(&s, PB[m]), rot = (int)rd(&s, RB[m]), isb = (int)rd(&s, ISB[m]);
    int ep[6][4];
    for (int c = 0; c < 3; c++) for (int i = 0; i < 2 * ns; i++) ep[i][c] = (int)rd(&s, CB[m]);
    for (int i = 0; i < 2 * ns; i++) ep[i][3] = AB[m] ? (int)rd(&s, AB[m]) : 255;
    int pb[6] = {0};
    if (EPB[m]) for (int i = 0; i < 2 * ns; i++) pb[i] = (int)rd(&s, 1);
    if (SPB[m]) for (int k = 0; k < ns; k++) pb[2 * k] = pb[2 * k + 1] = (int)rd(&s, 1);
    int haspb = EPB[m] || SPB[m];
    for (int i = 0; i < 2 * ns; i++)
        for (int c = 0; c < 4; c++) {
            if (c == 3 && !AB[m]) continue;
            int bits = (c < 3 ? CB[m] : AB[m]) + haspb, v = haspb ? (ep[i][c] << 1 | pb[i]) : ep[i][c];
            v <<= 8 - bits; ep[i][c] = v | v >> bits;
        }
    int idx[16], idx2[16];
    for (int i = 0; i < 16; i++) idx[i] = (int)rd(&s, IB[m] - is_anchor(ns, part, i));
    if (IB2[m]) for (int i = 0; i < 16; i++) idx2[i] = (int)rd(&s, IB2[m] - (i == 0));
    for (int i = 0; i < 16; i++) {
        int sub = subset_of(ns, part, i); const int *e0 = ep[2 * sub], *e1 = ep[2 * sub + 1];
        int ci = idx[i], ai = idx[i], cb = IB[m], ab = IB[m];
        if (IB2[m]) { if (isb) { ci = idx2[i]; cb = IB2[m]; } else { ai = idx2[i]; ab = IB2[m]; } }
        int wc = weights(cb)[ci], wa = weights(ab)[ai]; uint8_t px[4];
        for (int c = 0; c < 3; c++) px[c] = (uint8_t)((e0[c] * (64 - wc) + e1[c] * wc + 32) >> 6);
        px[3] = (uint8_t)((e0[3] * (64 - wa) + e1[3] * wa + 32) >> 6);
        if (rot) { uint8_t t = px[3]; px[3] = px[rot - 1]; px[rot - 1] = t; }
        memcpy(out + (i >> 2) * stride + (i & 3) * 4, px, 4);
    }
}

// ---------- BC6H ----------
// Each mode's header as a list of fields read in order: endpoint value (0 r0 1 g0 2 b0 3 r1 4 g1 5 b1 6 r2 7 g2 8 b2 9 r3 10 g3 11 b3), first bit, bit count
// (a negative count reads that many bits in reverse order: the 12- and 16-bit one-region modes)
typedef struct { uint8_t v, lo; int8_t n; } fld;
#define F(v, lo, n) {v, lo, n}
static const fld L1[] = { F(7,4,1),F(8,4,1),F(11,4,1),F(0,0,10),F(1,0,10),F(2,0,10),F(3,0,5),F(10,4,1),F(7,0,4),F(4,0,5),F(11,0,1),F(10,0,4),F(5,0,5),F(11,1,1),F(8,0,4),F(6,0,5),F(11,2,1),F(9,0,5),F(11,3,1),{255,0,0} };
static const fld L2[] = { F(7,5,1),F(10,4,1),F(10,5,1),F(0,0,7),F(11,0,1),F(11,1,1),F(8,4,1),F(1,0,7),F(8,5,1),F(11,2,1),F(7,4,1),F(2,0,7),F(11,3,1),F(11,5,1),F(11,4,1),F(3,0,6),F(7,0,4),F(4,0,6),F(10,0,4),F(5,0,6),F(8,0,4),F(6,0,6),F(9,0,6),{255,0,0} };
static const fld L3[] = { F(0,0,10),F(1,0,10),F(2,0,10),F(3,0,5),F(0,10,1),F(7,0,4),F(4,0,4),F(1,10,1),F(11,0,1),F(10,0,4),F(5,0,4),F(2,10,1),F(11,1,1),F(8,0,4),F(6,0,5),F(11,2,1),F(9,0,5),F(11,3,1),{255,0,0} };
static const fld L4[] = { F(0,0,10),F(1,0,10),F(2,0,10),F(3,0,4),F(0,10,1),F(10,4,1),F(7,0,4),F(4,0,5),F(1,10,1),F(10,0,4),F(5,0,4),F(2,10,1),F(11,1,1),F(8,0,4),F(6,0,4),F(11,0,1),F(11,2,1),F(9,0,4),F(7,4,1),F(11,3,1),{255,0,0} };
static const fld L5[] = { F(0,0,10),F(1,0,10),F(2,0,10),F(3,0,4),F(0,10,1),F(8,4,1),F(7,0,4),F(4,0,4),F(1,10,1),F(11,0,1),F(10,0,4),F(5,0,5),F(2,10,1),F(8,0,4),F(6,0,4),F(11,1,1),F(11,2,1),F(9,0,4),F(11,4,1),F(11,3,1),{255,0,0} };
static const fld L6[] = { F(0,0,9),F(8,4,1),F(1,0,9),F(7,4,1),F(2,0,9),F(11,4,1),F(3,0,5),F(10,4,1),F(7,0,4),F(4,0,5),F(11,0,1),F(10,0,4),F(5,0,5),F(11,1,1),F(8,0,4),F(6,0,5),F(11,2,1),F(9,0,5),F(11,3,1),{255,0,0} };
static const fld L7[] = { F(0,0,8),F(10,4,1),F(8,4,1),F(1,0,8),F(11,2,1),F(7,4,1),F(2,0,8),F(11,3,1),F(11,4,1),F(3,0,6),F(7,0,4),F(4,0,5),F(11,0,1),F(10,0,4),F(5,0,5),F(11,1,1),F(8,0,4),F(6,0,6),F(9,0,6),{255,0,0} };
static const fld L8[] = { F(0,0,8),F(11,0,1),F(8,4,1),F(1,0,8),F(7,5,1),F(7,4,1),F(2,0,8),F(10,5,1),F(11,4,1),F(3,0,5),F(10,4,1),F(7,0,4),F(4,0,6),F(10,0,4),F(5,0,5),F(11,1,1),F(8,0,4),F(6,0,5),F(11,2,1),F(9,0,5),F(11,3,1),{255,0,0} };
static const fld L9[] = { F(0,0,8),F(11,1,1),F(8,4,1),F(1,0,8),F(8,5,1),F(7,4,1),F(2,0,8),F(11,5,1),F(11,4,1),F(3,0,5),F(10,4,1),F(7,0,4),F(4,0,5),F(11,0,1),F(10,0,4),F(5,0,6),F(8,0,4),F(6,0,5),F(11,2,1),F(9,0,5),F(11,3,1),{255,0,0} };
static const fld L10[] = { F(0,0,6),F(10,4,1),F(11,0,1),F(11,1,1),F(8,4,1),F(1,0,6),F(7,5,1),F(8,5,1),F(11,2,1),F(7,4,1),F(2,0,6),F(10,5,1),F(11,3,1),F(11,5,1),F(11,4,1),F(3,0,6),F(7,0,4),F(4,0,6),F(10,0,4),F(5,0,6),F(8,0,4),F(6,0,6),F(9,0,6),{255,0,0} };
static const fld L11[] = { F(0,0,10),F(1,0,10),F(2,0,10),F(3,0,10),F(4,0,10),F(5,0,10),{255,0,0} };
static const fld L12[] = { F(0,0,10),F(1,0,10),F(2,0,10),F(3,0,9),F(0,10,1),F(4,0,9),F(1,10,1),F(5,0,9),F(2,10,1),{255,0,0} };
static const fld L13[] = { F(0,0,10),F(1,0,10),F(2,0,10),F(3,0,8),F(0,10,-2),F(4,0,8),F(1,10,-2),F(5,0,8),F(2,10,-2),{255,0,0} };
static const fld L14[] = { F(0,0,10),F(1,0,10),F(2,0,10),F(3,0,4),F(0,10,-6),F(4,0,4),F(1,10,-6),F(5,0,4),F(2,10,-6),{255,0,0} };
#undef F
// per mode (1..14): layout, regions, transformed (deltas), endpoint bits, delta bits r g b
static const struct { const fld *l; uint8_t regions, xf, eb, db[3]; } BC6M[15] = {
    {0, 0, 0, 0, {0}}, {L1, 2, 1, 10, {5, 5, 5}}, {L2, 2, 1, 7, {6, 6, 6}}, {L3, 2, 1, 11, {5, 4, 4}}, {L4, 2, 1, 11, {4, 5, 4}}, {L5, 2, 1, 11, {4, 4, 5}},
    {L6, 2, 1, 9, {5, 5, 5}}, {L7, 2, 1, 8, {6, 5, 5}}, {L8, 2, 1, 8, {5, 6, 5}}, {L9, 2, 1, 8, {5, 5, 6}}, {L10, 2, 0, 6, {6, 6, 6}},
    {L11, 1, 0, 10, {10, 10, 10}}, {L12, 1, 1, 11, {9, 9, 9}}, {L13, 1, 1, 12, {8, 8, 8}}, {L14, 1, 1, 16, {4, 4, 4}} };
static int sext(int v, int bits) { return v & (1 << (bits - 1)) ? v - (1 << bits) : v; }
static int unq(int v, int bits, int sgn) {
    if (!sgn) {
        if (bits >= 15 || v == 0) return v;
        if (v == (1 << bits) - 1) return 0xFFFF;
        return ((v << 16) + 0x8000) >> bits;
    }
    if (bits >= 16) return v;
    int neg = v < 0; if (neg) v = -v; int u;
    if (v == 0) u = 0; else if (v >= (1 << (bits - 1)) - 1) u = 0x7FFF; else u = ((v << 15) + 0x4000) >> (bits - 1);
    return neg ? -u : u;
}
static uint16_t finish(int c, int sgn) {
    if (!sgn) return (uint16_t)((c * 31) >> 6);
    return c < 0 ? (uint16_t)(0x8000 | ((-c * 31) >> 5)) : (uint16_t)((c * 31) >> 5);
}
static void bc6h(const uint8_t *b, int sgn, uint8_t *out, size_t stride) {
    bitrd s = {b, 0}; int mb = (int)rd(&s, 2), mode;
    if (mb < 2) mode = mb + 1;
    else {
        static const int8_t m5[32] = { [2] = 3, [6] = 4, [10] = 5, [14] = 6, [18] = 7, [22] = 8, [26] = 9, [30] = 10, [3] = 11, [7] = 12, [11] = 13, [15] = 14 };
        mb |= (int)rd(&s, 3) << 2; mode = m5[mb];
    }
    if (!mode) { for (int y = 0; y < 4; y++) memset(out + y * stride, 0, 32); return; }   // reserved: black
    int e[12] = {0};
    for (const fld *f = BC6M[mode].l; f->v != 255; f++) {
        if (f->n > 0) e[f->v] |= (int)rd(&s, f->n) << f->lo;
        else for (int k = -f->n - 1; k >= 0; k--) e[f->v] |= (int)rd(&s, 1) << (f->lo + k);   // highest bit first
    }
    int ns = BC6M[mode].regions, part = ns == 2 ? (int)rd(&s, 5) : 0, eb = BC6M[mode].eb;
    // endpoints: e[0..2] base, then r1 g1 b1 / r2 g2 b2 / r3 g3 b3; transformed modes hold signed deltas from the base
    int ep[4][3];
    for (int c = 0; c < 3; c++) {
        ep[0][c] = sgn ? sext(e[c], eb) : e[c];
        for (int k = 1; k < 2 * ns; k++) {
            int v = e[3 * k + c];
            if (BC6M[mode].xf) { v = sext(v, BC6M[mode].db[c]); v = (e[c] + v) & ((1 << eb) - 1); if (sgn) v = sext(v, eb); }
            else if (sgn) v = sext(v, eb);
            ep[k][c] = v;
        }
        for (int k = 0; k < 2 * ns; k++) ep[k][c] = unq(ep[k][c], eb, sgn);
    }
    int ib = ns == 2 ? 3 : 4; const uint8_t *w = weights(ib);
    for (int i = 0; i < 16; i++) {
        int sub = subset_of(ns, part, i), ix = (int)rd(&s, ib - is_anchor(ns, part, i));
        uint16_t px[4];
        for (int c = 0; c < 3; c++) px[c] = finish((ep[2 * sub][c] * (64 - w[ix]) + ep[2 * sub + 1][c] * w[ix] + 32) >> 6, sgn);
        px[3] = 0x3C00;   // 1.0
        memcpy(out + (i >> 2) * stride + (i & 3) * 8, px, 8);
    }
}

void bc_decode_block(int kind, const uint8_t *b, uint8_t *out, size_t stride) {
    switch (kind) {
    case 1: bc1_color(b, out, stride, 0); break;
    case 2: bc1_color(b + 8, out, stride, 1);
            for (int i = 0; i < 16; i++) out[(i >> 2) * stride + (i & 3) * 4 + 3] = (uint8_t)((b[i >> 1] >> (4 * (i & 1)) & 15) * 17);
            break;
    case 3: bc1_color(b + 8, out, stride, 1); bc4_channel(b, 0, out + 3, stride, 4); break;
    case 4: case -4: bc4_channel(b, kind < 0, out, stride, 1); break;
    case 5: case -5: bc4_channel(b, kind < 0, out, stride, 2); bc4_channel(b + 8, kind < 0, out + 1, stride, 2); break;
    case 6: case -6: bc6h(b, kind < 0, out, stride); break;
    case 7: bc7(b, out, stride); break;
    }
}

void bc_decode_image(int kind, const uint8_t *src, size_t src_pitch, int w, int h, uint8_t *out, size_t out_pitch) {
    int bb = bc_block_bytes(kind), tb = bc_texel_bytes(kind); uint8_t tmp[4 * 4 * 8];
    for (int by = 0; by < (h + 3) / 4; by++)
        for (int bx = 0; bx < (w + 3) / 4; bx++) {
            const uint8_t *blk = src + by * src_pitch + (size_t)bx * bb;
            int full = bx * 4 + 4 <= w && by * 4 + 4 <= h;
            if (full) { bc_decode_block(kind, blk, out + (size_t)by * 4 * out_pitch + (size_t)bx * 4 * tb, out_pitch); continue; }
            bc_decode_block(kind, blk, tmp, (size_t)4 * tb);
            for (int y = 0; y < 4 && by * 4 + y < h; y++)
                for (int x = 0; x < 4 && bx * 4 + x < w; x++) memcpy(out + (size_t)(by * 4 + y) * out_pitch + (size_t)(bx * 4 + x) * tb, tmp + (y * 4 + x) * tb, (size_t)tb);
        }
}
