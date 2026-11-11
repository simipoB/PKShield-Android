#version 450
// Frame generation, the brightness pyramid: four bilinear taps around the centre, so a target a quarter of the frame's size (tap = one frame pixel) averages its 4x4
// pixels and each further level, half the size (tap = half a texel), its 2x2. p.a.z = 1: the source is the colour frame; 0: a brightness level
layout(location = 0) in vec2 uv;
layout(location = 0) out vec4 o;
layout(set = 0, binding = 0) uniform sampler2D s;
layout(push_constant) uniform P { vec4 a; } p;
void main() {
    vec2 t = p.a.xy;
    vec4 c = 0.25 * (texture(s, uv + vec2(-t.x, -t.y)) + texture(s, uv + vec2(t.x, -t.y)) + texture(s, uv + vec2(-t.x, t.y)) + texture(s, uv + vec2(t.x, t.y)));
    o = vec4(p.a.z > 0.5 ? dot(c.rgb, vec3(0.299, 0.587, 0.114)) : c.r);
}
