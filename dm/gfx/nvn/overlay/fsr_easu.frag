#version 450
// AMD FidelityFX Super Resolution 1, EASU (the upscale), for the game picture (nvn_vk.c scale_draw_view, DM_SCALER=fsr): the game frame upscaled into an image the
//   size of the picture on screen; RCAS (fsr_rcas.frag) then sharpens that onto the screen. AMD's headers as published: dm/third_party/fsr1 (MIT, license.txt there).
// The vertex shader is the sharp upscaler's (scale.vert) with its crop 0..1, so uv is the position inside the picture's box: ip = the output pixel
layout(location = 0) in vec2 uv;
layout(location = 0) out vec4 o;
layout(set = 0, binding = 0) uniform sampler2D tex;
// (the first four: scale.vert's; out.xy = the output's size in pixels, con3.z = 1 when the game frame's origin is lower left: its rows read flipped)
layout(push_constant) uniform P { vec4 src; vec4 outsz; vec4 box; vec4 rot; uvec4 con0; uvec4 con1; uvec4 con2; uvec4 con3; } p;
#define A_GPU 1
#define A_GLSL 1
#include "ffx_a.h"
#define FSR_EASU_F 1
AF4 FsrEasuRF(AF2 q) { return textureGather(tex, q, 0); }
AF4 FsrEasuGF(AF2 q) { return textureGather(tex, q, 1); }
AF4 FsrEasuBF(AF2 q) { return textureGather(tex, q, 2); }
#include "ffx_fsr1.h"
void main() {
    uvec2 ip = uvec2(clamp(uv * p.outsz.xy, vec2(0.0), p.outsz.xy - 1.0));
    if (p.con3.z != 0u) ip.y = uint(p.outsz.y) - 1u - ip.y;
    AF3 c; FsrEasuF(c, ip, p.con0, p.con1, p.con2, p.con3);
    o = vec4(c, 1.0);
}
