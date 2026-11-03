#version 450
// Snapdragon Game Super Resolution 1 (dm/third_party/sgsr, Qualcomm, BSD-3-Clause) as the game picture's upscaler: the same triangle as scale.vert, feeding the
// shader's in_TEXCOORD0; its ViewportInfo (1 / width, 1 / height, width, height of the game frame) leads the push constants
layout(location = 0) out highp vec4 in_TEXCOORD0;
layout(push_constant) uniform P { vec4 vp; vec4 src; vec4 box; vec4 rot; } p;
vec2 turn(vec2 v, float r) { return r < 0.5 ? v : r < 1.5 ? vec2(-v.y, v.x) : r < 2.5 ? -v : vec2(v.y, -v.x); }
void main() {
    vec2 c = vec2((gl_VertexIndex << 1) & 2, gl_VertexIndex & 2);
    in_TEXCOORD0 = vec4(mix(p.src.xy, p.src.zw, c), 0.0, 0.0);
    gl_Position = vec4(turn(mix(p.box.xy, p.box.zw, c), p.rot.x), 0.0, 1.0);
}
