#version 450
// Frame generation: how different the two frames are, into one texel: the difference the finest level's vectors leave against the picture's own contrast there
// (both summed over a 64x36 grid of samples). Near 0: the same scene, moved. Near 1: different pictures (a cut, a fade through white): fg_gen.frag then shows
// the previous frame again instead of a mix of two scenes
layout(location = 0) in vec2 uv;
layout(location = 0) out vec4 o;
layout(set = 0, binding = 0) uniform sampler2D f;
void main() {
    vec2 s = vec2(0.0);
    for (int j = 0; j < 36; j++) for (int i = 0; i < 64; i++) s += textureLod(f, (vec2(float(i), float(j)) + 0.5) / vec2(64.0, 36.0), 0.0).zw;
    o = vec4(s.x / (s.y + 1e-3 * 2304.0), s.x / 2304.0, s.y / 2304.0, 0.0);
}
