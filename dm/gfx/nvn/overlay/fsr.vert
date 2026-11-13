#version 450
// FSR 1.0's two passes (nvn_vk.c fsr_draw): one triangle over the target box (box: x0 y0 x1 y1 in normalized device coordinates of the target). The fragment
// shaders work from gl_FragCoord, so nothing else is passed on
layout(push_constant) uniform P { uvec4 con0; uvec4 con1; uvec4 con2; uvec4 con3; vec4 box; ivec4 org; } p;
void main() {
    vec2 c = vec2((gl_VertexIndex << 1) & 2, gl_VertexIndex & 2);
    gl_Position = vec4(mix(p.box.xy, p.box.zw, c), 0.0, 1.0);
}
