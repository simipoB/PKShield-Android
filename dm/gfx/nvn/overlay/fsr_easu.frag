#version 450
// FSR 1.0 EASU (AMD FidelityFX Super Resolution 1.0, MIT: dm/third_party/fsr1/license.txt), the 32-bit path: the game frame upscaled into an intermediate image
// the size of the picture on screen. con0..con3 come from FsrEasuConOffset on the CPU (nvn_vk.c fsr_con). org: z = the output height, w = 1 when the game
// frame's origin is lower left (the output rows are then taken from the bottom up)
#extension GL_GOOGLE_include_directive : require
#define A_GPU 1
#define A_GLSL 1
#include "ffx_a.h"
layout(set = 0, binding = 0) uniform sampler2D src;
layout(push_constant) uniform P { uvec4 con0; uvec4 con1; uvec4 con2; uvec4 con3; vec4 box; ivec4 org; } p;
layout(location = 0) out vec4 col;
AF4 FsrEasuRF(AF2 q) { return textureGather(src, q, 0); }
AF4 FsrEasuGF(AF2 q) { return textureGather(src, q, 1); }
AF4 FsrEasuBF(AF2 q) { return textureGather(src, q, 2); }
#define FSR_EASU_F 1
#include "ffx_fsr1.h"
void main() {
    AU2 ip = AU2(gl_FragCoord.xy);
    if (p.org.w != 0) ip.y = AU1(p.org.z - 1) - ip.y;
    AF3 c;
    FsrEasuF(c, ip, p.con0, p.con1, p.con2, p.con3);
    col = vec4(c, 1.0);
}
