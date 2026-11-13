#version 450
// FSR 1.0 RCAS (AMD FidelityFX Super Resolution 1.0, MIT: dm/third_party/fsr1/license.txt), the 32-bit path: EASU's picture sharpened into the screen's box.
// con0 comes from FsrRcasCon on the CPU (nvn_vk.c fsr_con); org.xy = the box's top-left corner in the screen image
#extension GL_GOOGLE_include_directive : require
#define A_GPU 1
#define A_GLSL 1
#include "ffx_a.h"
layout(set = 0, binding = 0) uniform sampler2D src;
layout(push_constant) uniform P { uvec4 con0; uvec4 con1; uvec4 con2; uvec4 con3; vec4 box; ivec4 org; } p;
layout(location = 0) out vec4 col;
AF4 FsrRcasLoadF(ASU2 q) { return texelFetch(src, q, 0); }
void FsrRcasInputF(inout AF1 r, inout AF1 g, inout AF1 b) {}
#define FSR_RCAS_F 1
#include "ffx_fsr1.h"
void main() {
    AU2 ip = AU2(ivec2(gl_FragCoord.xy) - p.org.xy);
    AF3 c;
    FsrRcasF(c.r, c.g, c.b, ip, p.con0);
    col = vec4(c, 1.0);
}
