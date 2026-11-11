// SPDX-License-Identifier: GPL-3.0-or-later
// The player's Lossless.dll → the 25 shaders Lossless Scaling frame generation uses (nvn_lsfg.h), as SPIR-V in files/lsfg (GameMenu "Import Lossless.dll").
// The DLL's RCDATA resources hold its compute shaders as DXBC (Direct3D 11 bytecode); dxbc-spirv (dm/third_party/dxbc-spirv, MIT) converts them with the options
// its own dxbc_compiler tool uses (the PC test, dm/gfx/tests/t_lsfg_vk.c, checked those). A resource that is SPIR-V already is kept as it is.
// Nothing of the DLL leaves the phone, and none of it is part of the app.
#include <jni.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sstream>
#include <string>
#include <vector>
#include <sys/stat.h>
#include <unistd.h>
#include "dxbc/dxbc_api.h"
#include "dxbc/dxbc_container.h"
#include "dxbc/dxbc_converter.h"
#include "ir/ir_legalize.h"
#include "ir/passes/ir_pass_lower_io.h"
#include "spirv/spirv_builder.h"
#include "spirv/spirv_mapping.h"
#include "util/util_byte_stream.h"

using namespace dxbc_spv;

namespace {
const uint32_t IDS[25] = { 255, 256, 280, 281, 282, 283, 284, 285, 286, 287, 288, 289, 290, 291, 292, 293, 294, 295, 296, 297, 298, 299, 300, 301, 302 };

struct Pe {
    const uint8_t *b; size_t n;
    struct Sec { uint32_t va, vsize, raw, rsize; }; std::vector<Sec> sec;
    bool u16(size_t o, uint16_t &v) const { if (o + 2 > n) return false; memcpy(&v, b + o, 2); return true; }
    bool u32(size_t o, uint32_t &v) const { if (o + 4 > n) return false; memcpy(&v, b + o, 4); return true; }
    bool off(uint32_t rva, size_t &o) const {   // a virtual address → where it is in the file
        for (const Sec &s : sec) { uint32_t sz = s.vsize > s.rsize ? s.vsize : s.rsize; if (rva >= s.va && rva - s.va < sz) { if (rva - s.va >= s.rsize) return false; o = (size_t)s.raw + (rva - s.va); return o < n; } }
        return false;
    }
};
// The RCDATA resource `id` (any language): its bytes, or NULL
const uint8_t *rcdata(const Pe &p, size_t rbase, uint32_t id, size_t &len) {
    // a resource directory: 16 bytes, then named and id entries of 8 bytes (id, offset with the top bit set for a subdirectory)
    auto find = [&](size_t dir, uint32_t want, bool any, uint32_t &out) -> bool {
        uint16_t nn, ni; if (!p.u16(dir + 12, nn) || !p.u16(dir + 14, ni)) return false;
        for (uint32_t k = 0; k < (uint32_t)nn + ni; k++) { uint32_t name, o; if (!p.u32(dir + 16 + 8 * k, name) || !p.u32(dir + 20 + 8 * k, o)) return false;
            if (k < nn) continue;   // (named: not what is looked for)
            if (any || name == want) { out = o; return true; } }
        return false; };
    uint32_t t, nm, lg;
    if (!find(rbase, 10, false, t) || !(t & 0x80000000u)) return NULL;   // RT_RCDATA
    if (!find(rbase + (t & 0x7fffffffu), id, false, nm) || !(nm & 0x80000000u)) return NULL;
    if (!find(rbase + (nm & 0x7fffffffu), 0, true, lg) || (lg & 0x80000000u)) return NULL;
    uint32_t rva, sz; size_t o; size_t leaf = rbase + lg;
    if (!p.u32(leaf, rva) || !p.u32(leaf + 4, sz) || !p.off(rva, o) || sz > p.n - o) return NULL;
    len = sz; return p.b + o;
}
// DXBC → SPIR-V, as dxbc_compiler does it with its default options
bool convert(const uint8_t *d, size_t n, std::vector<char> &out) {
    util::ByteReader reader(d, n);
    dxbc::Container container(reader); if (!container) return false;
    std::stringstream hs; hs << container.getHash(); std::string name = hs.str();   // (the shader's name: its hash, as the tool names it)
    dxbc::Converter::Options co = { }; co.includeDebugNames = true; co.name = name.c_str();
    dxbc::Converter converter(std::move(container), co);
    ir::Builder builder; if (!converter.convertShader(builder)) return false;
    ir::CompileOptions o = { };
    o.bufferOptions.useTypedForSparseFeedback = true; o.bufferIndexing.allowDynamicIndexing = true;
    o.cseOptions.relocateDescriptorLoad = true; o.cseOptions.resolveOverlappingLoads = true; o.descriptorIndexing.optimizeDescriptorIndexing = true;
    ir::legalizeIr(builder, o);
    { ir::LowerIoPass pass(builder); pass.lowerSampleCountToSpecConstant(0u); }   // (the legacy Direct3D 9 built-ins it lowers too never occur in compute shaders)
    spirv::BasicResourceMapping mapping = { };
    spirv::SpirvBuilder::Options so = { };
    so.includeDebugNames = true; so.floatControls2 = false;
    so.supportedRoundModesF16 = so.supportedRoundModesF32 = so.supportedRoundModesF64 = ir::RoundMode::eNearestEven | ir::RoundMode::eZero;
    so.supportedDenormModesF16 = so.supportedDenormModesF32 = so.supportedDenormModesF64 = ir::DenormMode::eFlush | ir::DenormMode::ePreserve;
    spirv::SpirvBuilder sb(builder, mapping, so); sb.buildSpirvBinary();
    size_t sz = 0; sb.getSpirvBinary(sz, nullptr); if (!sz) return false;
    out.resize(sz); sb.getSpirvBinary(sz, out.data()); return true;
}
// NULL when every shader is converted into dir (written as .tmp, then renamed: a folder never holds half an import), else why not
const char *import_dll(const uint8_t *b, size_t n, const std::string &dir, std::string &why) {
    Pe p = { b, n, {} }; uint16_t mz, nsec, optsz, magic; uint32_t lfa, sig;
    if (!p.u16(0, mz) || mz != 0x5a4d || !p.u32(0x3c, lfa) || !p.u32(lfa, sig) || sig != 0x00004550) return "this is not a Windows DLL";
    size_t coff = (size_t)lfa + 4, opt = coff + 20;
    if (!p.u16(coff + 2, nsec) || !p.u16(coff + 16, optsz) || !p.u16(opt, magic) || (magic != 0x10b && magic != 0x20b)) return "this is not a Windows DLL";
    size_t dd = opt + (magic == 0x20b ? 112 : 96); uint32_t ndd, rrva;   // the data directories; [2] the resources
    if (!p.u32(dd - 4, ndd) || ndd < 3 || !p.u32(dd + 16, rrva) || !rrva) return "the DLL has no resources (is it Lossless.dll?)";
    for (uint32_t k = 0; k < nsec; k++) { size_t s = opt + optsz + 40 * k; Pe::Sec x;
        if (!p.u32(s + 8, x.vsize) || !p.u32(s + 12, x.va) || !p.u32(s + 16, x.rsize) || !p.u32(s + 20, x.raw)) return "the DLL is damaged";
        p.sec.push_back(x); }
    size_t rbase; if (!p.off(rrva, rbase)) return "the DLL is damaged";
    mkdir(dir.c_str(), 0700);
    for (uint32_t id : IDS) {
        size_t len = 0; const uint8_t *d = rcdata(p, rbase, id, len); char path[600];
        if (!d || len < 20) { why = "shader " + std::to_string(id) + " is not in this DLL (Lossless Scaling 3.x is needed)"; return why.c_str(); }
        std::vector<char> spv;
        uint32_t m; memcpy(&m, d, 4);
        if (m == 0x07230203u) spv.assign((const char *)d, (const char *)d + len);
        else if (memcmp(d, "DXBC", 4) || !convert(d, len, spv)) { why = "shader " + std::to_string(id) + " could not be converted"; return why.c_str(); }
        snprintf(path, sizeof path, "%s/%u.spv.tmp", dir.c_str(), id); FILE *f = fopen(path, "wb");
        if (!f || fwrite(spv.data(), 1, spv.size(), f) != spv.size() || fclose(f)) { if (f) fclose(f); why = "could not write " + std::string(path); return why.c_str(); } }
    for (uint32_t id : IDS) { char a[600], c[600]; snprintf(a, sizeof a, "%s/%u.spv.tmp", dir.c_str(), id); snprintf(c, sizeof c, "%s/%u.spv", dir.c_str(), id);
        if (rename(a, c)) { why = "could not write " + std::string(c); return why.c_str(); } }
    return NULL;
}
}

// GameMenu.nativeLsfgImport: the DLL's bytes and the folder; null when done, else the reason for the player
extern "C" JNIEXPORT jstring JNICALL Java_com_nxnative_GameMenu_nativeLsfgImport(JNIEnv *env, jclass, jbyteArray dll, jstring jdir) {
    jsize n = env->GetArrayLength(dll); jbyte *b = env->GetByteArrayElements(dll, NULL);
    const char *d = env->GetStringUTFChars(jdir, NULL); std::string dir(d); env->ReleaseStringUTFChars(jdir, d);
    std::string why; const char *r = b ? import_dll((const uint8_t *)b, (size_t)n, dir, why) : "out of memory";
    if (b) env->ReleaseByteArrayElements(dll, b, JNI_ABORT);
    return r ? env->NewStringUTF(r) : NULL;
}

#ifdef LSFG_IMPORT_MAIN   // the same on a PC: lsfg_import <Lossless.dll> <folder>
int main(int argc, char **argv) {
    if (argc < 3) { fprintf(stderr, "usage: lsfg_import <Lossless.dll> <folder>\n"); return 2; }
    FILE *f = fopen(argv[1], "rb"); if (!f) { perror(argv[1]); return 1; }
    std::vector<uint8_t> b; uint8_t buf[65536]; size_t k; while ((k = fread(buf, 1, sizeof buf, f)) > 0) b.insert(b.end(), buf, buf + k); fclose(f);
    std::string why; const char *r = import_dll(b.data(), b.size(), argv[2], why);
    if (r) { fprintf(stderr, "lsfg_import: %s\n", r); return 1; }
    printf("lsfg_import: 25 shaders written to %s\n", argv[2]); return 0;
}
#endif
