// crypto_emu.c against known answers, on any machine: SHA-1 and SHA-256 of "abc" computed only through the emulated SHA instructions (the instruction sequences of Linux's
// arch/arm64/crypto sha1-ce / sha2-ce), AES-128 encryption and decryption of FIPS-197's example through AESE / AESMC / AESD / AESIMC, and PMULL / PMULL2 against a plain bit loop.
//   cc -O2 -o crypto_emu_test dm/host/tests/crypto_emu_test.c && ./crypto_emu_test
#include "../crypto_emu.c"
#include <stdio.h>
#include <stdlib.h>

static unsigned char V[32][16];
static void x(uint32_t ins) { if (!dm_crypto_exec(ins, V)) { printf("not an emulated instruction: %08x\n", ins); exit(1); } }
static uint32_t three(int op, int d, int n, int m) { return 0x5E000000u | (uint32_t)m << 16 | (uint32_t)op << 12 | (uint32_t)n << 5 | (uint32_t)d; }
static uint32_t two(int op, int d, int n) { return 0x5E280800u | (uint32_t)op << 12 | (uint32_t)n << 5 | (uint32_t)d; }
static uint32_t aes(int op, int d, int n) { return 0x4E280800u | (uint32_t)op << 12 | (uint32_t)n << 5 | (uint32_t)d; }
static void setw(int r, const uint32_t w[4]) { memcpy(V[r], w, 16); }
static void getv(int r, uint32_t w[4]) { memcpy(w, V[r], 16); }
static uint32_t be(const uint8_t *p) { return (uint32_t)p[0] << 24 | (uint32_t)p[1] << 16 | (uint32_t)p[2] << 8 | p[3]; }
static int fails;
static void check(const char *what, int ok) { printf("%-40s %s\n", what, ok ? "ok" : "FAIL"); fails += !ok; }

static void sha1_abc(void) {
    uint8_t blk[64] = { 'a', 'b', 'c', 0x80 }; blk[63] = 24;
    uint32_t G[20][4], st[5] = { 0x67452301, 0xefcdab89, 0x98badcfe, 0x10325476, 0xc3d2e1f0 }, K[4] = { 0x5a827999, 0x6ed9eba1, 0x8f1bbcdc, 0xca62c1d6 };
    for (int j = 0; j < 4; j++) for (int i = 0; i < 4; i++) G[j][i] = be(blk + 16 * j + 4 * i);
    for (int j = 4; j < 20; j++) { setw(0, G[j - 4]); setw(1, G[j - 3]); setw(2, G[j - 2]); setw(3, G[j - 1]); x(three(3, 0, 1, 2)); x(two(1, 0, 3)); getv(0, G[j]); }   // SHA1SU0, SHA1SU1
    uint32_t dg[4] = { st[0], st[1], st[2], st[3] }, e[4] = { st[4] }; setw(4, dg); setw(5, e);
    for (int j = 0; j < 20; j++) {
        uint32_t t[4]; for (int i = 0; i < 4; i++) t[i] = G[j][i] + K[j / 5]; setw(6, t);
        x(two(0, 7, 4));                                                    // SHA1H e_next, dg0
        x(three(j < 5 ? 0 : j < 10 ? 1 : j < 15 ? 2 : 1, 4, 5, 6));         // SHA1C / SHA1P / SHA1M dg0, e, t
        memcpy(V[5], V[7], 16);
    }
    uint32_t r[4], re[4]; getv(4, r); getv(5, re);
    uint32_t out[5] = { st[0] + r[0], st[1] + r[1], st[2] + r[2], st[3] + r[3], st[4] + re[0] }, want[5] = { 0xa9993e36, 0x4706816a, 0xba3e2571, 0x7850c26c, 0x9cd0d89d };
    check("SHA-1(\"abc\") through SHA1C/P/M/H/SU0/SU1", !memcmp(out, want, sizeof want));
}
static void sha256_abc(void) {
    static const uint32_t K[64] = { 0x428a2f98, 0x71374491, 0xb5c0fbcf, 0xe9b5dba5, 0x3956c25b, 0x59f111f1, 0x923f82a4, 0xab1c5ed5, 0xd807aa98, 0x12835b01, 0x243185be, 0x550c7dc3,
        0x72be5d74, 0x80deb1fe, 0x9bdc06a7, 0xc19bf174, 0xe49b69c1, 0xefbe4786, 0x0fc19dc6, 0x240ca1cc, 0x2de92c6f, 0x4a7484aa, 0x5cb0a9dc, 0x76f988da, 0x983e5152, 0xa831c66d,
        0xb00327c8, 0xbf597fc7, 0xc6e00bf3, 0xd5a79147, 0x06ca6351, 0x14292967, 0x27b70a85, 0x2e1b2138, 0x4d2c6dfc, 0x53380d13, 0x650a7354, 0x766a0abb, 0x81c2c92e, 0x92722c85,
        0xa2bfe8a1, 0xa81a664b, 0xc24b8b70, 0xc76c51a3, 0xd192e819, 0xd6990624, 0xf40e3585, 0x106aa070, 0x19a4c116, 0x1e376c08, 0x2748774c, 0x34b0bcb5, 0x391c0cb3, 0x4ed8aa4a,
        0x5b9cca4f, 0x682e6ff3, 0x748f82ee, 0x78a5636f, 0x84c87814, 0x8cc70208, 0x90befffa, 0xa4506ceb, 0xbef9a3f7, 0xc67178f2 };
    uint8_t blk[64] = { 'a', 'b', 'c', 0x80 }; blk[63] = 24;
    uint32_t G[16][4], st[8] = { 0x6a09e667, 0xbb67ae85, 0x3c6ef372, 0xa54ff53a, 0x510e527f, 0x9b05688c, 0x1f83d9ab, 0x5be0cd19 };
    for (int j = 0; j < 4; j++) for (int i = 0; i < 4; i++) G[j][i] = be(blk + 16 * j + 4 * i);
    for (int j = 4; j < 16; j++) { setw(0, G[j - 4]); setw(1, G[j - 3]); setw(2, G[j - 2]); setw(3, G[j - 1]); x(two(2, 0, 1)); x(three(6, 0, 2, 3)); getv(0, G[j]); }   // SHA256SU0, SHA256SU1
    setw(4, st); setw(5, st + 4);
    for (int j = 0; j < 16; j++) {
        uint32_t t[4]; for (int i = 0; i < 4; i++) t[i] = G[j][i] + K[4 * j + i]; setw(6, t);
        memcpy(V[7], V[4], 16);
        x(three(4, 4, 5, 6));   // SHA256H dg0, dg1, t
        x(three(5, 5, 7, 6));   // SHA256H2 dg1, dg2, t
    }
    uint32_t a[4], b[4]; getv(4, a); getv(5, b);
    uint32_t out[8], want[8] = { 0xba7816bf, 0x8f01cfea, 0x414140de, 0x5dae2223, 0xb00361a3, 0x96177a9c, 0xb410ff61, 0xf20015ad };
    for (int i = 0; i < 4; i++) { out[i] = st[i] + a[i]; out[4 + i] = st[4 + i] + b[i]; }
    check("SHA-256(\"abc\") through SHA256H/H2/SU0/SU1", !memcmp(out, want, sizeof want));
}
static void aes128(void) {
    uint8_t rk[11][16], pt[16], ct[16], want[16] = { 0x69, 0xc4, 0xe0, 0xd8, 0x6a, 0x7b, 0x04, 0x30, 0xd8, 0xcd, 0xb7, 0x80, 0x70, 0xb4, 0xc5, 0x5a };
    for (int i = 0; i < 16; i++) { rk[0][i] = (uint8_t)i; pt[i] = (uint8_t)(i * 0x11); }
    aes_tables(); uint8_t rc = 1;
    for (int r = 1; r < 11; r++) {   // key expansion (FIPS-197 5.2)
        uint8_t t[4] = { SBOX[rk[r - 1][13]], SBOX[rk[r - 1][14]], SBOX[rk[r - 1][15]], SBOX[rk[r - 1][12]] }; t[0] ^= rc; rc = xt(rc);
        for (int i = 0; i < 16; i++) { rk[r][i] = rk[r - 1][i] ^ (i < 4 ? t[i] : rk[r][i - 4]); }
    }
    memcpy(V[0], pt, 16);
    for (int r = 0; r < 9; r++) { memcpy(V[1], rk[r], 16); x(aes(4, 0, 1)); x(aes(6, 0, 0)); }   // AESE, AESMC
    memcpy(V[1], rk[9], 16); x(aes(4, 0, 1)); for (int i = 0; i < 16; i++) ct[i] = V[0][i] ^ rk[10][i];
    check("AES-128 FIPS-197 encrypt (AESE, AESMC)", !memcmp(ct, want, 16));
    memcpy(V[0], ct, 16); memcpy(V[1], rk[10], 16); x(aes(5, 0, 1)); x(aes(7, 0, 0));   // AESD, AESIMC
    for (int r = 9; r >= 2; r--) { memcpy(V[2], rk[r], 16); x(aes(7, 1, 2)); x(aes(5, 0, 1)); x(aes(7, 0, 0)); }
    memcpy(V[2], rk[1], 16); x(aes(7, 1, 2)); x(aes(5, 0, 1)); uint8_t back[16]; for (int i = 0; i < 16; i++) back[i] = V[0][i] ^ rk[0][i];
    check("AES-128 FIPS-197 decrypt (AESD, AESIMC)", !memcmp(back, pt, 16));
}
static void pmull(void) {
    uint64_t s = 0x9e3779b97f4a7c15ull; int ok = 1;
    for (int k = 0; k < 1000 && ok; k++) {
        uint64_t a[2], b[2]; for (int i = 0; i < 2; i++) { s ^= s << 13; s ^= s >> 7; s ^= s << 17; a[i] = s; s ^= s << 13; s ^= s >> 7; s ^= s << 17; b[i] = s; }
        memcpy(V[1], a, 16); memcpy(V[2], b, 16);
        for (int q = 0; q < 2; q++) {
            x(0x0EE0E000u | (uint32_t)q << 30 | 2u << 16 | 1u << 5 | 0u);
            uint64_t lo = 0, hi = 0; for (int i = 0; i < 64; i++) for (int j = 0; j < 64; j++) if ((a[q] >> i & 1) && (b[q] >> j & 1)) { int p = i + j; if (p < 64) lo ^= 1ull << p; else hi ^= 1ull << (p - 64); }
            uint64_t got[2]; memcpy(got, V[0], 16); ok &= got[0] == lo && got[1] == hi;
        }
    }
    check("PMULL / PMULL2 1Q (1000 random pairs)", ok);
}
int main(void) { sha1_abc(); sha256_abc(); aes128(); pmull(); printf("%s\n", fails ? "crypto_emu: FAILED" : "crypto_emu: all match"); return fails != 0; }
