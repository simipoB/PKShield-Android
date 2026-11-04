// ARMv8 Cryptographic Extension instructions in software, for CPUs (and CPU translators) without them: the Android emulator's ARM translation on x86-64 has none, and
// Pokémon Violet's sdk hashes and decrypts with them from nninitStartup on (SHA-1 there; AES, SHA-256 and PMULL elsewhere: 1,100 of them in sdk + main). A SIGILL on one of
// these is answered here (crash.c on_crash → dm_crypto_emu): the instruction is computed as the Arm Architecture Reference Manual's pseudocode defines it on the vector
// registers saved in the signal frame, and the thread goes on at the next instruction. Phones have the extension in hardware and never come here.
//   SHA1C / SHA1P / SHA1M / SHA1SU0 / SHA256H / SHA256H2 / SHA256SU1   0101 1110 000 Rm 0 opc 00 Rn Rd
//   SHA1H / SHA1SU1 / SHA256SU0                                        0101 1110 0010 1000 0 opc 10 Rn Rd
//   AESE / AESD / AESMC / AESIMC                                        0100 1110 0010 1000 0 opc 10 Rn Rd
//   PMULL / PMULL2 (1Q: 64 x 64 → 128)                                 0 Q 00 1110 111 Rm 1110 00 Rn Rd
#include <stdint.h>
#include <string.h>

static uint32_t rol(uint32_t x, int n) { return (x << n) | (x >> (32 - n)); }
static uint32_t ror(uint32_t x, int n) { return (x >> n) | (x << (32 - n)); }

// ---- SHA-1 / SHA-256 (words are the lanes: w[0] = bits 31:0) ----
static void sha1_hash(uint32_t x[4], uint32_t *y, const uint32_t w[4], int op) {   // op 0 choose (SHA1C), 1 parity (SHA1P), 2 majority (SHA1M)
    for (int e = 0; e < 4; e++) {
        uint32_t b = x[1], c = x[2], d = x[3];
        uint32_t t = op == 0 ? (((c ^ d) & b) ^ d) : op == 1 ? (b ^ c ^ d) : ((b & c) | ((b | c) & d));
        *y = *y + rol(x[0], 5) + t + w[e];
        x[1] = rol(x[1], 30);
        uint32_t ny = x[3]; x[3] = x[2]; x[2] = x[1]; x[1] = x[0]; x[0] = *y; *y = ny;   // <Y, X> = ROL(Y : X, 32)
    }
}
static void sha256_hash(uint32_t x[4], uint32_t y[4], const uint32_t w[4]) {
    for (int e = 0; e < 4; e++) {
        uint32_t chs = (y[0] & y[1]) ^ (~y[0] & y[2]), maj = (x[0] & x[1]) ^ (x[0] & x[2]) ^ (x[1] & x[2]);
        uint32_t t = y[3] + (ror(y[0], 6) ^ ror(y[0], 11) ^ ror(y[0], 25)) + chs + w[e];
        x[3] = t + x[3];
        y[3] = t + (ror(x[0], 2) ^ ror(x[0], 13) ^ ror(x[0], 22)) + maj;
        uint32_t x3 = x[3], y3 = y[3];   // <Y, X> = ROL(Y : X, 32) over 256 bits
        x[3] = x[2]; x[2] = x[1]; x[1] = x[0]; x[0] = y3;
        y[3] = y[2]; y[2] = y[1]; y[1] = y[0]; y[0] = x3;
    }
}

// ---- AES (the state's byte i is row i % 4, column i / 4) ----
static uint8_t SBOX[256], ISBOX[256];
static uint8_t xt(uint8_t a) { return (uint8_t)((a << 1) ^ (a & 0x80 ? 0x1b : 0)); }
static uint8_t gmul(uint8_t a, uint8_t b) { uint8_t p = 0; while (b) { if (b & 1) p ^= a; a = xt(a); b >>= 1; } return p; }
static void aes_tables(void) {
    if (SBOX[0]) return;   // (S(0) = 0x63)
    for (int i = 0; i < 256; i++) {
        uint8_t inv = 0; if (i) for (int j = 1; j < 256; j++) if (gmul((uint8_t)i, (uint8_t)j) == 1) { inv = (uint8_t)j; break; }
        uint8_t s = inv; for (int k = 1; k < 5; k++) s ^= (uint8_t)((inv << k) | (inv >> (8 - k)));
        s ^= 0x63; SBOX[i] = s; ISBOX[s] = (uint8_t)i;
    }
}
static void mix(uint8_t s[16], int inv) {
    for (int c = 0; c < 4; c++) {
        uint8_t *a = s + 4 * c, b[4];
        for (int r = 0; r < 4; r++)
            b[r] = inv ? (uint8_t)(gmul(a[r], 14) ^ gmul(a[(r + 1) & 3], 11) ^ gmul(a[(r + 2) & 3], 13) ^ gmul(a[(r + 3) & 3], 9))
                       : (uint8_t)(gmul(a[r], 2) ^ gmul(a[(r + 1) & 3], 3) ^ a[(r + 2) & 3] ^ a[(r + 3) & 3]);
        memcpy(a, b, 4);
    }
}

// one instruction on a register file of 32 x 16 bytes (v[i] = Vi, little-endian lanes); 1 when it is one of these and was done (tests/crypto_emu_test.c runs it on a PC)
int dm_crypto_exec(uint32_t ins, unsigned char (*v)[16]) {
    int d = ins & 31, n = (ins >> 5) & 31, m = (ins >> 16) & 31;
    uint32_t vd[4], vn[4], vm[4], r[4]; memcpy(vd, v[d], 16); memcpy(vn, v[n], 16); memcpy(vm, v[m], 16);
    if ((ins & 0xFF208C00) == 0x5E000000) {   // three registers
        int op = (ins >> 12) & 7;
        if (op <= 2) { uint32_t y = vn[0]; memcpy(r, vd, 16); sha1_hash(r, &y, vm, op); }
        else if (op == 3) { uint32_t t[4] = { vd[2], vd[3], vn[0], vn[1] }; for (int i = 0; i < 4; i++) r[i] = t[i] ^ vd[i] ^ vm[i]; }   // SHA1SU0
        else if (op == 4 || op == 5) { uint32_t x[4], y[4]; memcpy(x, vd, 16); memcpy(y, vn, 16);   // SHA256H: hash(X = Qd, Y = Qn) → X; SHA256H2: hash(X = Qn, Y = Qd) → Y
            if (op == 4) sha256_hash(x, y, vm); else sha256_hash(y, x, vm);
            memcpy(r, x, 16); }
        else if (op == 6) {   // SHA256SU1
            uint32_t t0[4] = { vn[1], vn[2], vn[3], vm[0] };
            for (int e = 0; e < 2; e++) { uint32_t x = vm[2 + e]; r[e] = (ror(x, 17) ^ ror(x, 19) ^ (x >> 10)) + vd[e] + t0[e]; }
            for (int e = 2; e < 4; e++) { uint32_t x = r[e - 2]; r[e] = (ror(x, 17) ^ ror(x, 19) ^ (x >> 10)) + vd[e] + t0[e]; }
        } else return 0;
    } else if ((ins & 0xFFFE0C00) == 0x5E280800) {   // two registers: SHA1H, SHA1SU1, SHA256SU0
        int op = (ins >> 12) & 0x1f; memset(r, 0, 16);
        if (op == 0) r[0] = rol(vn[0], 30);
        else if (op == 1) { uint32_t t[4] = { vd[0] ^ vn[1], vd[1] ^ vn[2], vd[2] ^ vn[3], vd[3] }; for (int i = 0; i < 4; i++) r[i] = rol(t[i], 1); r[3] ^= rol(t[0], 2); }
        else if (op == 2) { uint32_t t[4] = { vd[1], vd[2], vd[3], vn[0] }; for (int e = 0; e < 4; e++) r[e] = (ror(t[e], 7) ^ ror(t[e], 18) ^ (t[e] >> 3)) + vd[e]; }
        else return 0;
    } else if ((ins & 0xFFFE0C00) == 0x4E280800) {   // AES
        int op = (ins >> 12) & 0x1f; uint8_t s[16], o[16]; aes_tables();
        if (op == 4 || op == 5) {
            uint8_t a[16], b[16]; memcpy(a, vd, 16); memcpy(b, vn, 16); for (int i = 0; i < 16; i++) s[i] = a[i] ^ b[i];
            for (int c = 0; c < 4; c++) for (int rr = 0; rr < 4; rr++) o[rr + 4 * c] = op == 4 ? SBOX[s[rr + 4 * ((c + rr) & 3)]] : ISBOX[s[rr + 4 * ((c - rr + 4) & 3)]];
        } else if (op == 6 || op == 7) { memcpy(o, vn, 16); mix(o, op == 7); }
        else return 0;
        memcpy(r, o, 16);
    } else if ((ins & 0xBFE0FC00) == 0x0EE0E000) {   // PMULL / PMULL2, 1Q
        uint64_t a, b; int hi = (ins >> 30) & 1; memcpy(&a, v[n] + 8 * hi, 8); memcpy(&b, v[m] + 8 * hi, 8);
        uint64_t lo = 0, up = 0; for (int i = 0; i < 64; i++) if (b >> i & 1) { lo ^= a << i; if (i) up ^= a >> (64 - i); }
        r[0] = (uint32_t)lo; r[1] = (uint32_t)(lo >> 32); r[2] = (uint32_t)up; r[3] = (uint32_t)(up >> 32);
    } else return 0;
    memcpy(v[d], r, 16);
    return 1;
}

#if defined(__aarch64__) && defined(__linux__)
#include <asm/sigcontext.h>
#include <stdio.h>
#include <ucontext.h>
#include <unistd.h>
static __uint128_t *vregs(ucontext_t *uc) {   // the FP / SIMD registers in the signal frame's extension records
    struct _aarch64_ctx *h = (struct _aarch64_ctx *)(void *)uc->uc_mcontext.__reserved;
    for (int n = 0; n < 64 && h->magic; n++) {
        if (h->magic == FPSIMD_MAGIC) return ((struct fpsimd_context *)h)->vregs;
        if (!h->size) break;
        h = (struct _aarch64_ctx *)(void *)((char *)h + h->size);
    }
    return NULL;
}

int dm_crypto_emu(ucontext_t *uc) {   // crash.c: a SIGILL on one of these instructions, done here; 1 = the thread goes on at the next instruction
    uint32_t ins; memcpy(&ins, (void *)(uintptr_t)uc->uc_mcontext.pc, 4);
    __uint128_t *v = vregs(uc);
    if (!v) {   // (once: what the frame holds instead, for the log)
        static int told; if (!told++) { char b[200]; struct _aarch64_ctx *h = (struct _aarch64_ctx *)(void *)uc->uc_mcontext.__reserved;
            int k = snprintf(b, sizeof b, "[dm] crypto_emu: no FP/SIMD record in the signal frame (first record magic %#x size %u)\n", h->magic, h->size); if (write(2, b, (size_t)k) < 0) { } }
        return 0;
    }
    if (!dm_crypto_exec(ins, (unsigned char (*)[16])(void *)v)) return 0;
    uc->uc_mcontext.pc += 4;
    return 1;
}
#endif
