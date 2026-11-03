#version 450
// The game picture's upscaler (nvn_vk.c scale_draw): one triangle over the picture's box; uv runs over the game frame's crop (flipped when its origin is lower left)
layout(location = 0) out vec2 uv;
layout(push_constant) uniform P { vec4 src; vec4 tex; } p;   // src: u0 v0 u1 v1 of the crop, normalized; tex: 1 / width, 1 / height of the frame, sharpness, unused
void main() {
    vec2 c = vec2((gl_VertexIndex << 1) & 2, gl_VertexIndex & 2);
    uv = mix(p.src.xy, p.src.zw, c);
    gl_Position = vec4(c * 2.0 - 1.0, 0.0, 1.0);
}
