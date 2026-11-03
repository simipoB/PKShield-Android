#version 450
// Touch overlay (nvn_vk.c ovl_draw): one quad per control, from its rectangle in pixels and the target size in the push constants; rot = the swapchain's
// pre-rotation in quarter turns (0 when the system rotates the picture), applied to the final position so the controls and their labels turn with it
layout(push_constant) uniform P { vec4 rect; vec4 color; vec4 shape; vec4 glyph; vec2 target; float rot; } p;
layout(location = 0) out vec2 uv;
vec2 turn(vec2 v, float r) { return r < 0.5 ? v : r < 1.5 ? vec2(-v.y, v.x) : r < 2.5 ? -v : vec2(v.y, -v.x); }
void main() {
    vec2 c = vec2((gl_VertexIndex == 1 || gl_VertexIndex == 2 || gl_VertexIndex == 4) ? 1.0 : 0.0, (gl_VertexIndex == 2 || gl_VertexIndex == 4 || gl_VertexIndex == 5) ? 1.0 : 0.0);
    vec2 px = p.rect.xy + c * p.rect.zw;
    uv = c * 2.0 - 1.0;
    gl_Position = vec4(turn(px / p.target * 2.0 - 1.0, p.rot), 0.0, 1.0);
}
