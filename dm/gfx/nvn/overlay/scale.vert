#version 450
// The game picture's upscaler (nvn_vk.c scale_draw): one triangle over the picture's box (box: x0 y0 x1 y1 in normalized device coordinates of the unrotated
// window); uv runs over the game frame's crop (flipped when its origin is lower left); rot = the swapchain's pre-rotation in quarter turns (0: none)
layout(location = 0) out vec2 uv;
layout(push_constant) uniform P { vec4 src; vec4 tex; vec4 box; vec4 rot; } p;   // src: u0 v0 u1 v1 of the crop; tex: 1 / width, 1 / height, sharpness, unused
vec2 turn(vec2 v, float r) { return r < 0.5 ? v : r < 1.5 ? vec2(-v.y, v.x) : r < 2.5 ? -v : vec2(v.y, -v.x); }
void main() {
    vec2 c = vec2((gl_VertexIndex << 1) & 2, gl_VertexIndex & 2);
    uv = mix(p.src.xy, p.src.zw, c);
    gl_Position = vec4(turn(mix(p.box.xy, p.box.zw, c), p.rot.x), 0.0, 1.0);
}
