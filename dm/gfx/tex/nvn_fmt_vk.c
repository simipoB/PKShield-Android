// NVNformat → Vulkan, entry by entry in NVN enum order, mirroring nvn_fmt_mtl.c (which documents each substitution); the packed 16-bit formats take the Vulkan format with the
// same bit layout as the Metal one there. Unlisted formats (0 and out of range) return 0.
#include "nvn_fmt_vk.h"

enum {   // VkFormat values (vulkan_core.h)
    R4G4B4A4 = 2, B5G6R5 = 5, B5G5R5A1 = 6, A1R5G5B5 = 8, R8U = 9, R8S = 10, R8US = 11, R8SS = 12, R8UI = 13, R8I = 14,
    RG8U = 16, RG8S = 17, RG8US = 18, RG8SS = 19, RG8UI = 20, RG8I = 21, RGB8U = 23, RGB8S = 24, RGB8US = 25, RGB8SS = 26, RGB8UI = 27, RGB8I = 28,
    RGBA8U = 37, RGBA8S = 38, RGBA8US = 39, RGBA8SS = 40, RGBA8UI = 41, RGBA8I = 42, RGBA8SRGB = 43, BGRA8U = 44, BGRA8SRGB = 50,
    A2B10G10R10U = 64, A2B10G10R10S = 65, A2B10G10R10UI = 68, A2B10G10R10I = 69,
    R16U = 70, R16S = 71, R16US = 72, R16SS = 73, R16UI = 74, R16I = 75, R16F = 76, RG16U = 77, RG16S = 78, RG16US = 79, RG16SS = 80, RG16UI = 81, RG16I = 82, RG16F = 83,
    RGB16U = 84, RGB16S = 85, RGB16US = 86, RGB16SS = 87, RGB16UI = 88, RGB16I = 89, RGB16F = 90,
    RGBA16U = 91, RGBA16S = 92, RGBA16US = 93, RGBA16SS = 94, RGBA16UI = 95, RGBA16I = 96, RGBA16F = 97,
    R32UI = 98, R32I = 99, R32F = 100, RG32UI = 101, RG32I = 102, RG32F = 103, RGB32UI = 104, RGB32I = 105, RGB32F = 106, RGBA32UI = 107, RGBA32I = 108, RGBA32F = 109,
    B10G11R11F = 122, E5B9G9R9F = 123, D16 = 124, X8D24 = 125, D32F = 126, S8 = 127, D24S8 = 129, D32FS8 = 130,
    BC1RGBA = 133, BC1RGBA_SRGB = 134, BC2 = 135, BC2_SRGB = 136, BC3 = 137, BC3_SRGB = 138, BC4U = 139, BC4S = 140, BC5U = 141, BC5S = 142,
    BC6HU = 143, BC6HS = 144, BC7 = 145, BC7_SRGB = 146, ASTC0 = 157,   // ASTC 4x4 UNORM; each footprint has UNORM then SRGB, in NVN's order
};
static int g_x8d24, g_d24s8;   // the device's 24-bit depth formats work as depth targets (nvn_fmt_vk_depth24)
void nvn_fmt_vk_depth24(int has_x8d24, int has_d24s8) { g_x8d24 = has_x8d24; g_d24s8 = has_d24s8; }
static int g_bc = 1;
void nvn_fmt_vk_bc(int has_bc) { g_bc = has_bc; }
// BC Vulkan format (131–146) → decode kind (bc_decode.h) and the format it decodes into
static int bc_kind(uint32_t vk) {
    static const int8_t k[16] = { 1, 1, 1, 1, 2, 2, 3, 3, 4, -4, 5, -5, 6, -6, 7, 7 };
    return vk >= 131 && vk <= 146 ? k[vk - 131] : 0;
}
static uint32_t bc_plain(uint32_t vk) {
    switch (bc_kind(vk)) {
    case 4: return 9; case -4: return 10; case 5: return 16; case -5: return 17; case 6: case -6: return 97;   // R8 / R8 snorm / RG8 / RG8 snorm / RGBA16F
    default: return vk == 132 || vk == 134 || vk == 136 || vk == 138 || vk == 146 ? 43 : 37;                   // RGBA8 sRGB / UNORM
    }
}

typedef struct { uint16_t vk; uint8_t kind, noalpha, rb; } ent;
#define E(m) {m, 1, 0, 0}
#define X(m) {m, 1, 1, 0}
#define S(m) {m, 2, 0, 0}
#define SX(m) {m, 2, 1, 0}
#define N {0, 0, 0, 0}
#define SR(m) {m, 2, 0, 1}
#define SXR(m) {m, 2, 1, 1}
static const ent T[0x9D] = {
    [0x01] = E(R8U), E(R8S), E(R8UI), E(R8I),
    [0x05] = E(R16F), E(R16U), E(R16S), E(R16UI), E(R16I),
    [0x0A] = E(R32F), E(R32UI), E(R32I),
    [0x0D] = E(RG8U), E(RG8S), E(RG8UI), E(RG8I),
    [0x11] = E(RG16F), E(RG16U), E(RG16S), E(RG16UI), E(RG16I),
    [0x16] = E(RG32F), E(RG32UI), E(RG32I),
    [0x19] = N, N, N, N, N, N, N, N, N, N, N, N,   // three-component textures: as in nvn_fmt_mtl.c, not sampled as such
    [0x25] = E(RGBA8U), E(RGBA8S), E(RGBA8UI), E(RGBA8I),
    [0x29] = E(RGBA16F), E(RGBA16U), E(RGBA16S), E(RGBA16UI), E(RGBA16I),
    [0x2E] = E(RGBA32F), E(RGBA32UI), E(RGBA32I),
    [0x31] = E(S8), E(D16),
    [0x33] = S(D32F), E(D32F),            // DEPTH24: X8_D24 when the device has it (nvn_fmt_to_vk)
    [0x35] = S(D32FS8), E(D32FS8),        // DEPTH24_STENCIL8: D24_S8 when the device has it
    [0x37] = X(RGBA8SRGB), E(RGBA8SRGB),
    [0x39] = S(R4G4B4A4), SXR(B5G5R5A1), SR(B5G5R5A1), SR(B5G6R5),
    [0x3D] = E(A2B10G10R10U), E(A2B10G10R10UI), E(B10G11R11F), E(E5B9G9R9F),
    [0x41] = X(BC1RGBA), E(BC1RGBA), E(BC2), E(BC3), X(BC1RGBA_SRGB), E(BC1RGBA_SRGB), E(BC2_SRGB), E(BC3_SRGB),
    [0x49] = E(BC4U), E(BC4S), E(BC5U), E(BC5S), E(BC7), E(BC7_SRGB), E(BC6HS), E(BC6HU),
    [0x51] = S(R8UI), S(R8I), S(R16UI), S(R16I), S(R32UI), S(R32I),
    [0x57] = S(RG8UI), S(RG8I), S(RG16UI), S(RG16I), S(RG32UI), S(RG32I),
    [0x5D] = N, N, N, N, N, N,
    [0x63] = S(RGBA8UI), S(RGBA8I), S(RGBA16UI), S(RGBA16I), S(RGBA32UI), S(RGBA32I),
    [0x69] = E(A2B10G10R10S), E(A2B10G10R10I), S(A2B10G10R10UI), S(A2B10G10R10I),
    [0x6D] = X(RGBA8U), X(RGBA8S), X(RGBA8UI), X(RGBA8I),
    [0x71] = X(RGBA16F), X(RGBA16U), X(RGBA16S), X(RGBA16UI), X(RGBA16I),
    [0x76] = X(RGBA32F), X(RGBA32UI), X(RGBA32I),
    [0x95] = S(B5G6R5), SX(B5G5R5A1), S(B5G5R5A1), SR(A1R5G5B5),
    [0x99] = X(BGRA8U), E(BGRA8U), X(BGRA8SRGB), E(BGRA8SRGB),
};

int nvn_fmt_vk_decode(int f) {
    if (g_bc || f < 0x41 || f > 0x50 || f >= (int)(sizeof T / sizeof *T)) return 0;
    return T[f].kind ? bc_kind(T[f].vk) : 0;
}
int nvn_fmt_to_vk(int f, uint32_t *vk, uint8_t swz[4]) {
    ent e = {0, 0, 0, 0};
    if (f >= 0x79 && f <= 0x94) { int k = (f - 0x79) % 14; e = (ent){(uint16_t)(ASTC0 + 2 * k + (f >= 0x87)), 1, 0, 0}; }
    else if (f > 0 && f < (int)(sizeof T / sizeof *T)) e = T[f];
    if (f == 0x33 && g_x8d24) e = (ent){X8D24, 1, 0, 0};
    if (f == 0x35 && g_d24s8) e = (ent){D24S8, 1, 0, 0};
    if (vk) *vk = e.kind ? (!g_bc && bc_kind(e.vk) ? bc_plain(e.vk) : e.vk) : 0;
    if (swz) { swz[0] = e.rb ? NVN_SWZ_B : NVN_SWZ_R; swz[1] = NVN_SWZ_G; swz[2] = e.rb ? NVN_SWZ_R : NVN_SWZ_B; swz[3] = e.noalpha ? NVN_SWZ_ONE : NVN_SWZ_A; }
    return e.kind;
}

uint32_t nvn_vfmt_to_vk(int f, int *ct) {
    static const struct { uint8_t f; uint16_t vk; uint8_t ct; } V[] = {
        {0x01, R8U, 0}, {0x02, R8S, 0}, {0x03, R8UI, 1}, {0x04, R8I, 2}, {0x05, R16F, 0}, {0x06, R16U, 0}, {0x07, R16S, 0}, {0x08, R16UI, 1}, {0x09, R16I, 2},
        {0x0A, R32F, 0}, {0x0B, R32UI, 1}, {0x0C, R32I, 2},
        {0x0D, RG8U, 0}, {0x0E, RG8S, 0}, {0x0F, RG8UI, 1}, {0x10, RG8I, 2}, {0x11, RG16F, 0}, {0x12, RG16U, 0}, {0x13, RG16S, 0}, {0x14, RG16UI, 1}, {0x15, RG16I, 2},
        {0x16, RG32F, 0}, {0x17, RG32UI, 1}, {0x18, RG32I, 2},
        {0x19, RGB8U, 0}, {0x1A, RGB8S, 0}, {0x1B, RGB8UI, 1}, {0x1C, RGB8I, 2}, {0x1D, RGB16F, 0}, {0x1E, RGB16U, 0}, {0x1F, RGB16S, 0}, {0x20, RGB16UI, 1}, {0x21, RGB16I, 2},
        {0x22, RGB32F, 0}, {0x23, RGB32UI, 1}, {0x24, RGB32I, 2},
        {0x25, RGBA8U, 0}, {0x26, RGBA8S, 0}, {0x27, RGBA8UI, 1}, {0x28, RGBA8I, 2}, {0x29, RGBA16F, 0}, {0x2A, RGBA16U, 0}, {0x2B, RGBA16S, 0}, {0x2C, RGBA16UI, 1}, {0x2D, RGBA16I, 2},
        {0x2E, RGBA32F, 0}, {0x2F, RGBA32UI, 1}, {0x30, RGBA32I, 2},
        {0x3D, A2B10G10R10U, 0}, {0x69, A2B10G10R10S, 0}, {0x3F, B10G11R11F, 0}, {0x9A, BGRA8U, 0},
        // *_UI2F / *_I2F: integers converted to float unnormalized = Vulkan's USCALED / SSCALED (32-bit ones have none: read as integers, see nvn_vk.c)
        {0x51, R8US, 0}, {0x52, R8SS, 0}, {0x53, R16US, 0}, {0x54, R16SS, 0}, {0x55, R32UI, 1}, {0x56, R32I, 2},
        {0x57, RG8US, 0}, {0x58, RG8SS, 0}, {0x59, RG16US, 0}, {0x5A, RG16SS, 0}, {0x5B, RG32UI, 1}, {0x5C, RG32I, 2},
        {0x5D, RGB8US, 0}, {0x5E, RGB8SS, 0}, {0x5F, RGB16US, 0}, {0x60, RGB16SS, 0}, {0x61, RGB32UI, 1}, {0x62, RGB32I, 2},
        {0x63, RGBA8US, 0}, {0x64, RGBA8SS, 0}, {0x65, RGBA16US, 0}, {0x66, RGBA16SS, 0}, {0x67, RGBA32UI, 1}, {0x68, RGBA32I, 2},
    };
    for (unsigned i = 0; i < sizeof V / sizeof *V; i++) if (V[i].f == f) { if (ct) *ct = V[i].ct; return V[i].vk; }
    if (ct) *ct = 0;
    return 0;
}
