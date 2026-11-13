#version 450
// AMD FidelityFX Super Resolution 1, RCAS (the sharpening), for the game picture (nvn_vk.c scale_draw_view, DM_SCALER=fsr): the EASU image (fsr_easu.frag), the size
//   of the picture on screen, sharpened onto the screen. AMD's headers as published: dm/third_party/fsr1 (MIT, license.txt there).
// The vertex shader is scale.vert with its crop 0..1: uv is the position inside the picture's box, ip the pixel of the EASU image
layout(location = 0) in vec2 uv;
layout(location = 0) out vec4 o;
layout(set = 0, binding = 0) uniform sampler2D tex;
// (outsz.xy = the EASU image's size in pixels, con0 = FsrRcasCon's)
layout(push_constant) uniform P { vec4 src; vec4 outsz; vec4 box; vec4 rot; uvec4 con0; uvec4 con1; uvec4 con2; uvec4 con3; } p;
#define A_GPU 1
#define A_GLSL 1
#include "ffx_a.h"
#define FSR_RCAS_F 1
AF4 FsrRcasLoadF(ASU2 q) { return texelFetch(tex, clamp(ivec2(q), ivec2(0), ivec2(p.outsz.xy) - 1), 0); }   // (the edges: the nearest pixel)
void FsrRcasInputF(inout AF1 r, inout AF1 g, inout AF1 b) {}
#include "ffx_fsr1.h"
void main() {
    uvec2 ip = uvec2(clamp(uv * p.outsz.xy, vec2(0.0), p.outsz.xy - 1.0));
    AF1 r, g, b; FsrRcasF(r, g, b, ip, p.con0);
    o = vec4(r, g, b, 1.0);
}
