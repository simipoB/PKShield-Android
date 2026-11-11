// Multi-module loading (playing the role of Horizon's rtld): mapping, claiming thread slots, gate registration, cross-module relocation, take-over by name, segment permissions.
// The code area (original text and patch area) is set to read-only+execute as soon as it is copied in, and from then on the loader only writes data: on iOS these two blocks must be embedded as is as signed code segments.
#include "dm.h"
#include <TargetConditionals.h>
#include <fcntl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>
#include <mach-o/getsect.h>
#include <mach-o/ldsyms.h>
#include "hle.h"

module_t g_mod[MAX_MOD];
int g_nmod;
pthread_key_t g_key18, g_keytls, g_keytlr, g_keygate;
enum { KEY_K18, KEY_KTLS, KEY_KTLR, KEY_KGATE, KEY_N };

uint64_t resolve_import(module_t *m, const char *name);
int hle_implements(const char *name);
void gate_register(uint64_t ret_addr, const char *kind, int arg, uint64_t orig);

static pthread_key_t key_value(unsigned i) {
    switch (i) {
    case KEY_K18: return g_key18; case KEY_KTLS: return g_keytls; case KEY_KTLR: return g_keytlr;
    default: return g_keygate;
    }
}
static void create_keys(void) {
    if (pthread_key_create(&g_key18, NULL) || pthread_key_create(&g_keytls, NULL) ||
        pthread_key_create(&g_keytlr, NULL) || pthread_key_create(&g_keygate, NULL)) die("cannot create the host key");
    LOG("key numbers K18=%lu KTLS=%lu KTLR=%lu KGATE=%lu", g_key18, g_keytls, g_keytlr, g_keygate);
}

#ifdef __ANDROID__
// Android: the slot block of this thread, published in bionic's application TLS slot (TLS_SLOT_APP = 2, tpidr_el0 + 16; bionic leaves it to the app) on first use.
// Entry 0 stays unused so that no key page entry is 0 (install_key_page's check)
static __thread uint64_t g_slot_blk[1 + KEY_N];
static uint64_t *slot_block(void) {
    uint64_t *tp; __asm__ volatile("mrs %0, tpidr_el0" : "=r"(tp));
    if (tp[2] != (uint64_t)(uintptr_t)g_slot_blk) {
        if (tp[2]) die("bionic's application TLS slot is already in use (%#llx)", (unsigned long long)tp[2]);
        tp[2] = (uint64_t)(uintptr_t)g_slot_blk;
    }
    return g_slot_blk;
}
void dm_slot_set(int slot, void *v) { slot_block()[1 + slot] = (uint64_t)(uintptr_t)v; }
static void tsd_check(void) {   // the key page entry of K18 must lead patched code to this thread's K18 slot
    dm_slot_set(DM_SLOT_K18, (void *)0x1234);
    uint64_t *tp; __asm__ volatile("mrs %0, tpidr_el0" : "=r"(tp));
    uint64_t off = *(uint64_t *)(g_mod[0].base + g_mod[0].key_page_off + g_mod[0].key_idx[KEY_K18] * 8);
    if (((uint64_t *)(uintptr_t)tp[2])[off / 8] != 0x1234) die("thread slot layout mismatch");
    dm_slot_set(DM_SLOT_K18, 0);
}
#else
void dm_slot_set(int slot, void *v) { pthread_setspecific(key_value((unsigned)slot), v); }
// K slot check: the pthread TSD layout is fixed, slot = tpidrro_el0 + 8*key
static void tsd_check(void) {
    pthread_setspecific(g_key18, (void *)0x1234);
    uint64_t base; __asm__ volatile("mrs %0, tpidrro_el0" : "=r"(base));
    uint64_t off = *(uint64_t *)(g_mod[0].base + g_mod[0].key_page_off + g_mod[0].key_idx[KEY_K18] * 8);
    if (((uint64_t *)(base & ~7ull))[off / 8] != 0x1234) die("TSD layout mismatch");
    pthread_setspecific(g_key18, 0);
}
#endif

static void build_symhash(module_t *m);
static void parse_dynamic(module_t *m) {
    uint32_t mod0 = *(uint32_t *)(m->base + 4);
    if (memcmp(m->base + mod0, "MOD0", 4)) die("%s: no MOD0", m->name);
    m->dyn = (const Elf64_Dyn *)(m->base + mod0 + *(int32_t *)(m->base + mod0 + 4));
    for (const Elf64_Dyn *d = m->dyn; d->d_tag; d++) switch (d->d_tag) {
        case DT_SYMTAB: m->symtab = (const Elf64_Sym *)(m->base + d->d_un.d_ptr); break;
        case DT_STRTAB: m->strtab = (const char *)(m->base + d->d_un.d_ptr); break;
        case DT_INIT: m->init = d->d_un.d_ptr; break;
        case DT_INIT_ARRAY: m->init_array = d->d_un.d_ptr; break;
        case DT_INIT_ARRAYSZ: m->init_arraysz = d->d_un.d_val; break;
    }
    m->nsym = (unsigned)(((const uint8_t *)m->strtab - (const uint8_t *)m->symtab) / sizeof(Elf64_Sym));
    build_symhash(m);
}

static void *module_sym_linear(module_t *m, const char *name) {
    for (unsigned i = 1; i < m->nsym; i++) {
        const Elf64_Sym *s = &m->symtab[i];
        if (s->st_shndx && !strcmp(m->strtab + s->st_name, name)) return m->base + s->st_value;
    }
    return NULL;
}
// Symbol name hash table: inserted in ascending index order, not overwritten if the same name is already there ⇒ lookup results are identical, entry by entry, to a linear scan (the first defined symbol with that name).
// Of the 91–99 ms of ① host loading, relocation + forced take-over took 77–86 ms, mainly because every by-name relocation scanned the symbol tables of two modules linearly
static uint32_t sym_hash(const char *s) { uint32_t h = 2166136261u; while (*s) h = (h ^ (uint8_t)*s++) * 16777619u; return h; }
static void build_symhash(module_t *m) {
    unsigned cap = 16; while (cap < m->nsym * 2) cap <<= 1;
    m->symh = calloc(cap, sizeof *m->symh); m->symh_mask = cap - 1;
    for (unsigned i = 1; i < m->nsym; i++) {
        const Elf64_Sym *s = &m->symtab[i]; if (!s->st_shndx) continue;
        const char *nm = m->strtab + s->st_name;
        for (unsigned j = sym_hash(nm) & m->symh_mask;; j = (j + 1) & m->symh_mask) {
            if (!m->symh[j]) { m->symh[j] = i; break; }
            if (!strcmp(m->strtab + m->symtab[m->symh[j]].st_name, nm)) break;   // The same name already exists with a smaller index: keep that one
        }
    }
}
static unsigned long g_symchk_n, g_symchk_bad;
void *module_sym(module_t *m, const char *name) {
    unsigned long nq; (void)nq;
    if (!m->symh) return module_sym_linear(m, name);
    void *r = NULL;
    for (unsigned j = sym_hash(name) & m->symh_mask; m->symh[j]; j = (j + 1) & m->symh_mask) {
        const Elf64_Sym *s = &m->symtab[m->symh[j]];
        if (!strcmp(m->strtab + s->st_name, name)) { r = m->base + s->st_value; break; }
    }
    static int chk = -1; if (chk < 0) chk = getenv("DM_SYMCHECK") != NULL;
    if (chk) { void *l = module_sym_linear(m, name); g_symchk_n++; nq = g_symchk_n;
               if (l != r) { g_symchk_bad++; LOG("[RED] DM_SYMCHECK: %s in %s hash %p ≠ linear %p", name, m->name, r, l); } }
    return r;
}
void *guest_sym(const char *name) {
    for (int i = 0; i < g_nmod; i++) { void *p = module_sym(&g_mod[i], name); if (p) return p; }
    return NULL;
}
// For modules with the patch area before the base (patch_off < 0) addresses in the patch area count too, as for modules whose patch area follows the image (crash reports, the GC pause's "in guest code" test)
module_t *module_of(const void *p) {
    for (int i = 0; i < g_nmod; i++) {
        const uint8_t *b = g_mod[i].base;
        if ((const uint8_t *)p >= b && (const uint8_t *)p < b + g_mod[i].size) return &g_mod[i];
        if (g_mod[i].patch_off < 0 && (const uint8_t *)p >= b + g_mod[i].patch_off && (const uint8_t *)p < b + g_mod[i].patch_off + g_mod[i].patch_len) return &g_mod[i];
    }
    return NULL;
}

// SDK major version, from the version string the SDK's libraries keep in the sdk module's read-only data ("NintendoSDK_nnSdk-10_4_0"; SDK 12 spells it NintendoSdk_).
// Behaviour that follows the SDK version is decided by it (the NVN API version, the shared-font layout), not by the game profile. 0 = no sdk module or no version string
int dm_sdk_major(void) {
    static int major = -1;
    if (major >= 0) return major;
    major = 0;
    for (int i = 0; i < g_nmod; i++) if (!strcmp(g_mod[i].name, "sdk")) {
        const char *b = (const char *)g_mod[i].base + g_mod[i].ro_off, *e = (const char *)g_mod[i].base + g_mod[i].key_page_off;   // read-only data, data and bss
        for (const char *p = b; (p = memmem(p, (size_t)(e - p), "_nnSdk-", 7)); p += 7)
            if (p - b >= 11 && !strncasecmp(p - 11, "NintendoSDK", 11) && p[7] >= '0' && p[7] <= '9') {
                major = atoi(p + 7); LOG("sdk: NintendoSDK nnSdk-%.12s (major %d)", p + 7, major); break; }
    }
    if (!major) LOG("[RED] sdk: no SDK version string in the sdk module; SDK-dependent behaviour takes the newer SDKs' side");
    return major;
}

// Cross-module symbol resolution: first look for a definition in the loaded modules, and only if not found hand over to HLE / stubs
static uint64_t resolve_sym(module_t *m, const char *name) {
    for (int i = 0; i < g_nmod; i++) { void *p = module_sym(&g_mod[i], name); if (p) return (uint64_t)p; }
    return resolve_import(m, name);
}

// Forced take-over table (hle.h HLE_FORCE, the __DATA,swforce section): a hit overrides the definition inside the module
static const hle_force_ent *g_force; static unsigned g_nforce; static unsigned *g_force_hits;
static void force_init(void) {
    unsigned long sz; g_force = (const hle_force_ent *)getsectiondata(&_mh_execute_header, "__DATA", "swforce", &sz);
    g_nforce = g_force ? (unsigned)(sz / sizeof *g_force) : 0;
    g_force_hits = calloc(g_nforce + 1, sizeof *g_force_hits);
    for (unsigned i = 0; i < g_nforce; i++)
        for (unsigned j = i + 1; j < g_nforce; j++)
            if (!strcmp(g_force[i].name, g_force[j].name)) die("duplicate entry in the forced takeover list: %s", g_force[i].name);
}
// "soft crypto" takeovers (hle_softcrypto.c) only on a CPU without the ARMv8 Cryptographic Extension (the Android emulator's ARM translation); DM_SOFT_CRYPTO=1 forces them
#if defined(__linux__) && defined(__aarch64__)
#include <sys/auxv.h>
static int soft_crypto(void) {
    static int on = -1;
    if (on < 0) { const char *e = getenv("DM_SOFT_CRYPTO"); unsigned long hw = getauxval(AT_HWCAP), need = (1ul << 5) | (1ul << 6) | (1ul << 3);   // HWCAP_SHA1 | HWCAP_SHA2 | HWCAP_AES
                  on = e ? atoi(e) != 0 : (hw & need) != need; if (on) LOG("soft crypto: the CPU lacks SHA1 / SHA2 / AES instructions (hwcap %#lx): nn::crypto's SHA blocks run in C", hw); }
    return on;
}
#else
static int soft_crypto(void) { const char *e = getenv("DM_SOFT_CRYPTO"); return e && atoi(e); }
#endif
static void *force_lookup(const char *name) {
    for (unsigned i = 0; i < g_nforce; i++)
        if (!strcmp(g_force[i].name, name)) {
            if (!strncmp(g_force[i].why, "soft crypto", 11) && !soft_crypto()) return NULL;   // (the SDK's own hardware code)
            if (!g_force_hits[i]++ && g_flag.imports) LOG("force %s  (%s)", name, g_force[i].why);
            void *hlecount_wrap(const char *, const char *, void *); return hlecount_wrap("FORCE", name, g_force[i].fn);   // A counting stub when DM_HLECOUNT is set
        }
    return NULL;
}
// Generic host: the list serves several games, so entries a game does not reference are only noted; with DM_FORCE_STRICT=1 it still fails loudly (for checking a mistyped list)
static void force_check(void) {
    unsigned unused = 0; int strict = getenv("DM_FORCE_STRICT") != NULL;
    for (unsigned i = 0; i < g_nforce; i++)
        if (!g_force_hits[i]) { if (strict || g_flag.imports) LOG("forced takeover entry with no reference at all: %s", g_force[i].name); unused++; }
    if (unused && strict) die("forced takeover list is wrong");
    LOG("force: %u entries hit, %u not referenced by this game", g_nforce - unused, unused);
}

// Read the rewriter table header (segment boundaries, slot numbers); any row beyond the header means an old-format build output (code modified at load time), which is rejected
static void read_layout(module_t *m, const char *dir) {
    char path[1024]; snprintf(path, sizeof path, "%s/%s.fixups.tsv", dir, m->name);
    FILE *f = fopen(path, "r");
    if (!f) die("open %s", path);
    static const char *kn[KEY_N] = {"K18", "KTLS", "KTLR", "KGATE"};
    char line[256], key[32]; unsigned long v; unsigned seen = 0;
    while (fgets(line, sizeof line, f)) {
        if (sscanf(line, "# %31s %lx", key, &v) != 2) die("%s: the output comes from an old rewriter; rewrite the modules again with dm/tools/patch_nso.py", path);
        if (!strcmp(key, "PATCH_OFF")) m->patch_off = strtoll(line + 2 + strlen(key), NULL, 16);   // may be negative (patch area before the base)
        else if (!strcmp(key, "IMAGE_SIZE")) m->image_size = v;
        else if (!strcmp(key, "FILE_IMAGE_LEN")) m->file_image_len = v;
        else if (!strcmp(key, "KEY_PAGE_OFF")) m->key_page_off = v;
        else if (!strcmp(key, "KEY_PAGE_SIZE")) m->key_page_size = v;
        else if (!strcmp(key, "TEXT_END")) m->text_end = v;
        else if (!strcmp(key, "RO_OFF")) m->ro_off = v;
        else if (!strcmp(key, "DATA_OFF")) m->data_off = v;
        else if (!strcmp(key, "FPS_STATE_OFF")) m->fps_state_off = v;
        else if (!strcmp(key, "BUILD_ID")) sscanf(line, "# %*s %32s", m->build_id);
        else for (int k = 0; k < KEY_N; k++) {
            size_t n = strlen(kn[k]);
            if (!strncmp(key, "KEY_", 4) && !strncmp(key + 4, kn[k], n) && !strcmp(key + 4 + n, "_IDX")) {
                m->key_idx[k] = (unsigned)strtoul(strrchr(line, '\t') + 1, 0, 10); seen |= 1u << k;
            }
        }
    }
    fclose(f);
    if (!m->patch_off || m->key_page_off % 0x4000 || m->key_page_size != 0x4000 || seen != (1u << KEY_N) - 1)
        die("%s: the output comes from an old rewriter; rewrite the modules again with dm/tools/patch_nso.py", path);
    if (m->patch_off < 0 && (!m->image_size || !m->file_image_len || m->file_image_len % 0x4000 || m->patch_off % 0x4000 || m->key_page_off + m->key_page_size > m->image_size))
        die("%s: patch area before the base, but IMAGE_SIZE / FILE_IMAGE_LEN missing or wrong", path);
}

// Frame step state (the page after the key page, read-write; layout in patch_nso.py FRAME_STEP): the rewriter's stubs step the game by the host's whole vsyncs capped at this
// target, 0 = the game's own interval logic. The profile's fps_target, overridden by DM_FPS_TARGET (0, 30 or 60); the graphics layer decides the steps and paces presents
// (hle_nvn.c fstep_tick)
uint8_t *dm_fps_state;
static void install_fps_state(module_t *m) {
    if (m->fps_state_off + 0x40 > m->size || m->fps_state_off < m->key_page_off + m->key_page_size) die("%s: frame step state page out of range", m->name);
    const char *e = getenv("DM_FPS_TARGET"); int t = e ? atoi(e) : DM_GAME_FPS_TARGET;
    if (t != 0 && t != 30 && t != 60) { LOG("[RED] DM_FPS_TARGET=%s: must be 0, 30 or 60; using the game's own frame step", e); t = 0; }
    float f = (float)t; dm_fps_state = m->base + m->fps_state_off; memcpy(dm_fps_state, &f, sizeof f);
    // Render scale of the 3D views (the stubs at Tears of the Kingdom's main+0xd5cc0, Breath of the Wild's main+0x1266880): 1.0 keeps the game's own size and dynamic resolution;
    // another factor fixes the size at that fraction (DM_RENDER_SCALE, 0.5 to 1.0). Independent of the frame step: written at target 0 too
    const char *rs = getenv("DM_RENDER_SCALE"); float sc = 1.0f;
    if (rs) { char *end; sc = strtof(rs, &end); if (end == rs || *end || !(sc >= 0.5f && sc <= 1.0f)) { LOG("[RED] DM_RENDER_SCALE=%s: must be 0.5 to 1.0; keeping the game's size", rs); sc = 1.0f; } else LOG("%s: 3D views rendered at %.2f of the game's size (DM_RENDER_SCALE)", m->name, sc); }
    memcpy(dm_fps_state + 0x10, &sc, sizeof sc);
    char cap[32]; snprintf(cap, sizeof cap, ", capped at %d fps", t);
    LOG("%s: frame step %s%s (%s)", m->name, !t ? "as the game sets it" : "in whole vsyncs decided by the host", t ? cap : "", e ? "DM_FPS_TARGET" : "game profile fps_target");
}
static void install_key_page(module_t *m) {
    if (m->key_page_off + m->key_page_size > m->size) die("%s: key page out of range", m->name);
    uint64_t *page = (uint64_t *)(m->base + m->key_page_off);
    memset(page, 0, m->key_page_size);
    #ifdef __ANDROID__
    for (unsigned i = 0; i < KEY_N; i++) page[m->key_idx[i]] = (uint64_t)(1 + i) * 8;   // offset of the slot in the thread's slot block (slot_block)
#else
    for (unsigned i = 0; i < KEY_N; i++) page[m->key_idx[i]] = (uint64_t)key_value(i) * 8;
#endif
    for (int k = 0; k < KEY_N; k++) if (!page[m->key_idx[k]]) die("%s: key page entry %d is 0 (host key not created) -- the game would read TSD slot 0", m->name, k);
    if (mprotect(m->base + m->key_page_off, m->key_page_size, PROT_READ)) die("%s key page mprotect", m->name);
    if (m->fps_state_off) install_fps_state(m);
}

static void register_gates(module_t *m, const char *dir) {
    char path[1024], line[256]; snprintf(path, sizeof path, "%s/%s.gates.tsv", dir, m->name);
    FILE *f = fopen(path, "r");
    int ng = 0;
    if (f) {
        unsigned long ret, orig; char kind[16]; int arg;
        while (fgets(line, sizeof line, f))
            if (sscanf(line, "%lx %15s %d %lx", &ret, kind, &arg, &orig) == 4) { gate_register((uint64_t)m->base + ret, kind, arg, orig); ng++; }
        fclose(f);
    }
    LOG("%s: gates %d patch_off %#llx keys %lu/%lu/%lu/%lu", m->name, ng, m->patch_off, g_key18, g_keytls, g_keytlr, g_keygate);
}

// DT_RELR (from SDK 20; all three modules of The Legend of Zelda: Breath of the Wild 1.9.0 have it): a compact table of relative relocations with the addend in the slot (*slot += base).
// Each 8-byte entry: even = a new start offset (relocate it, the pointer moves to the next slot); odd = a bitmap whose bits 1-63 stand for the 63 slots after the pointer, which then moves on by 63 slots.
// Without it main's GOT and vtables hold module offsets (main's RELA keeps only symbol relocations)
static unsigned relr_apply(module_t *m, uint64_t relr, uint64_t relrsz, uint64_t delta) {
    const uint64_t *e = (const uint64_t *)(m->base + relr); uint64_t *where = NULL; unsigned n = 0;
    for (uint64_t i = 0; i < relrsz / 8; i++) {
        uint64_t v = e[i];
        if (!(v & 1)) { where = (uint64_t *)(m->base + v); *where++ += delta; n++; continue; }
        if (!where) die("%s: DT_RELR bitmap entry with no start before it", m->name);
        for (unsigned b = 0; (v >>= 1); b++) if (v & 1) { where[b] += delta; n++; }
        where += 63;
    }
    return n;
}

static void relocate(module_t *m) {
    uint64_t rela = 0, relasz = 0, jmprel = 0, pltsz = 0, relr = 0, relrsz = 0;
    for (const Elf64_Dyn *d = m->dyn; d->d_tag; d++) switch (d->d_tag) {
        case DT_RELA: rela = d->d_un.d_ptr; break;
        case DT_RELASZ: relasz = d->d_un.d_val; break;
        case DT_JMPREL: jmprel = d->d_un.d_ptr; break;
        case DT_PLTRELSZ: pltsz = d->d_un.d_val; break;
        case DT_RELR: relr = d->d_un.d_ptr; break;
        case DT_RELRSZ: relrsz = d->d_un.d_val; break;
    }
    int cnt[3] = {0}, nint = 0, nweak0 = 0;
    unsigned nrelr = relr ? relr_apply(m, relr, relrsz, (uint64_t)m->base) : 0;
    const Elf64_Rela *tabs[2] = {(const Elf64_Rela *)(m->base + rela), (const Elf64_Rela *)(m->base + jmprel)};
    uint64_t sizes[2] = {relasz, pltsz};
    for (int t = 0; t < 2; t++)
        for (uint64_t i = 0; i < sizes[t] / sizeof(Elf64_Rela); i++) {
            const Elf64_Rela *r = &tabs[t][i];
            uint64_t *where = (uint64_t *)(m->base + r->r_offset);
            unsigned type = ELF64_R_TYPE(r->r_info), si = ELF64_R_SYM(r->r_info);
            if (type == R_AARCH64_RELATIVE) { *where = (uint64_t)m->base + r->r_addend; cnt[0]++; continue; }
            const Elf64_Sym *s = &m->symtab[si];
            uint64_t v = (uint64_t)force_lookup(m->strtab + s->st_name);
            // Symbols the module defines itself are also looked up globally in load order (the ELF default; Horizon's rtld does the same): the definition of an earlier-loaded module overrides a later one; only local / non-default visibility binds to itself.
            // Engage's main defines its own operator new / delete (Unity's memory manager) and sdk has a copy too (forwarding to malloc / free): previously sdk bound its own delete,
            // so blocks from main's new were freed by sdk's libc++ string code into the SDK heap, and CentralHeap::Free reported NotNMallocPtr
            if (!v && s->st_shndx) {
                uint64_t self = (uint64_t)m->base + s->st_value;
                v = ELF64_ST_BIND(s->st_info) == STB_LOCAL || ELF64_ST_VISIBILITY(s->st_other) != STV_DEFAULT ? self : resolve_sym(m, m->strtab + s->st_name);
                if (!v) v = self;
                if (v != self && nint++ < 8) LOG("%s: %s bound to an earlier module's definition (%s)", m->name, m->strtab + s->st_name, module_of((void *)v) ? module_of((void *)v)->name : "?");
            }
            // An undefined weak symbol that no module defines and the host does not implement is 0, as Horizon's rtld binds it: the code tests its address before calling it.
            // Octopath Traveler 0's main imports the thread_local init function _ZTHN13LowLevelTasks5FTask10ActiveTaskE weakly; a named stub made it look defined and the game called it
            const char *nm = m->strtab + s->st_name;
            int weak0 = !v && !s->st_shndx && ELF64_ST_BIND(s->st_info) == STB_WEAK && !guest_sym(nm) && !hle_implements(nm);
            if (weak0 && nweak0++ < 8) LOG("%s: undefined weak %s bound to 0", m->name, nm);
            if (!v && !weak0) v = resolve_sym(m, nm);
            if (type == R_AARCH64_JUMP_SLOT) { *where = v; cnt[2]++; }
            else { *where = v + r->r_addend; cnt[1]++; }       // ABS64 / GLOB_DAT
        }
    LOG("%s: reloc relative=%d relr=%u abs/glob=%d jump=%d%s", m->name, cnt[0], nrelr, cnt[1], cnt[2], nint ? " (some bound to an earlier module's definition)" : "");
    if (nint > 8) LOG("%s: %d bound to an earlier module's definition in all", m->name, nint);
}

// DM_LOADHASH=1 (diagnostics): after loading, output an address-independent sha256 per module, to prove that the load results of "early start / no early start" are byte-identical.
// Module base addresses and host ASLR differ every run, and only the values of the relocation slots change with them: copy the image and, using the same relocation table, replace every slot with an address-independent token --
// RELATIVE → (0, addend); symbol target falls in some module → (1 + module number, offset within the module); otherwise (host forced take-over / host symbol) → (name FNV, addend); all other bytes as is.
#include <CommonCrypto/CommonDigest.h>
void loader_hash_log(void) {
    if (!getenv("DM_LOADHASH")) return;
    for (int k = 0; k < g_nmod; k++) {
        module_t *m = &g_mod[k];
        uint8_t *cp = malloc(m->size); memcpy(cp, m->base, m->size);
        uint64_t rela = 0, relasz = 0, jmprel = 0, pltsz = 0, relr = 0, relrsz = 0;
        for (const Elf64_Dyn *d = m->dyn; d->d_tag; d++) switch (d->d_tag) {
            case DT_RELA: rela = d->d_un.d_ptr; break; case DT_RELASZ: relasz = d->d_un.d_val; break;
            case DT_JMPREL: jmprel = d->d_un.d_ptr; break; case DT_PLTRELSZ: pltsz = d->d_un.d_val; break;
            case DT_RELR: relr = d->d_un.d_ptr; break; case DT_RELRSZ: relrsz = d->d_un.d_val; break;
        }
        // DT_RELR slots: subtracting the base makes them address-independent (done on the copy, reusing relr_apply's walk)
        if (relr) { uint8_t *b = m->base; m->base = cp; relr_apply(m, relr, relrsz, -(uint64_t)b); m->base = b; }
        const Elf64_Rela *tabs[2] = {(const Elf64_Rela *)(m->base + rela), (const Elf64_Rela *)(m->base + jmprel)};
        uint64_t sizes[2] = {relasz, pltsz}; unsigned nslot = 0;
        for (int t = 0; t < 2; t++)
            for (uint64_t i = 0; i < sizes[t] / sizeof(Elf64_Rela); i++) {
                const Elf64_Rela *r = &tabs[t][i]; uint64_t tok[1] = {0};
                if (ELF64_R_TYPE(r->r_info) != R_AARCH64_RELATIVE) {
                    uint64_t v = *(const uint64_t *)(m->base + r->r_offset); int hit = 0;
                    for (int j = 0; j < g_nmod; j++) if (v >= (uint64_t)g_mod[j].base && v < (uint64_t)g_mod[j].base + g_mod[j].size) { tok[0] = 1 + j; hit = 1; break; }
                    if (!hit) { const char *nm = m->strtab + m->symtab[ELF64_R_SYM(r->r_info)].st_name; uint64_t h = 0xcbf29ce484222325ull;
                                for (; *nm; nm++) h = (h ^ (uint8_t)*nm) * 0x100000001b3ull; tok[0] = h | 1ull << 63; }
                }
                memcpy(cp + r->r_offset, &tok[0], 8);                // The 8 slot bytes are replaced by the token head; the token tail is appended below in slot order
                nslot++;
            }
        CC_SHA256_CTX c; CC_SHA256_Init(&c); CC_SHA256_Update(&c, cp, (CC_LONG)m->size);
        for (int t = 0; t < 2; t++)                                // Token tails (addend / offset) are appended in slot order, and together with the slot head uniquely determine the slot value
            for (uint64_t i = 0; i < sizes[t] / sizeof(Elf64_Rela); i++) {
                const Elf64_Rela *r = &tabs[t][i]; uint64_t tail = (uint64_t)r->r_addend;
                if (ELF64_R_TYPE(r->r_info) != R_AARCH64_RELATIVE) {
                    uint64_t v = *(const uint64_t *)(m->base + r->r_offset);
                    for (int j = 0; j < g_nmod; j++) if (v >= (uint64_t)g_mod[j].base && v < (uint64_t)g_mod[j].base + g_mod[j].size) { tail = v - (uint64_t)g_mod[j].base; break; }
                }
                CC_SHA256_Update(&c, &tail, 8);
            }
        uint8_t d[32]; CC_SHA256_Final(d, &c); free(cp);
        char hx[65]; for (int i = 0; i < 32; i++) sprintf(hx + 2 * i, "%02x", d[i]);
        LOG("DM_LOADHASH %s size %#zx relocation slots %u sha256 %s", m->name, m->size, nslot, hx);
    }
}

#if TARGET_OS_IPHONE
// iOS does not allow code loaded at run time to execute, and Apple allows an executable only one segment with execute permission (ITMS-90999): the image is compiled into this executable by dm/ios/tools/gen_embed.py --
// the two code blocks (original text, patch area) are sections in __TEXT, and W (the rest of ro + data + bss) is a separate RW segment. The three blocks are not adjacent in the file; here one contiguous address range is reserved,
// and they are vm_remap'ed back at their original relative positions: code pages with copy=FALSE (sharing the same signed pages with __TEXT, r-x), W with copy=TRUE (copy-on-write, paged only when used).
// The run-time layout is byte-identical to the original image. Remapping signed pages to a new address and then executing them on iOS is exactly what libffi's trampoline table (FFI_EXEC_TRAMPOLINE_TABLE) does
#include <mach/mach.h>
static void remap_at(module_t *m, vm_address_t to, const void *from, unsigned long size, boolean_t copy, vm_prot_t need) {
    vm_address_t a = to; vm_prot_t cur = 0, max = 0;
    kern_return_t kr = vm_remap(mach_task_self(), &a, size, 0, VM_FLAGS_FIXED | VM_FLAGS_OVERWRITE, mach_task_self(), (vm_address_t)from, copy, &cur, &max, VM_INHERIT_NONE);
    if (kr != KERN_SUCCESS || a != to) die("%s vm_remap %p → %#lx (%#lx bytes) failed: %d", m->name, from, (unsigned long)to, size, kr);
    if ((cur & need) != need) die("%s permissions after vm_remap %#x, missing %#x", m->name, cur, need);
}
#define IMAGE_MH (&_mh_execute_header)
static void map_image(module_t *m, const char *dir) {
    (void)dir;
    char sx0[17], sx1[17], dm[17]; snprintf(sx0, sizeof sx0, "__dm%sx0", m->name); snprintf(sx1, sizeof sx1, "__dm%sx1", m->name); snprintf(dm, sizeof dm, "__DM%sW", m->name);
    unsigned long z0, zw, z1;
    const uint8_t *x0 = getsectiondata(IMAGE_MH, "__TEXT", sx0, &z0), *w = getsectiondata(IMAGE_MH, dm, "__img", &zw), *x1 = getsectiondata(IMAGE_MH, "__TEXT", sx1, &z1);
    if (!x0 || !w || !x1) die("embedded sections %s / %s / %s missing", sx0, dm, sx1);
    // Patch area before the base (patch_off < 0): W ends at the end of data (FILE_IMAGE_LEN); bss and the key page are zero pages from vm_allocate and are not in the executable
    int front = m->patch_off < 0;
    if (z0 + zw != (front ? m->file_image_len : (uint64_t)m->patch_off) || (z0 & 0x3FFF) || (zw & 0x3FFF) || (z1 & 0x3FFF) || ((uintptr_t)x0 & 0x3FFF) || ((uintptr_t)w & 0x3FFF) || ((uintptr_t)x1 & 0x3FFF))
        die("%s embedded section layout mismatch (x0 %#lx w %#lx x1 %#lx, PATCH_OFF %#llx)", m->name, z0, zw, z1, (unsigned long long)m->patch_off);
    m->size = front ? m->image_size : m->patch_off + z1;
    uint64_t pre = front ? (uint64_t)-m->patch_off : 0;
    if (front) m->patch_len = z1;
    vm_address_t base = 0;
    if (vm_allocate(mach_task_self(), &base, pre + m->size, VM_FLAGS_ANYWHERE) != KERN_SUCCESS) die("%s failed to reserve %#llx bytes of address space", m->name, (unsigned long long)(pre + m->size));
    base += pre;
    remap_at(m, base, x0, z0, FALSE, VM_PROT_READ | VM_PROT_EXECUTE);
    remap_at(m, base + z0, w, zw, TRUE, VM_PROT_READ | VM_PROT_WRITE);
    remap_at(m, base + m->patch_off, x1, z1, FALSE, VM_PROT_READ | VM_PROT_EXECUTE);
    m->base = (uint8_t *)base; install_key_page(m);
}
#else
// Code patches applied while the image is still writable, before the code area is sealed: IPSwitch .pchtxt files from DM_MODS_DIR (community mods, only the ones whose
// @nsobid matches the module), then the built-in ones. Either skips a patch that would overwrite an instruction the rewriter (patch_nso.py) turned into a branch into
// its patch area [p0, p1), since that would undo the rewrite
#include <ctype.h>
#include <dirent.h>
#include <strings.h>
static int hex_nib(char c) { return c >= '0' && c <= '9' ? c - '0' : c >= 'a' && c <= 'f' ? c - 'a' + 10 : c >= 'A' && c <= 'F' ? c - 'A' + 10 : -1; }
static int overlaps_rewrite(const module_t *m, uint64_t off, size_t nb, int64_t p0, int64_t p1) {
    if (!nb || off >= m->text_end) return 0;
    for (uint64_t a = off & ~3ull; a <= ((off + nb - 1) & ~3ull) && a + 4 <= m->text_end; a += 4) {
        uint32_t w; memcpy(&w, m->base + a, 4);
        if ((w & 0xfc000000u) == 0x14000000u) { int64_t t = (int64_t)a + ((int64_t)(int32_t)(w << 6) >> 4); if (t >= p0 && t < p1) return 1; }
    }
    return 0;
}
// One .pchtxt file: "@nsobid-<build id>", "@flag offset_shift <n>", "@enabled" / "@disabled", "@stop", then "<hex offset> <hex bytes>" or "<hex offset> \"text\"" lines
// (offsets count the 0x100-byte NSO header, as IPSwitch's do); // and # start comments
static int pchtxt_apply(module_t *m, uint64_t lim, int64_t p0, int64_t p1, const char *path) {
    if (!m->build_id[0]) return 0;
    FILE *f = fopen(path, "r"); if (!f) return 0;
    char ln[1024]; int bid_ok = 0, en = 0, applied = 0, line_no = 0; int64_t shift = 0;
    while (fgets(ln, sizeof ln, f)) {
        char *t = ln; if (!line_no++ && (uint8_t)t[0] == 0xef && (uint8_t)t[1] == 0xbb && (uint8_t)t[2] == 0xbf) t += 3;
        char *c1 = strstr(t, "//"), *c2 = strchr(t, '#'); if (c1) *c1 = 0; if (c2) *c2 = 0;
        while (isspace((unsigned char)*t)) t++;
        for (size_t e = strlen(t); e && isspace((unsigned char)t[e - 1]); ) t[--e] = 0;
        if (!*t) continue;
        if (*t == '@') {
            if (!strncasecmp(t, "@nsobid-", 8)) { const char *b = t + 8; size_t bl = 0; while (hex_nib(b[bl]) >= 0) bl++;
                if (bl < 16 || strncasecmp(b, m->build_id, bl < 32 ? bl : 32)) { fclose(f); return 0; }   // another module or version
                bid_ok = 1; }
            else if (!strncasecmp(t, "@flag", 5) && isspace((unsigned char)t[5])) { char *q = t + 5; while (isspace((unsigned char)*q)) q++;
                if (!strncasecmp(q, "offset_shift", 12) && isspace((unsigned char)q[12])) shift = strtoll(q + 12, NULL, 0); }
            else if (!strcasecmp(t, "@enabled")) en = 1;
            else if (!strcasecmp(t, "@disabled")) en = 0;
            else if (!strcasecmp(t, "@stop")) break;
            continue;
        }
        if (!bid_ok || !en) continue;
        char *end = NULL; uint64_t addr = strtoull(t, &end, 16);
        if (end == t || !isspace((unsigned char)*end)) continue;
        char *v = end; while (isspace((unsigned char)*v)) v++;
        uint8_t buf[512]; size_t nb = 0; int bad = 0;
        if (*v == '"') { for (v++; *v && *v != '"'; v++) { if (*v == '\\' && v[1]) v++; if (nb >= sizeof buf) { bad = 1; break; } buf[nb++] = (uint8_t)*v; } if (*v != '"') bad = 1; }
        else while (*v) { if (isspace((unsigned char)*v)) { v++; continue; } int hi = hex_nib(v[0]), lo = v[1] ? hex_nib(v[1]) : -1;
            if (hi < 0 || lo < 0 || nb >= sizeof buf) { bad = 1; break; } buf[nb++] = (uint8_t)(hi << 4 | lo); v += 2; }
        if (bad || !nb) { LOG("[RED] %s:%d: not a valid value, skipped", path, line_no); continue; }
        int64_t off = (int64_t)addr + shift - 0x100;
        if (off < 0 || (uint64_t)off + nb > lim) { LOG("[RED] %s:%d: +%#llx (%zu bytes) is outside %s, skipped", path, line_no, (unsigned long long)off, nb, m->name); continue; }
        if (overlaps_rewrite(m, (uint64_t)off, nb, p0, p1)) { LOG("[RED] %s:%d: +%#llx is code the host rewrote, skipped", path, line_no, (unsigned long long)off); continue; }
        memcpy(m->base + off, buf, nb); applied++;
    }
    fclose(f);
    if (applied) LOG("%s: %d patch(es) from %s", m->name, applied, path);
    return applied;
}
// the .pchtxt files of one folder, in name order; depth 1 also looks into each mod folder as the Switch emulators lay them out (<mod>/exefs/*.pchtxt, or <mod>/*.pchtxt);
// a mod folder named with a leading "-" or "." is switched off
static int pchtxt_scan(module_t *m, uint64_t lim, int64_t p0, int64_t p1, const char *d, int depth, int *files) {
    struct dirent **ents = NULL; int n = scandir(d, &ents, NULL, alphasort), total = 0;
    if (n < 0) { if (errno != ENOENT && depth) LOG("[RED] %s: mods folder %s unreadable (errno %d)", m->name, d, errno); return 0; }
    for (int i = 0; i < n; i++) { const char *nm = ents[i]->d_name; size_t l = strlen(nm); char path[1200]; snprintf(path, sizeof path, "%s/%s", d, nm);
        struct stat st;
        if (l > 7 && !strcasecmp(nm + l - 7, ".pchtxt")) { (*files)++; total += pchtxt_apply(m, lim, p0, p1, path); }
        else if (depth > 0 && nm[0] != '.' && nm[0] != '-' && strcasecmp(nm, "romfs") && !stat(path, &st) && S_ISDIR(st.st_mode)) {
            char ex[1300]; snprintf(ex, sizeof ex, "%s/exefs", path);
            total += pchtxt_scan(m, lim, p0, p1, ex, 0, files) + pchtxt_scan(m, lim, p0, p1, path, 0, files);
        }
        free(ents[i]); }
    free(ents);
    return total;
}
static int pchtxt_dir(module_t *m, uint64_t lim, int64_t p0, int64_t p1) {
    const char *d = getenv("DM_MODS_DIR"); if (!d || !*d || !m->build_id[0]) return 0;
    int files = 0, total = pchtxt_scan(m, lim, p0, p1, d, 1, &files);
    if (files) LOG("%s: mods folder %s: %d .pchtxt file(s), %d patch(es) applied to this module", m->name, d, files, total);
    return total;
}
// Built-in patches. (No built-in 20:9 camera: the 16:9 that PR #48 patched, MOVK X9, #0x8e39 / #0x3fe3 at +0x607de8, is a camera object's default aspect, which
// the game replaces for the gameplay camera; patched, the picture was unchanged. DM_ASPECT=20:9 is the presentation alone, for a 20:9 .pchtxt mod.)
int dm_step_hz; uint8_t *dm_step_word;   // the high frame rate below: its rate and the game's step word, which hle_nvn.c vstep_tick rewrites every frame
uint8_t *dm_step_table;   // the game's whole frame-time table (5 u64 ns, intervals 1-5), when it holds the game's own values: vstep_tick writes the real step into all of it
static int fps_patch(module_t *m, uint64_t lim, int64_t p0, int64_t p1);
// Built-in community mods (builtin_mods.h, from dm/games/*/builtin_mods/*.pchtxt), each behind a setting: Scarlet / Violet 4.0.0's dynamic frame rate (Fl4sh's ASM
// "Dynamic FPS": up to 60, the game's step from the time each frame really took) with DM_FPS_TARGET=60, their 2048×2048 shadow maps (Fl4sh's "2K Shadows") with
// DM_SHADOWS=2k. A mod goes in whole or not at all: every site must still hold the game's own bytes and none may be code the rewriter turned into a branch
#include "builtin_mods.h"
static int bmod_wanted(const char *name) {
    if (!strcmp(name, "dynamic_fps")) { const char *e = getenv("DM_FPS_TARGET"); return e && atoi(e) >= 60; }
    if (!strcmp(name, "shadows_2k")) { const char *e = getenv("DM_SHADOWS"); return e && !strcasecmp(e, "2k"); }   // (test only: not offered in the menu)
    return 0;
}
static int g_bmod_fps;   // the frame rate came from a built-in mod: the Sword / Shield patch below is not this game's
int dm_dynfps;   // hle_nvn.c dyn_tick: Scarlet / Violet's Dynamic FPS is in, presentation is paced in steps of 30 / 60
static int bmod_apply(module_t *m, uint64_t lim, int64_t p0, int64_t p1) {
    int total = 0;
    for (unsigned i = 0; i < sizeof BUILTIN_MODS / sizeof *BUILTIN_MODS; i++) {
        const bmod *b = &BUILTIN_MODS[i]; int hit = 0;
        for (int k = 0; b->builds[k]; k++) if (!strncasecmp(m->build_id, b->builds[k], 32)) hit = 1;
        if (!hit || !bmod_wanted(b->name)) continue;
        int bad = -1;
        for (int s = 0; s < b->n && bad < 0; s++) { const bmod_site *t = &b->sites[s];
            if ((uint64_t)t->off + t->len > lim || memcmp(m->base + t->off, t->orig, t->len) || overlaps_rewrite(m, t->off, t->len, p0, p1)) bad = s; }
        if (bad >= 0) { LOG("[RED] %s: built-in mod %s not applied: +%#x differs from the game's code or was rewritten", m->name, b->name, b->sites[bad].off); continue; }
        for (int s = 0; s < b->n; s++) memcpy(m->base + b->sites[s].off, b->sites[s].mod, b->sites[s].len);
        LOG("%s: built-in mod %s applied (%d sites)", m->name, b->name, b->n); total += b->n; if (!strcmp(b->name, "dynamic_fps")) g_bmod_fps = dm_dynfps = 1;
    }
    return total;
}
static int builtin_patches(module_t *m, uint64_t lim, int64_t p0, int64_t p1, int modded) {
    (void)modded;
    if (strcmp(m->name, "main")) return 0;
    int n = bmod_apply(m, lim, p0, p1);
    return n + (g_bmod_fps ? 0 : fps_patch(m, lim, p0, p1));
}
// Built-in: DM_FPS_TARGET=60..120 (opt-in, the in-game menu; experimental) for Pokémon Shield and Sword 1.3.2's main: the present interval the game asks NVN for, 2 (30 fps),
// becomes 1 (in the game's present-interval setter: if the interval asked for differs from the window's, MOV W1, W21 then a call through NVN's set-present-interval
// pointer; → MOV W1, #1), and the frame time the game takes for interval 2 (in a table of u64 ns by interval: 16666667, 33333334, 50000000 ...) becomes 1 / fps.
// From then on presentation paces at that rate and the step word follows the frames actually shown (hle_nvn.c vstep_tick), so game time stays real time when frames
// run late. The two games are the same engine build: Sword's setter is the same 13 instructions as Shield's, 0x90 bytes earlier; the table is at the same place.
// Both words are checked first: another build, or code that differs, is left alone
static const struct { const char *build; uint64_t itv, dt; const char *game; } FPS_SITES[] = {
    { "a16802625e7826bf83b6f9708e475b91", 0x18a2d18, 0x203eeb8, "Shield 1.3.2" },
    { "a3b75bcd3311385aeed67fbeeb79cbb7", 0x18a2c88, 0x203eeb8, "Sword 1.3.2" },
};
// Live frame rate (DM_FPS_LIVE, on unless "0"): with the frame rate at 30, 60 or Auto the patch is applied at 60 Hz slots whatever the target, and the target lives in
//   dm_fps_live (30 or 60), which the in-game menu changes while playing (android_main.c nativeSetting): at 30 every frame is held 2 slots and the game told
//   interval 2, as the game itself does at 30 (hle_nvn.c vstep_tick, reported_hold); at 60 the hold is 1, 2 when frames run late. 90 / 120 (env.txt) stay fixed
int dm_fps_live;   // 0: not live (the game's own 30, or a fixed 90 / 120)
// The stadium crowd at 30 (DM_CROWD_30, on unless "0"): the crowd's flipbook (models ob0041..ob0044 stadiumcrowda..d, in the new-game intro in Rose's stadium after the
//   name entry) flickers at 60, and no fix in the frame step has cured it yet. When the game opens one of those models the live rate drops to 30; when it next loads a
//   zone's scripts (bin/script/amx/z_*.amx: play has moved to a field area) the rate it had comes back, unless the menu changed it meanwhile. hle_fs.c h_fs_OpenFile
static pthread_mutex_t g_crowd_mu = PTHREAD_MUTEX_INITIALIZER; static int g_crowd_saved;   // the live rate before the hold, 0: no hold
void dm_fps_scene_file(const char *path) {
    if (!path || !__atomic_load_n(&dm_fps_live, __ATOMIC_RELAXED)) return;
    int crowd = strstr(path, "_stadiumcrowd") != NULL, zone = !crowd && strstr(path, "/script/amx/z_") != NULL;
    if (!crowd && !zone) return;
    static int on = -1; if (on < 0) { const char *e = getenv("DM_CROWD_30"); on = !(e && !strcmp(e, "0")); }
    if (!on) return;
    pthread_mutex_lock(&g_crowd_mu);
    int cur = __atomic_load_n(&dm_fps_live, __ATOMIC_RELAXED);
    if (crowd && !g_crowd_saved && cur != 30) {
        g_crowd_saved = cur; __atomic_store_n(&dm_fps_live, 30, __ATOMIC_RELAXED);
        LOG("frame rate: the stadium crowd is loading (%s): 30 fps until play moves to a field area (its flipbook flickers at %d)", path, cur); }
    else if (zone && g_crowd_saved) {
        if (cur == 30) { __atomic_store_n(&dm_fps_live, g_crowd_saved, __ATOMIC_RELAXED); LOG("frame rate: a field area loads (%s): %d fps again", path, g_crowd_saved); }
        else LOG("frame rate: a field area loads (%s): the menu set %d meanwhile, kept", path, cur);
        g_crowd_saved = 0; }
    pthread_mutex_unlock(&g_crowd_mu);
}
static int fps_patch(module_t *m, uint64_t lim, int64_t p0, int64_t p1) {
    const char *e = getenv("DM_FPS_TARGET"), *lv = getenv("DM_FPS_LIVE"); int live = !(lv && !strcmp(lv, "0"));
    int want = e && *e ? atoi(e) : 0;
    if (!want) { const char *a = getenv("DM_AUTO_FPS"); want = a && atoi(a) >= 60 ? 60 : 30; }   // (Frame rate on Auto: the phone profile's)
    int fps = want > 60 && want <= 120 ? want : want == 60 || (live && want == 30) ? 60 : 0;
    if (!fps) return 0;
    int si = -1; for (unsigned i = 0; i < sizeof FPS_SITES / sizeof *FPS_SITES; i++) if (!strncasecmp(m->build_id, FPS_SITES[i].build, 32)) si = (int)i;
    if (si < 0) { if (want >= 60) LOG("[RED] %s: %d fps patch not applied (not Pokémon Sword / Shield 1.3.2's main)", m->name, fps); return 0; }   // (at 30: nothing was asked for)
    const uint64_t itv = FPS_SITES[si].itv, dt = FPS_SITES[si].dt;
    // (the step word is rewritten every frame, so it must lie past the code area, which is sealed read + execute)
    if (itv + 4 > m->text_end || dt < m->text_end || dt + 4 > lim || overlaps_rewrite(m, itv, 4, p0, p1)) {
        LOG("[RED] %s: %d fps patch not applied (%s: the step word lies in code, or that code was rewritten)", m->name, fps, FPS_SITES[si].game); return 0; }
    uint32_t wi, wd; memcpy(&wi, m->base + itv, 4); memcpy(&wd, m->base + dt, 4);
    if ((wi != 0x2a1503e1u && wi != 0x52800041u) || wd != 33333334u) { LOG("[RED] %s: %d fps patch not applied (unexpected words %08x %u)", m->name, fps, wi, wd); return 0; }
    // The game does not take its step from the interval alone: once a frame (Shield FUN_006ea110 / FUN_006df360 in Ghidra, image +0x5ea110 / +0x5df360) it
    //   measures its own frame cost, max(CPU work without its waits, the GPU time between two timestamp counter reports), rounds it up to whole vsyncs and
    //   picks that entry of this table (16.7 / 33.3 / 50 / 66.7 / 83.3 ms). On this engine both measures miss time: the waits it subtracts are where our
    //   translation and a throttled phone make it wait, and the timestamps of one submission are equal (written when it completes), so the game advanced
    //   33 ms for frames that took 40-62 ms: the world, its animations above all, in slow motion. With the whole table found, every entry gets the real step
    {   static const uint64_t own[5] = { 16666667, 33333334, 50000000, 66666667, 83333334 }; uint64_t t[5];
        if (dt >= 8 && dt - 8 >= m->text_end && dt + 32 <= lim) { memcpy(t, m->base + dt - 8, sizeof t);
            if (!memcmp(t, own, sizeof t)) dm_step_table = m->base + dt - 8; }
        if (!dm_step_table) LOG("%s: the frame-time table around +%#llx is not the game's own: only its interval-2 entry follows the real step", m->name, (unsigned long long)dt); }
    wi = 0x52800021u; wd = (uint32_t)((1000000000ull + (uint64_t)fps - 1) / (uint64_t)fps);
    memcpy(m->base + itv, &wi, 4); memcpy(m->base + dt, &wd, 4);
    dm_step_hz = fps; dm_step_word = m->base + dt;
    if (fps == 60 && live) __atomic_store_n(&dm_fps_live, want == 60 ? 60 : 30, __ATOMIC_RELAXED);
    LOG("%s: %d fps patch for %s (present interval 1 at +%#llx, step %.2f ms at +%#llx, then paced by the host)%s", m->name, fps, FPS_SITES[si].game, (unsigned long long)itv, wd / 1e6,
        (unsigned long long)dt, dm_fps_live ? (want == 60 ? "; live frame rate, now 60 (the menu switches 30 / 60 at once)" : "; live frame rate, now 30 (the menu switches 30 / 60 at once)") : "");
    return 2;
}
static uint8_t *slurp(const char *path, size_t *n) {
    int fd = open(path, O_RDONLY);
    if (fd < 0) die("open %s", path);
    struct stat st; fstat(fd, &st);
    uint8_t *p = malloc(st.st_size);
    if (read(fd, p, st.st_size) != st.st_size) die("read %s", path);
    close(fd); *n = st.st_size; return p;
}

// Seal the code area immediately after copying into anonymous pages (16 KB pages): [0, text_end) and the patch area are read-only+execute, and from then on any write to the code area crashes on the spot;
// what lies between (ro and data on the same page can only go together) is writable
static void map_image(module_t *m, const char *dir) {
    char path[1024]; snprintf(path, sizeof path, "%s/%s.patched.bin", dir, m->name);
    size_t sz; uint8_t *raw = slurp(path, &sz);
    if (m->patch_off < 0) {
        // Patch area before the base: file = the first part of the image (text + ro + data) + patch area; bss and the key page are anonymous zero pages. The gap from the patch area's end to the base is left inaccessible (PROT_NONE)
        uint64_t pre = (uint64_t)-m->patch_off, fl = m->file_image_len;
        if (sz < fl) die("%s is shorter than FILE_IMAGE_LEN %#llx", path, (unsigned long long)fl);
        m->size = m->image_size; m->patch_len = (sz - fl + 0x3FFF) & ~0x3FFFull;
        if (m->patch_len > pre) die("%s patch area of %#llx bytes does not fit in the %#llx before the base", path, (unsigned long long)m->patch_len, (unsigned long long)pre);
        uint8_t *a = mmap(NULL, pre + m->size, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANON, -1, 0);
        if (a == MAP_FAILED) die("%s mmap %#llx", m->name, (unsigned long long)(pre + m->size));
        m->base = a + pre;
        memcpy(m->base, raw, fl); memcpy(a, raw + fl, sz - fl); free(raw);
        int modded = pchtxt_dir(m, fl, m->patch_off, m->patch_off + (int64_t)m->patch_len); modded += builtin_patches(m, fl, m->patch_off, m->patch_off + (int64_t)m->patch_len, modded);
        install_key_page(m);
        uint64_t rx = (m->text_end + 0x3FFF) & ~0x3FFFull;
        if (mprotect(m->base, rx, PROT_READ | PROT_EXEC)) die("%s mprotect text", m->name);
        if (modded) __builtin___clear_cache((char *)m->base, (char *)(m->base + rx));
        if (mprotect(a, m->patch_len, PROT_READ | PROT_EXEC)) die("%s mprotect patch", m->name);
        if (pre > m->patch_len && mprotect(a + m->patch_len, pre - m->patch_len, PROT_NONE)) die("%s mprotect gap", m->name);
        return;
    }
    m->size = (sz + 0x3FFF) & ~0x3FFFull;
    m->base = mmap(NULL, m->size, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANON, -1, 0);
    if (m->base == MAP_FAILED) die("%s mmap", m->name);
    memcpy(m->base, raw, sz); free(raw);
    int modded = pchtxt_dir(m, m->key_page_off, m->patch_off, (int64_t)sz); modded += builtin_patches(m, m->key_page_off, m->patch_off, (int64_t)sz, modded);
    install_key_page(m);
    uint64_t rx = (m->text_end + 0x3FFF) & ~0x3FFFull;
    if (mprotect(m->base, rx, PROT_READ | PROT_EXEC)) die("%s mprotect text", m->name);
    if (modded) __builtin___clear_cache((char *)m->base, (char *)(m->base + rx));
    uint64_t po = m->patch_off & ~0x3FFFull;
    if (mprotect(m->base + po, m->size - po, PROT_READ | PROT_EXEC)) die("%s mprotect patch", m->name);
}
#endif

void load_modules(const char *dir, const char **names, int n) {
    // Map everything and parse the symbol tables first, then relocate all at once (cross-module symbols must all be in place first)
    // DM_BOOTPROF: breakdown of ① host loading (milliseconds), to set the upper bound for ③ pre-relocation / mmap
    uint64_t t0 = now_ns(), tm = 0, tp = 0, tg = 0, tr = 0, t;
    for (int i = 0; i < n; i++) {
        module_t *m = &g_mod[i];
        strlcpy(m->name, names[i], sizeof m->name);
        read_layout(m, dir);
    }
    create_keys();
    for (int i = 0; i < n; i++) {
        module_t *m = &g_mod[i];
        t = now_ns(); map_image(m, dir); tm += now_ns() - t;   // On return the code area is already read-only+execute; from here on the loader only writes data
        if (!i) tsd_check();
        t = now_ns(); parse_dynamic(m); tp += now_ns() - t;
        t = now_ns(); register_gates(m, dir); tg += now_ns() - t;
        LOG("%s: base %p size %#zx", names[i], m->base, m->size);
        g_nmod++;
    }
    force_init();
    t = now_ns(); for (int i = 0; i < g_nmod; i++) relocate(&g_mod[i]); tr = now_ns() - t;
    force_check();
    { void hlecount_seal(void); hlecount_seal(); }   // DM_HLECOUNT: relocation done, seal the counting stub pages as read-only executable
    if (getenv("DM_SYMCHECK")) LOG("DM_SYMCHECK: %lu symbol lookups at load time, hash and linear disagree %lu", g_symchk_n, g_symchk_bad);
    if (g_flag.bootprof) LOG("① breakdown: mapping (read + copy + seal pages) %.1f ms, dynamic section parsing %.1f ms, gate registration %.1f ms, relocation + forced takeover %.1f ms, total %.1f ms",
                             tm / 1e6, tp / 1e6, tg / 1e6, tr / 1e6, (now_ns() - t0) / 1e6);
}

// Plays the role of rtld's InitModules (rtld+0x830): call each module's DT_INIT in forward order of the load list.
// The timing is decided by nn::init::Start: after nninitStartup and _init_libc2, before nnMain (the 4th argument callback of Start).
void run_module_inits(void) {
    for (int i = 0; i < g_nmod; i++) {
        module_t *m = &g_mod[i];
        if (!m->init) continue;
        LOG("%s: DT_INIT +%#llx (init_array %llu entries)", m->name, m->init, m->init_arraysz / 8);
        ((void (*)(void))(m->base + m->init))();
    }
}
