#version 450
// Frame generation, the in-between picture: the previous frame taken half the motion back and the current frame half the motion forward, averaged. Where the two
// disagree (something uncovered, a match that failed) the plain average of the two frames stands in, which ghosts for one sixtieth of a second instead of tearing;
// a pixel that is the same in both frames is left alone (HUD, text, a still camera); when the frames are different pictures (fg_stat.frag) the previous one is shown
layout(location = 0) in vec2 uv;
layout(location = 0) out vec4 o;
layout(set = 0, binding = 0) uniform sampler2D a;    // the previous frame
layout(set = 0, binding = 1) uniform sampler2D b;    // the current frame
layout(set = 0, binding = 2) uniform sampler2D fl;   // the finest level's vectors
layout(set = 0, binding = 3) uniform sampler2D st;   // fg_stat.frag's texel
layout(push_constant) uniform P { vec4 a; } p;       // 1 / width, 1 / height of the vectors' level; z: 1 = show the vectors (DM_FRAMEGEN=debug)
float max3(vec3 v) { return max(v.x, max(v.y, v.z)); }
void main() {
    vec4 f = textureLod(fl, uv, 0.0);
    vec2 h = 0.5 * f.xy * p.a.xy, ua = uv - h, ub = uv + h;
    vec3 a0 = textureLod(a, uv, 0.0).rgb, b0 = textureLod(b, uv, 0.0).rgb, aw = textureLod(a, ua, 0.0).rgb, bw = textureLod(b, ub, 0.0).rgb;
    bool ina = all(greaterThanEqual(ua, vec2(0.0))) && all(lessThanEqual(ua, vec2(1.0))), inb = all(greaterThanEqual(ub, vec2(0.0))) && all(lessThanEqual(ub, vec2(1.0)));
    vec3 m = ina && inb ? mix(0.5 * (aw + bw), 0.5 * (a0 + b0), smoothstep(0.10, 0.30, max3(abs(aw - bw)))) : inb ? bw : ina ? aw : 0.5 * (a0 + b0);   // (off the edge on one side: the other frame alone)
    m = mix(m, b0, 1.0 - smoothstep(0.004, 0.016, max3(abs(a0 - b0))));
    m = mix(m, a0, smoothstep(0.70, 0.95, textureLod(st, vec2(0.5), 0.0).r));
    if (p.a.z > 0.5) m = mix(m, vec3(0.5) + vec3(f.xy * 0.1, -f.x * 0.1), 0.6);
    o = vec4(m, 1.0);
}
