// The game's own Oodle decoder, for the offline shader tools. Unreal Engine games compress each shader in their shader code archives with Oodle (Octopath Traveler 0: every entry of the
// two .ushaderbytecode files in pakchunk0-Switch.pak, block header 8C 0A = Mermaid), and Game Freak's Trinity engine every file of its packs (Pokémon Legends: Z-A: arc/data.trpfs, compression
// type 3). Shaders are translated offline only and Oodle's decoder is not open source, so they are decoded on the Mac with the copy statically linked into main, in one of two modes:
// DM_OODLE_JOB=<job file> runs OodleLZ_Decompress on every record right before nnMain, writes <job file>.out and exits (the NSP script, Unreal);
// DM_OODLE_SERVE=1 answers requests on stdin / stdout from the guest main thread before the SDK starts, until stdin ends (shader_split.py's workers, Trinity: the packs are decoded file by file
// while they are scanned, about 4.7 GB, so they are never written out). Request: u64 compressed length, u64 decompressed length, the compressed bytes; answer: u64 bytes decoded (0 on failure),
// then those bytes.
// Job file: "DMOJ", u32 count, then per record u32 compressed length, u32 raw length and the compressed bytes. Output: "DMOR", u32 count, then per record u32 decoded length
// (0 = failed) and the decoded bytes.
#include "hle.h"
#include <unistd.h>

// OodleLZ_Decompress is found in main by the name its profiler scope passes: the one function whose code takes the address of the NUL-terminated string "OodleLZ_Decompress" (adrp + add),
// with its start from the module's .eh_frame_hdr. The string belongs to the Oodle library itself, so the rule holds whatever the engine: Octopath Traveler 0 1.0.5 (Unreal) main+0x4f77e10,
// Pokemon Legends: Z-A 2.0.2 main+0x1a9c9e0, each the only function referencing it (twice)
static uint64_t eh_read(const uint8_t *b, uint64_t *o, int enc, uint64_t ehs) {   // one .eh_frame_hdr value (formats udata4 / sdata4 / udata8, relative to itself or to the table)
    uint64_t v; int fmt = enc & 0xF, app = enc & 0x70, at = (int)*o;
    if (fmt == 0x3) { uint32_t x; memcpy(&x, b + at, 4); v = x; *o += 4; }
    else if (fmt == 0xB) { int32_t x; memcpy(&x, b + at, 4); v = (uint64_t)(int64_t)x; *o += 4; }
    else if (fmt == 0x4) { memcpy(&v, b + at, 8); *o += 8; }
    else die("DM_OODLE_JOB: .eh_frame_hdr encoding %#x not handled", enc);
    return app == 0x10 ? v + (uint64_t)at : app == 0x30 ? v + ehs : v;
}
static uint64_t func_start(const module_t *m, uint64_t off) {   // start of the function holding off (the table is sorted by start)
    const uint8_t *b = m->base; uint32_t m0; int32_t eh; memcpy(&m0, b + 4, 4); memcpy(&eh, b + m0 + 16, 4);
    if (memcmp(b + m0, "MOD0", 4)) die("DM_OODLE_JOB: main has no MOD0");
    uint64_t ehs = m0 + (int64_t)eh, o = ehs + 4; int penc = b[ehs + 1], cenc = b[ehs + 2], tenc = b[ehs + 3];
    eh_read(b, &o, penc, ehs); uint64_t n = eh_read(b, &o, cenc, ehs), best = 0;
    for (uint64_t i = 0; i < n; i++) { uint64_t s = eh_read(b, &o, tenc, ehs); eh_read(b, &o, tenc, ehs); if (s <= off && s > best) best = s; }
    return best;
}
static uint64_t oodle_find(const module_t *m) {
    static const char nm[] = "OodleLZ_Decompress";   // sizeof includes the terminating NUL
    const uint8_t *b = m->base, *ro = b + m->ro_off, *re = b + m->data_off;
    uint64_t strs[8]; int ns = 0;
    for (const uint8_t *p = ro; ns < 8 && (p = memmem(p, (size_t)(re - p), nm, sizeof nm)); p += sizeof nm) if (p[-1] == 0) strs[ns++] = (uint64_t)(p - b);
    if (!ns) die("DM_OODLE_JOB: main has no \"OodleLZ_Decompress\" string: no Oodle decoder to find");
    uint64_t fn = 0; int nfn = 0;
    for (uint64_t pc = 0; pc + 4 <= m->text_end; pc += 4) {
        uint32_t w; memcpy(&w, b + pc, 4);
        if ((w & 0x9F000000) != 0x90000000) continue;   // adrp xd, page
        int64_t imm = (int64_t)(((w >> 29) & 3) | (((w >> 5) & 0x7FFFF) << 2)); if (imm & (1 << 20)) imm -= 1 << 21;
        uint64_t page = (pc & ~0xFFFull) + (uint64_t)(imm << 12);
        for (int k = 1; k < 8 && pc + 4 * k + 4 <= m->text_end; k++) {   // the add that completes it, within a few instructions
            uint32_t w2; memcpy(&w2, b + pc + 4 * k, 4);
            if ((w2 & 0xFF800000) != 0x91000000 || ((w2 >> 5) & 31) != (w & 31)) continue;   // add xd, xn = xd of the adrp, #imm
            uint64_t a = page + ((w2 >> 10) & 0xFFF);
            for (int s = 0; s < ns; s++) if (a == strs[s]) { uint64_t f = func_start(m, pc + 4 * k); if (f != fn) { fn = f; nfn++; } }
            break;
        }
    }
    if (nfn != 1) die("DM_OODLE_JOB: %d functions reference \"OodleLZ_Decompress\" in main (expected exactly 1)", nfn);
    return fn;
}

// OodleLZ_Decompress(compBuf, compBufSize, rawBuf, rawLen, fuzzSafe, checkCRC, verbosity, decBufBase, decBufSize, fpCallback, callbackUserData, decoderMemory, decoderMemorySize, threadPhase).
// Arguments 9-14 go on the stack; all are 64-bit here so the Apple and AAPCS64 stack layouts agree (Apple packs smaller stack arguments)
typedef int64_t (*oodle_fn)(const void *, int64_t, void *, int64_t, int64_t, int64_t, int64_t, void *, int64_t, void *, void *, void *, int64_t, int64_t);

static uint32_t rd32(FILE *f) { uint32_t v = 0; if (fread(&v, 4, 1, f) != 1) die("DM_OODLE_JOB: the job file ends early"); return v; }

void dm_oodle_job_maybe(void) {
    const char *job = getenv("DM_OODLE_JOB");
    if (!job || !*job) return;
    oodle_fn fn = NULL; uint64_t off = 0;
    for (int i = 0; i < g_nmod; i++) if (!strcmp(g_mod[i].name, "main")) { off = oodle_find(&g_mod[i]); fn = (oodle_fn)(void *)(g_mod[i].base + off); }
    if (!fn) die("DM_OODLE_JOB: no main module");
    LOG("DM_OODLE_JOB: OodleLZ_Decompress at main+%#llx", (unsigned long long)off);
    FILE *in = fopen(job, "rb"); if (!in) die("DM_OODLE_JOB: cannot open %s", job);
    if (rd32(in) != 0x4a4f4d44) die("DM_OODLE_JOB: %s is not a job file", job);
    uint32_t n = rd32(in);
    char outp[1024]; snprintf(outp, sizeof outp, "%s.out", job);
    FILE *out = fopen(outp, "wb"); if (!out) die("DM_OODLE_JOB: cannot write %s", outp);
    uint32_t hdr[2] = {0x524f4d44, n}; fwrite(hdr, 4, 2, out);
    size_t scratch_n = 16u << 20; void *scratch = malloc(scratch_n);   // the decoder's work memory, so Oodle never calls its allocator
    uint32_t bad = 0; uint64_t raw_total = 0;
    for (uint32_t i = 0; i < n; i++) {
        uint32_t cl = rd32(in), rl = rd32(in);
        uint8_t *c = malloc(cl ? cl : 1), *r = malloc(rl ? rl : 1);
        if (fread(c, 1, cl, in) != cl) die("DM_OODLE_JOB: record %u ends early", i);
        int64_t got = fn(c, cl, r, rl, 1, 0, 0, NULL, 0, NULL, NULL, scratch, (int64_t)scratch_n, 3);   // fuzz safe, no CRC check, silent, unthreaded
        uint32_t ol = got == (int64_t)rl ? rl : 0;
        if (!ol && bad++ < 8) LOG("[RED] DM_OODLE_JOB: record %u (%u → %u bytes) failed (decoder returned %lld)", i, cl, rl, (long long)got);
        fwrite(&ol, 4, 1, out); if (ol) fwrite(r, 1, ol, out);
        raw_total += ol; free(c); free(r);
    }
    fclose(in); fclose(out); free(scratch);
    LOG("DM_OODLE_JOB: %u records decoded with the game's OodleLZ_Decompress (%u failed, %.1f MB out) → %s", n - bad, bad, raw_total / 1048576.0, outp);
    fflush(stderr); _exit(bad ? 1 : 0);
}

static int read_all(int fd, void *p, size_t n) {
    for (uint8_t *q = p; n; ) { ssize_t k = read(fd, q, n); if (k <= 0) return 0; q += k; n -= (size_t)k; }
    return 1;
}
static void write_all(int fd, const void *p, size_t n) {
    for (const uint8_t *q = p; n; ) { ssize_t k = write(fd, q, n); if (k <= 0) die("DM_OODLE_SERVE: answer write failed"); q += k; n -= (size_t)k; }
}
void dm_oodle_serve_maybe(void) {
    if (!getenv("DM_OODLE_SERVE")) return;
    oodle_fn fn = NULL; uint64_t off = 0;
    for (int i = 0; i < g_nmod; i++) if (!strcmp(g_mod[i].name, "main")) { off = oodle_find(&g_mod[i]); fn = (oodle_fn)(void *)(g_mod[i].base + off); }
    if (!fn) die("DM_OODLE_SERVE: no main module");
    enum { SLACK = 0x10000 };
    size_t scratch_n = 16u << 20; uint8_t *scratch = malloc(scratch_n), *comp = NULL, *raw = NULL; size_t comp_cap = 0, raw_cap = 0; uint64_t n = 0, bad = 0;   // the decoder's work memory, as for a job
    if (!scratch) die("DM_OODLE_SERVE: decoder memory");
    LOG("DM_OODLE_SERVE: OodleLZ_Decompress at main+%#llx, requests on stdin", (unsigned long long)off);
    for (;;) {
        uint64_t hdr[2];
        if (!read_all(0, hdr, sizeof hdr)) break;
        if (hdr[0] > (1ull << 32) || hdr[1] > (1ull << 32)) die("DM_OODLE_SERVE: request too large (%#llx → %#llx)", (unsigned long long)hdr[0], (unsigned long long)hdr[1]);
        if (hdr[0] > comp_cap) { free(comp); comp_cap = hdr[0]; if (!(comp = malloc(comp_cap))) die("DM_OODLE_SERVE: input buffer"); }
        if (hdr[1] + SLACK > raw_cap) { free(raw); raw_cap = hdr[1] + SLACK; if (!(raw = malloc(raw_cap))) die("DM_OODLE_SERVE: output buffer"); }
        if (!read_all(0, comp, hdr[0])) die("DM_OODLE_SERVE: request cut short");
        int64_t got = fn(comp, (int64_t)hdr[0], raw, (int64_t)hdr[1], 1, 0, 0, NULL, 0, NULL, NULL, scratch, (int64_t)scratch_n, 3);   // fuzz safe, no CRC check, silent, unthreaded
        uint64_t out = got == (int64_t)hdr[1] ? (uint64_t)got : 0;
        if (!out && hdr[1] && bad++ < 8) LOG("[RED] DM_OODLE_SERVE: request %llu (%llu → %llu bytes) failed (decoder returned %lld)", (unsigned long long)n, (unsigned long long)hdr[0], (unsigned long long)hdr[1], (long long)got);
        write_all(1, &out, sizeof out);
        if (out) write_all(1, raw, out);
        n++;
    }
    LOG("DM_OODLE_SERVE: %llu requests, %llu failed", (unsigned long long)n, (unsigned long long)bad);
    fflush(stderr); _exit(bad ? 1 : 0);
}
