#version 450
// Frame generation (nvn_fgen.h): one triangle over the whole target; uv runs 0..1 over it
layout(location = 0) out vec2 uv;
void main() {
    vec2 c = vec2((gl_VertexIndex << 1) & 2, gl_VertexIndex & 2);
    uv = c;
    gl_Position = vec4(c * 2.0 - 1.0, 0.0, 1.0);
}
