// SPIR-V reading for the Vulkan replay, see nvn_spv.h. Only what the translator's shaders use: names, Binding / DescriptorSet / Location / Block / BufferBlock decorations,
// variables in UniformConstant / Uniform / StorageBuffer / Input storage, and the types behind them.
#include "nvn_spv.h"
#include <stdlib.h>
#include <string.h>

enum { OpName = 5, OpEntryPoint = 15, OpExecutionMode = 16, OpTypeInt = 21, OpTypeFloat = 22, OpTypeVector = 23, OpTypeImage = 25, OpTypeSampler = 26, OpTypeSampledImage = 27,
       OpTypeArray = 28, OpTypeRuntimeArray = 29, OpTypeStruct = 30, OpTypePointer = 32, OpConstant = 43, OpVariable = 59, OpDecorate = 71 };
enum { DecBlock = 2, DecBufferBlock = 3, DecBuiltIn = 11, DecLocation = 30, DecBinding = 33, DecDescriptorSet = 34 };
enum { SC_UniformConstant = 0, SC_Input = 1, SC_Uniform = 2, SC_StorageBuffer = 12 };

typedef struct { uint16_t op; uint32_t a, b, c, d, e, f; uint8_t block, bufblock, has_binding, has_set, has_loc, builtin; uint32_t binding, set, loc; char name[48]; } ent;

int spv_reflect(const uint32_t *code, size_t words, spv_res *res, int max_res, int *nres, spv_input *in, int max_in, int *nin, int *stage, uint32_t ls[3]) {
    *nres = 0; if (nin) *nin = 0; *stage = -1; if (ls) ls[0] = ls[1] = ls[2] = 0;
    if (words < 5 || code[0] != 0x07230203u) return -1;
    uint32_t bound = code[3]; if (!bound || bound > (1u << 22)) return -1;
    ent *e = calloc(bound, sizeof *e); if (!e) return -1;
    uint32_t *vars = malloc(sizeof(uint32_t) * bound), nv = 0;
    for (size_t i = 5; i < words;) {
        uint32_t w = code[i], n = w >> 16, op = w & 0xffff;
        if (!n || i + n > words) break;
        const uint32_t *x = code + i;
        if (op == OpName && n >= 3 && x[1] < bound) { size_t m = (n - 2) * 4; if (m > sizeof e[0].name - 1) m = sizeof e[0].name - 1; memcpy(e[x[1]].name, x + 2, m); e[x[1]].name[m] = 0; }
        else if (op == OpEntryPoint && n >= 3) *stage = (int)x[1];
        else if (op == OpExecutionMode && n >= 6 && x[2] == 17 && ls) { ls[0] = x[3]; ls[1] = x[4]; ls[2] = x[5]; }
        else if (op == OpDecorate && n >= 3 && x[1] < bound) {
            ent *t = &e[x[1]];
            switch (x[2]) {
                case DecBlock: t->block = 1; break; case DecBufferBlock: t->bufblock = 1; break; case DecBuiltIn: t->builtin = 1; break;
                case DecBinding: if (n >= 4) { t->has_binding = 1; t->binding = x[3]; } break;
                case DecDescriptorSet: if (n >= 4) { t->has_set = 1; t->set = x[3]; } break;
                case DecLocation: if (n >= 4) { t->has_loc = 1; t->loc = x[3]; } break;
            }
        }
        else if ((op == OpTypeInt || op == OpTypeFloat || op == OpTypeVector || op == OpTypeImage || op == OpTypeSampler || op == OpTypeSampledImage || op == OpTypeArray
                  || op == OpTypeRuntimeArray || op == OpTypeStruct || op == OpTypePointer) && n >= 2 && x[1] < bound) {
            ent *t = &e[x[1]]; t->op = (uint16_t)op;
            if (n > 2) t->a = x[2]; if (n > 3) t->b = x[3]; if (n > 4) t->c = x[4]; if (n > 5) t->d = x[5]; if (n > 6) t->e = x[6]; if (n > 7) t->f = x[7];
        }
        else if (op == OpConstant && n >= 4 && x[2] < bound) { e[x[2]].op = OpConstant; e[x[2]].a = x[1]; e[x[2]].b = x[3]; }   // result type first, then the result: the value is in b
        else if (op == OpVariable && n >= 4 && x[2] < bound) { e[x[2]].op = OpVariable; e[x[2]].a = x[1]; e[x[2]].b = x[3]; vars[nv++] = x[2]; }   // a = pointer type, b = storage class
        i += n;
    }
    for (uint32_t k = 0; k < nv; k++) {
        uint32_t id = vars[k]; ent *v = &e[id]; uint32_t sc = v->b;
        if (v->a >= bound || e[v->a].op != OpTypePointer) continue;
        uint32_t ty = e[v->a].b;
        if (sc == SC_Input && v->has_loc && !v->builtin && in && nin && *nin < max_in) {   // vertex inputs: component class and count of the (vector) type
            ent *t = ty < bound ? &e[ty] : NULL; uint8_t nc = 1;
            if (t && t->op == OpTypeVector) { nc = (uint8_t)t->b; t = t->a < bound ? &e[t->a] : NULL; }
            in[*nin].location = v->loc; in[*nin].ncomp = nc;
            in[*nin].sclass = t && t->op == OpTypeInt ? (t->b ? 2 : 1) : 0; (*nin)++;
            continue;
        }
        if (sc != SC_UniformConstant && sc != SC_Uniform && sc != SC_StorageBuffer) continue;
        if (!v->has_binding || *nres >= max_res) continue;
        spv_res *r = &res[*nres]; memset(r, 0, sizeof *r);
        strncpy(r->name, v->name, sizeof r->name - 1); r->set = v->set; r->binding = v->binding; r->count = 1;
        ent *t = ty < bound ? &e[ty] : NULL;
        if (t && (t->op == OpTypeArray || t->op == OpTypeRuntimeArray)) {
            if (t->op == OpTypeArray && t->b < bound && e[t->b].op == OpConstant) r->count = e[t->b].b;
            t = t->a < bound ? &e[t->a] : NULL;
        }
        if (!t) continue;
        if (sc == SC_StorageBuffer || (sc == SC_Uniform && t->bufblock)) r->kind = SPV_RES_SSBO;
        else if (sc == SC_Uniform) r->kind = SPV_RES_UBO;
        else {
            ent *im = t;
            if (t->op == OpTypeSampledImage) im = t->a < bound ? &e[t->a] : NULL;
            if (t->op == OpTypeSampler) r->kind = SPV_RES_SAMP;
            else if (im && im->op == OpTypeImage) {
                // OpTypeImage: a = sampled type, b = dim, c = depth, d = arrayed, e = ms, f = sampled (1 with a sampler, 2 storage)
                r->dim = (uint8_t)im->b; r->depth = im->c == 1; r->arrayed = (uint8_t)im->d; r->ms = (uint8_t)im->e;
                ent *st = im->a < bound ? &e[im->a] : NULL; r->sclass = st && st->op == OpTypeInt ? (st->b ? 2 : 1) : 0;
                if (im->f == 2) r->kind = im->b == SPV_DIM_BUFFER ? SPV_RES_IBUF : SPV_RES_IMG;
                else if (im->b == SPV_DIM_BUFFER) r->kind = SPV_RES_TBUF;
                else r->kind = t->op == OpTypeSampledImage ? SPV_RES_TEX : SPV_RES_SIMG;
            } else continue;
        }
        (*nres)++;
    }
    free(vars); free(e);
    return *stage < 0 ? -1 : 0;
}

void spv_rebind(uint32_t *code, size_t words, uint32_t add) {
    for (size_t i = 5; i < words;) {
        uint32_t n = code[i] >> 16, op = code[i] & 0xffff;
        if (!n || i + n > words) return;
        if (op == OpDecorate && n >= 4 && code[i + 2] == DecBinding) code[i + 3] += add;
        i += n;
    }
}

enum { OpTypeBool = 20, OpTypeMatrix = 24, OpConstantNull = 46, OpAccessChain = 65, OpInBoundsAccessChain = 66, SC_Output = 3, SC_Private = 6 };
static uint32_t slots_of(const uint32_t *code, size_t words, uint32_t ty, int depth);
// The definition of id (its instruction's start), by a scan: used only for the few types behind I/O variables
static const uint32_t *def_of(const uint32_t *code, size_t words, uint32_t id) {
    for (size_t i = 5; i < words;) { uint32_t n = code[i] >> 16, op = code[i] & 0xffff; if (!n || i + n > words) return NULL;
        if (((op >= OpTypeBool && op <= OpTypePointer) || op == OpConstant) && n >= 2 && code[i + (op == OpConstant ? 2 : 1)] == id) return code + i;
        i += n; }
    return NULL;
}
static uint32_t slots_of(const uint32_t *code, size_t words, uint32_t ty, int depth) {
    const uint32_t *d = def_of(code, words, ty); if (!d || depth > 4) return 1;
    uint32_t op = d[0] & 0xffff;
    if (op == OpTypeArray) { const uint32_t *c = def_of(code, words, d[3]); uint32_t len = c && (c[0] & 0xffff) == OpConstant ? c[3] : 1; return len * slots_of(code, words, d[2], depth + 1); }
    if (op == OpTypeMatrix) return d[3];
    return 1;
}
void spv_io_masks(const uint32_t *code, size_t words, uint64_t *in_mask, uint64_t *out_mask) {
    *in_mask = *out_mask = 0;
    if (words < 5 || code[0] != 0x07230203u) return;
    uint32_t bound = code[3]; if (!bound || bound > (1u << 22)) return;
    int32_t *loc = malloc(sizeof(int32_t) * bound); uint8_t *bi = calloc(bound, 1); for (uint32_t i = 0; i < bound; i++) loc[i] = -1;
    for (size_t i = 5; i < words;) { uint32_t n = code[i] >> 16, op = code[i] & 0xffff; if (!n || i + n > words) break; const uint32_t *x = code + i;
        if (op == OpDecorate && n >= 3 && x[1] < bound) { if (x[2] == DecLocation && n >= 4) loc[x[1]] = (int32_t)x[3]; else if (x[2] == DecBuiltIn) bi[x[1]] = 1; }
        i += n; }
    for (size_t i = 5; i < words;) { uint32_t n = code[i] >> 16, op = code[i] & 0xffff; if (!n || i + n > words) break; const uint32_t *x = code + i;
        if (op == OpVariable && n >= 4 && x[2] < bound && (x[3] == SC_Input || x[3] == SC_Output) && loc[x[2]] >= 0 && !bi[x[2]]) {
            const uint32_t *p = def_of(code, words, x[1]); uint32_t k = p && (p[0] & 0xffff) == OpTypePointer ? slots_of(code, words, p[3], 0) : 1;
            for (uint32_t s = 0; s < k && loc[x[2]] + s < 64; s++) *(x[3] == SC_Input ? in_mask : out_mask) |= 1ull << (loc[x[2]] + s);
        }
        i += n; }
    free(loc); free(bi);
}
// Emits (in the types / constants section) the value an unwritten Maxwell attribute reads, for a varying of type ty: (0, 0, 0, 1) for a 4-component vector (the 1 as the
// bits of float 1.0 whatever the component type: attributes are untyped 32-bit slots), zeros otherwise. Returns its id (new ids from *nb)
static uint32_t emit_default(uint32_t *o, size_t *w, const uint32_t *code, size_t words, uint32_t ty, uint32_t *nb) {
    const uint32_t *d = def_of(code, words, ty);
    if (d && (d[0] & 0xffff) == OpTypeVector && d[3] >= 2 && d[3] <= 4) {
        const uint32_t *c = def_of(code, words, d[2]);
        if (c && ((c[0] & 0xffff) == OpTypeFloat || (c[0] & 0xffff) == OpTypeInt) && c[2] == 32) {
            uint32_t z = (*nb)++, one = (*nb)++, v = (*nb)++;
            o[(*w)++] = 4u << 16 | OpConstant; o[(*w)++] = d[2]; o[(*w)++] = z; o[(*w)++] = 0;
            o[(*w)++] = 4u << 16 | OpConstant; o[(*w)++] = d[2]; o[(*w)++] = one; o[(*w)++] = 0x3f800000u;
            o[(*w)++] = (3 + d[3]) << 16 | 44 /* OpConstantComposite */; o[(*w)++] = ty; o[(*w)++] = v;
            for (uint32_t k = 0; k < d[3]; k++) o[(*w)++] = k == 3 ? one : z;
            return v;
        }
    }
    uint32_t v = (*nb)++; o[(*w)++] = 3u << 16 | OpConstantNull; o[(*w)++] = ty; o[(*w)++] = v; return v;
}
uint32_t *spv_drop_inputs(const uint32_t *code, size_t words, uint64_t drop, size_t *out_words) {
    if (!drop || words < 5 || code[0] != 0x07230203u) return NULL;
    uint32_t bound = code[3]; if (!bound || bound > (1u << 22)) return NULL;
    int32_t *loc = malloc(sizeof(int32_t) * bound); uint8_t *bi = calloc(bound, 1), *dv = calloc(bound, 1); uint32_t *pp = calloc(bound, 4), *pn = calloc(bound, 4);
    for (uint32_t i = 0; i < bound; i++) loc[i] = -1;
    for (size_t i = 5; i < words;) { uint32_t n = code[i] >> 16, op = code[i] & 0xffff; if (!n || i + n > words) break; const uint32_t *x = code + i;
        if (op == OpDecorate && n >= 3 && x[1] < bound) { if (x[2] == DecLocation && n >= 4) loc[x[1]] = (int32_t)x[3]; else if (x[2] == DecBuiltIn) bi[x[1]] = 1; }
        i += n; }
    int any = 0;   // dv: dropped variables; pp[input pointer type] = its Private twin (made below), pn[pointee] = OpConstantNull of it
    for (size_t i = 5; i < words;) { uint32_t n = code[i] >> 16, op = code[i] & 0xffff; if (!n || i + n > words) break; const uint32_t *x = code + i;
        if (op == OpVariable && n >= 4 && x[2] < bound && x[1] < bound && x[3] == SC_Input && loc[x[2]] >= 0 && loc[x[2]] < 64 && !bi[x[2]] && (drop >> loc[x[2]] & 1)) { dv[x[2]] = 1; pp[x[1]] = 1; any = 1; }
        else if ((op == OpAccessChain || op == OpInBoundsAccessChain) && n >= 4 && x[1] < bound && x[2] < bound && x[3] < bound && dv[x[3]]) { dv[x[2]] = 2; pp[x[1]] = 1; }   // pointers into them
        i += n; }
    if (!any) { free(loc); free(bi); free(dv); free(pp); free(pn); return NULL; }
    uint32_t *o = malloc(sizeof(uint32_t) * (words + 64 + 8 * bound / 64 + 1024)); size_t w = 5, cap = words + 64 + 8 * bound / 64 + 1024; memcpy(o, code, 20);
    uint32_t nb = bound; int pre14 = code[1] < 0x00010400u;
    for (size_t i = 5; i < words;) { uint32_t n = code[i] >> 16, op = code[i] & 0xffff; if (!n || i + n > words) break; const uint32_t *x = code + i;
        if (w + n + 16 > cap) { cap = cap * 2 + n; o = realloc(o, sizeof(uint32_t) * cap); }
        if (op == OpDecorate && n >= 2 && x[1] < bound && dv[x[1]] == 1) { i += n; continue; }   // no Location / Flat / … on a Private variable
        if (op == OpEntryPoint && pre14) {                               // before SPIR-V 1.4 the interface lists only Input / Output
            size_t s = 3; while (s < n && (x[s] & 0xff000000u) && (x[s] & 0xff0000u) && (x[s] & 0xff00u) && (x[s] & 0xffu)) s++; s++;   // past the name's last word (the one with a zero byte)
            size_t at = w; memcpy(o + w, x, 4 * s); w += s;
            for (size_t k = s; k < n; k++) if (!(x[k] < bound && dv[x[k]] == 1)) o[w++] = x[k];
            o[at] = (uint32_t)(w - at) << 16 | op; i += n; continue;
        }
        if (op == OpTypePointer && n >= 4 && x[1] < bound && pp[x[1]] == 1) {   // the Input pointer type: then its Private twin and the null constant of the pointee
            memcpy(o + w, x, 4 * n); w += n;
            uint32_t tw = nb++; o[w++] = 4u << 16 | OpTypePointer; o[w++] = tw; o[w++] = SC_Private; o[w++] = x[3]; pp[x[1]] = tw;
            if (x[3] < bound && !pn[x[3]]) pn[x[3]] = emit_default(o, &w, code, words, x[3], &nb);
            i += n; continue;
        }
        if ((op == OpAccessChain || op == OpInBoundsAccessChain) && n >= 4 && x[2] < bound && dv[x[2]] == 2 && x[1] < bound && pp[x[1]] > 1) {
            memcpy(o + w, x, 4 * n); o[w + 1] = pp[x[1]]; w += n; i += n; continue; }
        if (op == OpVariable && n >= 4 && x[2] < bound && dv[x[2]] == 1) {
            const uint32_t *p = def_of(code, words, x[1]); uint32_t pointee = p ? p[3] : 0;
            o[w++] = 5u << 16 | OpVariable; o[w++] = pp[x[1]]; o[w++] = x[2]; o[w++] = SC_Private; o[w++] = pointee < bound ? pn[pointee] : 0;
            i += n; continue;
        }
        memcpy(o + w, x, 4 * n); w += n; i += n;
    }
    o[3] = nb; *out_words = w;
    free(loc); free(bi); free(dv); free(pp); free(pn);
    return o;
}

uint32_t *spv_init_outputs(const uint32_t *code, size_t words, size_t *out_words) {
    if (words < 5 || code[0] != 0x07230203u) return NULL;
    uint32_t bound = code[3]; if (!bound || bound > (1u << 22)) return NULL;
    uint8_t *ov = calloc(bound, 1), *bi = calloc(bound, 1), *hasloc = calloc(bound, 1); uint32_t *pt = calloc(bound, 4), *dc = calloc(bound, 4); uint32_t entry = 0; int any = 0;
    for (size_t i = 5; i < words;) { uint32_t n = code[i] >> 16, op = code[i] & 0xffff; if (!n || i + n > words) break; const uint32_t *x = code + i;
        if (op == OpDecorate && n >= 3 && x[1] < bound) { if (x[2] == DecLocation) hasloc[x[1]] = 1; else if (x[2] == DecBuiltIn) bi[x[1]] = 1; }
        else if (op == OpEntryPoint && n >= 3 && !entry) entry = x[2];
        i += n; }
    for (size_t i = 5; i < words;) { uint32_t n = code[i] >> 16, op = code[i] & 0xffff; if (!n || i + n > words) break; const uint32_t *x = code + i;
        if (op == OpVariable && n >= 4 && x[2] < bound && x[1] < bound && x[3] == SC_Output && hasloc[x[2]] && !bi[x[2]]) {
            const uint32_t *p = def_of(code, words, x[1]); const uint32_t *d = p ? def_of(code, words, p[3]) : NULL;
            if (d && (d[0] & 0xffff) == OpTypeVector) { ov[x[2]] = 1; pt[x[1]] = 1; any = 1; } }   // vectors only (arrays of them are rare in vertex outputs)
        i += n; }
    if (!any || !entry) { free(ov); free(bi); free(hasloc); free(pt); free(dc); return NULL; }
    size_t cap = words + 64 + 16 * 64; uint32_t *o = malloc(sizeof(uint32_t) * cap), nb = bound; size_t w = 5; memcpy(o, code, 20);
    int in_entry = 0, stored = 0;
    for (size_t i = 5; i < words;) { uint32_t n = code[i] >> 16, op = code[i] & 0xffff; if (!n || i + n > words) break; const uint32_t *x = code + i;
        if (w + n + 64 > cap) { cap = cap * 2 + n; o = realloc(o, sizeof(uint32_t) * cap); }
        if (op == OpTypePointer && n >= 4 && x[1] < bound && pt[x[1]] == 1) {
            memcpy(o + w, x, 4 * n); w += n; if (x[3] < bound && !dc[x[3]]) dc[x[3]] = emit_default(o, &w, code, words, x[3], &nb); pt[x[1]] = 2; i += n; continue; }
        if (op == 54 /* OpFunction */ && n >= 3 && x[2] == entry) in_entry = 1;
        // in the entry function's first block, after its OpVariables: store the default into every user vector output
        if (in_entry && !stored && op != 248 /* OpLabel */ && op != OpVariable && op != 54 && op != 55 /* OpFunctionParameter */ && op != 8 /* OpLine */ && op != 317 /* OpNoLine */) {
            for (size_t k = 5; k < words;) { uint32_t kn = code[k] >> 16, kop = code[k] & 0xffff; if (!kn || k + kn > words) break; const uint32_t *y = code + k;
                if (kop == OpVariable && kn >= 4 && y[2] < bound && ov[y[2]]) { const uint32_t *p = def_of(code, words, y[1]);
                    if (p && p[3] < bound && dc[p[3]]) { if (w + 4 > cap) { cap *= 2; o = realloc(o, sizeof(uint32_t) * cap); } o[w++] = 3u << 16 | 62 /* OpStore */; o[w++] = y[2]; o[w++] = dc[p[3]]; } }
                k += kn; }
            stored = 1;
        }
        memcpy(o + w, x, 4 * n); w += n; i += n;
    }
    o[3] = nb; *out_words = w;
    free(ov); free(bi); free(hasloc); free(pt); free(dc);
    if (!stored) { free(o); return NULL; }
    return o;
}

// ---- Clean-up for strict drivers (Adreno): the translator declares capabilities every shader of a stage gets, whether it uses them or not ----
size_t spv_strip_unused(uint32_t *code, size_t words) {
    if (words < 5 || code[0] != 0x07230203u) return words;
    int f64 = 0, ilk = 0;
    for (size_t i = 5; i < words;) { uint32_t n = code[i] >> 16, op = code[i] & 0xffff; if (!n || i + n > words) return words;
        if (op == OpTypeFloat && n >= 3 && code[i + 2] == 64) f64 = 1;
        if (op == 5364 || op == 5365) ilk = 1;   // OpBegin / OpEndInvocationInterlockEXT
        i += n; }
    size_t w = 5;
    for (size_t i = 5; i < words;) { uint32_t n = code[i] >> 16, op = code[i] & 0xffff; const uint32_t *x = code + i; int drop = 0;
        if (op == 17 /* OpCapability */ && n == 2 && ((x[1] == 10 && !f64) || ((x[1] == 5378 || x[1] == 5363 || x[1] == 5372) && !ilk))) drop = 1;
        if (op == 10 /* OpExtension */ && !ilk && n >= 2 && !strncmp((const char *)(x + 1), "SPV_EXT_fragment_shader_interlock", 4 * (n - 1))) drop = 1;
        if (op == OpExecutionMode && n >= 3 && x[2] >= 5366 && x[2] <= 5371 && !ilk) drop = 1;   // the interlock execution modes
        if (!drop) { memmove(code + w, x, 4 * n); w += n; }
        i += n; }
    return w;
}

// The component class of each fragment output location: 0 float, 1 uint, 2 sint, 255 none
void spv_frag_outputs(const uint32_t *code, size_t words, uint8_t cls[8]) {
    memset(cls, 255, 8);
    if (words < 5 || code[0] != 0x07230203u) return;
    uint32_t bound = code[3]; if (!bound || bound > (1u << 22)) return;
    int32_t *loc = malloc(sizeof(int32_t) * bound); for (uint32_t i = 0; i < bound; i++) loc[i] = -1;
    for (size_t i = 5; i < words;) { uint32_t n = code[i] >> 16, op = code[i] & 0xffff; if (!n || i + n > words) break;
        if (op == OpDecorate && n >= 4 && code[i + 1] < bound && code[i + 2] == DecLocation) loc[code[i + 1]] = (int32_t)code[i + 3];
        i += n; }
    for (size_t i = 5; i < words;) { uint32_t n = code[i] >> 16, op = code[i] & 0xffff; if (!n || i + n > words) break; const uint32_t *x = code + i;
        if (op == OpVariable && n >= 4 && x[3] == SC_Output && x[2] < bound && loc[x[2]] >= 0 && loc[x[2]] < 8) {
            const uint32_t *p = def_of(code, words, x[1]); const uint32_t *t = p ? def_of(code, words, p[3]) : NULL;
            if (t && (t[0] & 0xffff) == OpTypeVector) t = def_of(code, words, t[2]);
            if (t) cls[loc[x[2]]] = (t[0] & 0xffff) == OpTypeInt ? (t[3] ? 2 : 1) : 0; }
        i += n; }
    free(loc);
}

// A copy in which the float outputs at the locations in umask / smask become uint / int outputs holding the same bits (OpBitcast before each store), for a colour
// target of an integer format: Maxwell's outputs are untyped registers the ROP writes as they are. NULL if the outputs cannot be rewritten (an output read back)
uint32_t *spv_int_outputs(const uint32_t *code, size_t words, uint32_t umask, uint32_t smask, size_t *out_words) {
    if (!(umask | smask) || words < 5 || code[0] != 0x07230203u) return NULL;
    uint32_t bound = code[3]; if (!bound || bound > (1u << 22)) return NULL;
    int32_t *loc = malloc(sizeof(int32_t) * bound); uint8_t *tv = calloc(bound, 1); uint32_t *ncomp = calloc(bound, 4); for (uint32_t i = 0; i < bound; i++) loc[i] = -1;
    uint32_t id_int[2] = {0, 0}, id_vec[2][5] = {{0}}, id_pv[2][5] = {{0}}, id_ps[2] = {0, 0};   // existing types: int scalar, vectors, Output pointers to them
    for (size_t i = 5; i < words;) { uint32_t n = code[i] >> 16, op = code[i] & 0xffff; if (!n || i + n > words) { free(loc); free(tv); free(ncomp); return NULL; } const uint32_t *x = code + i;
        if (op == OpDecorate && n >= 4 && x[1] < bound && x[2] == DecLocation) loc[x[1]] = (int32_t)x[3];
        if (op == OpTypeInt && n >= 4 && x[2] == 32) id_int[x[3] ? 1 : 0] = x[1];
        i += n; }
    for (size_t i = 5; i < words;) { uint32_t n = code[i] >> 16, op = code[i] & 0xffff; const uint32_t *x = code + i;
        if (op == OpTypeVector && n >= 4 && x[3] <= 4) for (int s = 0; s < 2; s++) if (id_int[s] && x[2] == id_int[s]) id_vec[s][x[3]] = x[1];
        i += n; }
    for (size_t i = 5; i < words;) { uint32_t n = code[i] >> 16, op = code[i] & 0xffff; const uint32_t *x = code + i;
        if (op == OpTypePointer && n >= 4 && x[2] == SC_Output) for (int s = 0; s < 2; s++) { if (id_int[s] && x[3] == id_int[s]) id_ps[s] = x[1]; for (int c = 2; c <= 4; c++) if (id_vec[s][c] && x[3] == id_vec[s][c]) id_pv[s][c] = x[1]; }
        if (op == OpVariable && n >= 4 && x[3] == SC_Output && x[2] < bound && loc[x[2]] >= 0 && loc[x[2]] < 8) {
            uint32_t bit = 1u << loc[x[2]]; int s = (smask & bit) ? 1 : (umask & bit) ? 0 : -1;
            const uint32_t *p = def_of(code, words, x[1]); const uint32_t *t = p ? def_of(code, words, p[3]) : NULL; uint32_t nc = 1; const uint32_t *ct = t;
            if (t && (t[0] & 0xffff) == OpTypeVector) { nc = t[3]; ct = def_of(code, words, t[2]); }
            if (s >= 0 && ct && (ct[0] & 0xffff) == OpTypeFloat && ct[2] == 32 && nc >= 1 && nc <= 4) { tv[x[2]] = (uint8_t)(1 + s); ncomp[x[2]] = nc; } }
        i += n; }
    // chains into the outputs, and outputs read back (not rewritten)
    for (size_t i = 5; i < words;) { uint32_t n = code[i] >> 16, op = code[i] & 0xffff; const uint32_t *x = code + i;
        if ((op == OpAccessChain || op == OpInBoundsAccessChain) && n >= 5 && x[3] < bound && tv[x[3]] && x[2] < bound) { tv[x[2]] = (uint8_t)(tv[x[3]] + 2); ncomp[x[2]] = 1; }   // 3 / 4: a scalar component
        if (op == 61 /* OpLoad */ && n >= 4 && x[3] < bound && tv[x[3]]) { free(loc); free(tv); free(ncomp); return NULL; }
        i += n; }
    int any = 0; for (uint32_t i = 0; i < bound; i++) if (tv[i] == 1 || tv[i] == 2) any = 1;
    if (!any) { free(loc); free(tv); free(ncomp); return NULL; }
    size_t cap = words * 2 + 256; uint32_t *o = malloc(4 * cap), nb = bound; size_t w = 5; memcpy(o, code, 20);
    int emitted = 0;
    for (size_t i = 5; i < words;) { uint32_t n = code[i] >> 16, op = code[i] & 0xffff; const uint32_t *x = code + i;
        if (w + n + 64 > cap) { cap = cap * 2 + n; o = realloc(o, 4 * cap); }
        if (op == 54 /* OpFunction */ && !emitted) {   // before the first function: the integer types still missing, then the rewritten output variables
            emitted = 1;
            for (int s = 0; s < 2; s++) {
                int need = 0; for (uint32_t v = 0; v < bound; v++) if ((tv[v] == 1 + s) || (tv[v] == 3 + s)) need = 1;
                if (!need) continue;
                if (!id_int[s]) { id_int[s] = nb++; o[w++] = 4u << 16 | OpTypeInt; o[w++] = id_int[s]; o[w++] = 32; o[w++] = (uint32_t)s; }
                if (!id_ps[s]) { id_ps[s] = nb++; o[w++] = 4u << 16 | OpTypePointer; o[w++] = id_ps[s]; o[w++] = SC_Output; o[w++] = id_int[s]; }
                for (uint32_t v = 0; v < bound; v++) if (tv[v] == 1 + s && ncomp[v] >= 2) { uint32_t c = ncomp[v];
                    if (!id_vec[s][c]) { id_vec[s][c] = nb++; o[w++] = 4u << 16 | OpTypeVector; o[w++] = id_vec[s][c]; o[w++] = id_int[s]; o[w++] = c; }
                    if (!id_pv[s][c]) { id_pv[s][c] = nb++; o[w++] = 4u << 16 | OpTypePointer; o[w++] = id_pv[s][c]; o[w++] = SC_Output; o[w++] = id_vec[s][c]; } }
            }
            for (uint32_t v = 0; v < bound; v++) if (tv[v] == 1 || tv[v] == 2) { int s = tv[v] - 1; uint32_t c = ncomp[v];
                o[w++] = 4u << 16 | OpVariable; o[w++] = c >= 2 ? id_pv[s][c] : id_ps[s]; o[w++] = v; o[w++] = SC_Output; }
        }
        if (op == OpVariable && n >= 4 && x[2] < bound && (tv[x[2]] == 1 || tv[x[2]] == 2)) { i += n; continue; }   // moved below the new types
        if ((op == OpAccessChain || op == OpInBoundsAccessChain) && n >= 5 && x[2] < bound && tv[x[2]] >= 3) {
            memcpy(o + w, x, 4 * n); o[w + 1] = id_ps[tv[x[2]] - 3]; w += n; i += n; continue; }
        if (op == 62 /* OpStore */ && n >= 3 && x[1] < bound && tv[x[1]]) {
            int s = (tv[x[1]] - 1) & 1; uint32_t c = ncomp[x[1]]; uint32_t ty = c >= 2 && tv[x[1]] <= 2 ? id_vec[s][c] : id_int[s], b = nb++;
            o[w++] = 4u << 16 | 124 /* OpBitcast */; o[w++] = ty; o[w++] = b; o[w++] = x[2];
            o[w++] = (uint32_t)n << 16 | 62; o[w++] = x[1]; o[w++] = b; for (uint32_t k = 3; k < n; k++) o[w++] = x[k];
            i += n; continue; }
        memcpy(o + w, x, 4 * n); w += n; i += n;
    }
    o[3] = nb; *out_words = w; free(loc); free(tv); free(ncomp);
    return emitted ? o : (free(o), NULL);
}

// Transform feedback capture (nvn_vk.c tf_draw; the Metal side's main0_tf from dmsh_build.py vs_tf): a copy of a vertex shader that, before each return of its entry function, stores
// the captured outputs as their bits into storage buffers: buffer b (binding binding0 + b, set 0) gets output tf[b][0]'s first tf[b][1] components at word
// (VertexIndex - pc.first) * tf[b][1] + pc.base[b] + i, with pc = push constants { uint first; uint base[4]; }. NULL if the code cannot be read
uint32_t *spv_tf_variant(const uint32_t *code, size_t words, const int tf[4][2], uint32_t binding0, size_t *out_words) {
    if (words < 5 || code[0] != 0x07230203u) return NULL;
    uint32_t bound = code[3]; if (!bound || bound > (1u << 22)) return NULL;
    int32_t *loc = malloc(sizeof(int32_t) * bound); uint8_t *bi = calloc(bound, 1); for (uint32_t i = 0; i < bound; i++) loc[i] = -1;
    uint32_t entry = 0, t_uint = 0, t_int = 0, vid = 0, vid_ty = 0, outv[64] = {0}, out_ty[64] = {0}, out_nc[64] = {0}, out_ct[64] = {0};   // out_ct: component class 0 float / 1 uint / 2 sint
    for (size_t i = 5; i < words;) { uint32_t n = code[i] >> 16, op = code[i] & 0xffff; if (!n || i + n > words) { free(loc); free(bi); return NULL; } const uint32_t *x = code + i;
        if (op == OpDecorate && n >= 4 && x[1] < bound) { if (x[2] == DecLocation) loc[x[1]] = (int32_t)x[3]; else if (x[2] == DecBuiltIn) { bi[x[1]] = 1; if (x[3] == 42 /* VertexIndex */) vid = x[1]; } }
        else if (op == OpEntryPoint && n >= 3 && !entry) entry = x[2];
        else if (op == OpTypeInt && n >= 4 && x[2] == 32) { if (x[3]) { if (!t_int) t_int = x[1]; } else if (!t_uint) t_uint = x[1]; }
        i += n; }
    for (size_t i = 5; i < words;) { uint32_t n = code[i] >> 16, op = code[i] & 0xffff; const uint32_t *x = code + i;
        if (op == OpVariable && n >= 4 && x[2] < bound) {
            const uint32_t *p = def_of(code, words, x[1]); uint32_t pointee = p && (p[0] & 0xffff) == OpTypePointer ? p[3] : 0;
            if (x[2] == vid) vid_ty = pointee;
            if (x[3] == SC_Output && loc[x[2]] >= 0 && loc[x[2]] < 64 && !bi[x[2]] && pointee) {
                const uint32_t *t = def_of(code, words, pointee), *c = t; uint32_t nc = 1;
                if (t && (t[0] & 0xffff) == OpTypeVector) { nc = t[3]; c = def_of(code, words, t[2]); }
                if (c && ((c[0] & 0xffff) == OpTypeFloat || (c[0] & 0xffff) == OpTypeInt) && c[2] == 32) {
                    int l = loc[x[2]]; outv[l] = x[2]; out_ty[l] = pointee; out_nc[l] = nc; out_ct[l] = (c[0] & 0xffff) == OpTypeFloat ? 0 : c[3] ? 2 : 1;
                    if (nc > 1) out_ct[l] |= (uint32_t)t[2] << 2; else out_ct[l] |= (uint32_t)pointee << 2; } }   // the component type id, above the class bits
        }
        i += n; }
    free(loc); free(bi);
    if (!entry) return NULL;
    int pre13 = code[1] < 0x00010300u, pre14 = code[1] < 0x00010400u; uint32_t sc = pre13 ? 2u /* Uniform */ : (uint32_t)SC_StorageBuffer;
    uint32_t nb = bound;
    uint32_t n_uint = t_uint ? 0 : nb++, n_int = (vid || t_int) ? 0 : nb++; if (!t_uint) t_uint = n_uint; if (!t_int && !vid) t_int = n_int;
    uint32_t c[6]; for (int k = 0; k < 6; k++) c[k] = nb++;
    uint32_t t_rarr = nb++, t_sbs = nb++, t_psbs = nb++, t_pu = nb++, t_pcs = nb++, t_ppcs = nb++, t_ppu = nb++, v_pc = nb++, v_sb[4] = {0};
    for (int b = 0; b < 4; b++) if (tf[b][1]) v_sb[b] = nb++;
    uint32_t t_pint = 0; if (!vid) { t_pint = nb++; vid = nb++; vid_ty = t_int; }
    size_t cap = words * 2 + 1024; uint32_t *o = malloc(4 * cap); size_t w = 5; memcpy(o, code, 20);
    int decorated = 0, typed = 0, in_entry = 0;
    #define E(...) do { uint32_t e_[] = { __VA_ARGS__ }; size_t k_ = sizeof e_ / 4; if (w + k_ + 8 > cap) { cap = cap * 2 + k_; o = realloc(o, 4 * cap); } memcpy(o + w, e_, 4 * k_); w += k_; } while (0)
    for (size_t i = 5; i < words;) { uint32_t n = code[i] >> 16, op = code[i] & 0xffff; const uint32_t *x = code + i;
        if (w + n + 256 > cap) { cap = cap * 2 + n; o = realloc(o, 4 * cap); }
        if (op == OpEntryPoint && x[2] == entry) {   // the interface: the vertex index (Input), and before SPIR-V 1.4 only Input / Output variables
            size_t at = w; memcpy(o + w, x, 4 * n); w += n;
            if (t_pint) o[w++] = vid;
            if (!pre14) { o[w++] = v_pc; for (int b = 0; b < 4; b++) if (v_sb[b]) o[w++] = v_sb[b]; }
            o[at] = (uint32_t)(w - at) << 16 | op; i += n; continue; }
        if (!decorated && op >= 19 && op <= 39) {   // the first type: annotations end here
            decorated = 1;
            E(4u << 16 | OpDecorate, t_rarr, 6 /* ArrayStride */, 4); E(3u << 16 | OpDecorate, t_sbs, pre13 ? (uint32_t)DecBufferBlock : (uint32_t)DecBlock); E(5u << 16 | 72, t_sbs, 0, 35 /* Offset */, 0);
            E(3u << 16 | OpDecorate, t_pcs, DecBlock); for (uint32_t m = 0; m < 5; m++) E(5u << 16 | 72, t_pcs, m, 35, 4 * m);
            for (int b = 0; b < 4; b++) if (v_sb[b]) { E(4u << 16 | OpDecorate, v_sb[b], DecDescriptorSet, 0); E(4u << 16 | OpDecorate, v_sb[b], DecBinding, binding0 + (uint32_t)b); }
            if (t_pint) E(4u << 16 | OpDecorate, vid, DecBuiltIn, 42);
        }
        if (!typed && op == 54 /* OpFunction */) {   // before the first function: the new types, constants and variables
            typed = 1;
            if (n_uint) E(4u << 16 | OpTypeInt, n_uint, 32, 0);
            if (n_int) E(4u << 16 | OpTypeInt, n_int, 32, 1);
            for (uint32_t k = 0; k < 6; k++) E(4u << 16 | OpConstant, t_uint, c[k], k);
            E(3u << 16 | OpTypeRuntimeArray, t_rarr, t_uint); E(3u << 16 | OpTypeStruct, t_sbs, t_rarr);
            E(4u << 16 | OpTypePointer, t_psbs, sc, t_sbs); E(4u << 16 | OpTypePointer, t_pu, sc, t_uint);
            E(7u << 16 | OpTypeStruct, t_pcs, t_uint, t_uint, t_uint, t_uint, t_uint);
            E(4u << 16 | OpTypePointer, t_ppcs, 9 /* PushConstant */, t_pcs); E(4u << 16 | OpTypePointer, t_ppu, 9, t_uint);
            E(4u << 16 | OpVariable, t_ppcs, v_pc, 9);
            for (int b = 0; b < 4; b++) if (v_sb[b]) E(4u << 16 | OpVariable, t_psbs, v_sb[b], sc);
            if (t_pint) { E(4u << 16 | OpTypePointer, t_pint, SC_Input, t_int); E(4u << 16 | OpVariable, t_pint, vid, SC_Input); }
        }
        if (op == 54 && n >= 3) in_entry = x[2] == entry;
        if (in_entry && op == 253 /* OpReturn */) {
            uint32_t vi = nb++, viu = vi, pf = nb++, first = nb++, rel = nb++;
            E(4u << 16 | 61 /* OpLoad */, vid_ty, vi, vid);
            if (vid_ty != t_uint) { viu = nb++; E(4u << 16 | 124 /* OpBitcast */, t_uint, viu, vi); }
            E(5u << 16 | 65, t_ppu, pf, v_pc, c[0]); E(4u << 16 | 61, t_uint, first, pf); E(5u << 16 | 130 /* OpISub */, t_uint, rel, viu, first);
            for (int b = 0; b < 4; b++) { if (!v_sb[b]) continue; int l = tf[b][0], nc = tf[b][1];
                uint32_t pb = nb++, base = nb++, m = nb++, row = nb++, val = 0;
                E(5u << 16 | 65, t_ppu, pb, v_pc, c[1 + b]); E(4u << 16 | 61, t_uint, base, pb);
                E(5u << 16 | 132 /* OpIMul */, t_uint, m, rel, c[nc]); E(5u << 16 | 128 /* OpIAdd */, t_uint, row, m, base);
                if (l >= 0 && l < 64 && outv[l]) { val = nb++; E(4u << 16 | 61, out_ty[l], val, outv[l]); }
                for (int k = 0; k < nc; k++) {
                    uint32_t bits = c[0];
                    if (val && (uint32_t)k < out_nc[l]) {
                        uint32_t comp = val; if (out_nc[l] > 1) { comp = nb++; E(5u << 16 | 81 /* OpCompositeExtract */, out_ct[l] >> 2, comp, val, (uint32_t)k); }
                        bits = comp; if ((out_ct[l] & 3) != 1) { bits = nb++; E(4u << 16 | 124, t_uint, bits, comp); } }
                    uint32_t idx = nb++, p = nb++; E(5u << 16 | 128, t_uint, idx, row, c[k]); E(6u << 16 | 65, t_pu, p, v_sb[b], c[0], idx); E(3u << 16 | 62 /* OpStore */, p, bits);
                }
            }
        }
        memcpy(o + w, x, 4 * n); w += n; i += n;
    }
    #undef E
    if (!typed || !decorated) { free(o); return NULL; }
    o[3] = nb; *out_words = w;
    return o;
}

// Storage images without a format that the shader writes / reads need StorageImageWriteWithoutFormat / StorageImageReadWithoutFormat, which the translator does not always
// declare (Pokémon Legends: Arceus compute shaders: spirv-val error on OpImageWrite; a strict driver may refuse the module). A copy with the missing ones added; NULL if none is missing
uint32_t *spv_image_caps(const uint32_t *code, size_t words, size_t *out_words) {
    if (words < 5 || code[0] != 0x07230203u) return NULL;
    int has_w = 0, has_r = 0, unfmt = 0, wr = 0, rd = 0; size_t last_cap = 0;
    for (size_t i = 5; i < words;) { uint32_t n = code[i] >> 16, op = code[i] & 0xffff; if (!n || i + n > words) return NULL; const uint32_t *x = code + i;
        if (op == 17 /* OpCapability */ && n == 2) { if (x[1] == 56) has_w = 1; if (x[1] == 55) has_r = 1; last_cap = i + n; }
        else if (op == OpTypeImage && n >= 9 && x[7] == 2 && x[8] == 0) unfmt = 1;   // a storage image with format Unknown
        else if (op == 99 /* OpImageWrite */) wr = 1;
        else if (op == 98 /* OpImageRead */) rd = 1;
        i += n; }
    int add_w = unfmt && wr && !has_w, add_r = unfmt && rd && !has_r;
    if (!(add_w || add_r) || !last_cap) return NULL;
    uint32_t *o = malloc(4 * (words + 4)); size_t w = 0;
    memcpy(o, code, 4 * last_cap); w = last_cap;
    if (add_w) { o[w++] = 2u << 16 | 17; o[w++] = 56; }
    if (add_r) { o[w++] = 2u << 16 | 17; o[w++] = 55; }
    memcpy(o + w, code + last_cap, 4 * (words - last_cap)); w += words - last_cap;
    *out_words = w; return o;
}
