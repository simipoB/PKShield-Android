// GLSLC at run time. Fire Emblem: Three Houses (KTGL, SDK 6.5.0) compiles 9 built-in GLSL shaders at boot with the game's own runtime compiler (subsdk0, glslcCompile called through a function
// table from main+0x9c89f8; the sources are plain strings in main, e.g. "in vec3 ktglPosition0; …"). Their outputs are in no game file, and shaders are translated offline only, so the
// outputs are captured once on the Mac: DM_GLSLC_CAPTURE=<dir> writes every successful compile's GLSLC output (magic 0x19866891, the whole output's length at +0x44) to
// <dir>/<FNV-1a of the output>.glslc. shader_split.py reads that folder like any other source of GLSLC outputs (profile key shader_glslc_capture).
// The compile object's first word points at its last results {GLSLCcompilationStatus *, GLSLCoutput *}; the status has "success" at +0xc and "allocation error" at +0xd, which is what the
// game itself reads after the call (main+0x9c8a04)
#include "hle.h"
#include <sys/stat.h>

static void save(const uint8_t *out) {
    static const char *dir; static int init;
    if (!init) { init = 1; dir = getenv("DM_GLSLC_CAPTURE"); if (dir) { mkdir(dir, 0755); LOG("glslc: run-time compile outputs are saved to %s", dir); } }
    if (!dir) return;
    uint32_t n = *(const uint32_t *)(out + 0x44);
    if (*(const uint32_t *)out != 0x19866891 || n < 0x48 || n > (64u << 20)) { LOG("[RED] glslc: unknown output header (magic %#x length %#x), not saved", *(const uint32_t *)out, n); return; }
    uint64_t h = 0xcbf29ce484222325ull; for (uint32_t i = 0; i < n; i++) h = (h ^ out[i]) * 0x100000001b3ull;
    char path[1024]; snprintf(path, sizeof path, "%s/%016llx.glslc", dir, (unsigned long long)h);
    struct stat st; if (!stat(path, &st)) return;
    FILE *f = fopen(path, "wb"); if (!f) { LOG("[RED] glslc: cannot write %s", path); return; }
    fwrite(out, 1, n, f); fclose(f);
    LOG("glslc: saved a run-time compile output %s (%u bytes)", path, n);
}
static void capture(const void *obj) {
    const uint8_t *const *res = *(const uint8_t *const *const *)obj;
    if (!res || !res[0] || !res[1] || !res[0][0xc] || res[0][0xd]) return;
    save(res[1]);
}

HLE_FORCE("glslcCompile", "run-time GLSL compile (the game's own GLSLC compiles as usual; with DM_GLSLC_CAPTURE the outputs are saved to disk for offline translation)", uint8_t, h_glslc_Compile, void *obj) {
    static uint8_t (*orig)(void *);
    if (!orig && !(orig = guest_sym("glslcCompile"))) die("glslcCompile missing");
    uint8_t ok = orig(obj);
    if (ok) capture(obj);
    return ok;
}
// Specialization (Pokémon Shield 1.3.2, SDK 7.3.2): the game compiles a shader once with glslcCompilePreSpecialized, then asks glslcCompileSpecialized for variants with uniform values baked in;
// the shaders it draws with are those variants, in no game file (45 missing from the map through the intro). The batch is {uint32 count, GLSLCspecializationSet *entries}; the call returns
// one GLSLCoutput pointer per entry (null for a failed one), saved like the outputs above
HLE_FORCE("glslcCompileSpecialized", "run-time GLSLC specialization (the game's own GLSLC as usual; with DM_GLSLC_CAPTURE every variant is saved to disk for offline translation)",
          const uint8_t *const *, h_glslc_CompileSpec, const void *obj, const uint32_t *batch) {
    static const uint8_t *const *(*orig)(const void *, const uint32_t *);
    if (!orig && !(orig = guest_sym("glslcCompileSpecialized"))) die("glslcCompileSpecialized missing");
    const uint8_t *const *outs = orig(obj, batch);
    if (outs && batch) for (uint32_t i = 0; i < batch[0]; i++) if (outs[i]) save(outs[i]);
    return outs;
}
