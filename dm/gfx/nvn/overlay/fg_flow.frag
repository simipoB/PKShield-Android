#version 450
// Frame generation, the motion between two frames at one pyramid level, seen from the picture halfway between them: for each texel the vector v for which the
// previous frame at -v/2 and the current frame at +v/2 look the same (a 3x3 window of taps 1.5 texels apart, sum of absolute brightness differences). The search
// starts from the coarser level's vector (doubled) and tries every whole step within the radius; a parabola through the best step's neighbours gives the fraction.
// A vector found from the middle needs no hole filling: every pixel of the in-between picture has one. Out: xy = v in texels of this level, z = the mean
// difference left at v, w = the mean difference of the window's taps from its centre (how much there is to match)
layout(location = 0) in vec2 uv;
layout(location = 0) out vec4 o;
layout(set = 0, binding = 0) uniform sampler2D ya;   // the previous frame's brightness
layout(set = 0, binding = 1) uniform sampler2D yb;   // the current frame's
layout(set = 0, binding = 2) uniform sampler2D fc;   // the coarser level's vectors
layout(push_constant) uniform P { vec4 a; } p;       // 1 / width, 1 / height, search radius, 1 when there is a coarser level
float cost(vec2 v) {
    vec2 h = 0.5 * v * p.a.xy; float s = 0.0;
    for (int j = -1; j <= 1; j++) for (int i = -1; i <= 1; i++) {
        vec2 q = uv + vec2(float(i), float(j)) * 1.5 * p.a.xy;
        s += abs(textureLod(ya, q - h, 0.0).r - textureLod(yb, q + h, 0.0).r); }
    return s;
}
void main() {
    vec2 v0 = p.a.w > 0.5 ? 2.0 * textureLod(fc, uv, 0.0).xy : vec2(0.0);
    int r = int(p.a.z + 0.5);
    float best = 1e9, bc = 0.0; vec2 bd = vec2(0.0);
    for (int j = -r; j <= r; j++) for (int i = -r; i <= r; i++) {
        vec2 d = vec2(float(i), float(j)); float c = cost(v0 + d), k = c + 0.01 * length(d);   // (among equals the step nearest the coarser level's vector)
        if (k < best) { best = k; bc = c; bd = d; } }
    vec2 v = v0 + bd;
    float xm = cost(v - vec2(1.0, 0.0)), xp = cost(v + vec2(1.0, 0.0)), ym = cost(v - vec2(0.0, 1.0)), yp = cost(v + vec2(0.0, 1.0));
    float dx = xm - 2.0 * bc + xp, dy = ym - 2.0 * bc + yp;
    vec2 f = vec2(dx > 1e-4 ? clamp(0.5 * (xm - xp) / dx, -0.5, 0.5) : 0.0, dy > 1e-4 ? clamp(0.5 * (ym - yp) / dy, -0.5, 0.5) : 0.0);
    v += f;
    float c0 = cost(vec2(0.0));
    if (c0 < bc * 1.1 + 0.004) { v = vec2(0.0); bc = min(bc, c0); }   // what does not move stays exactly where it is (the HUD, a still camera)
    vec2 h = 0.5 * v * p.a.xy; float ca = textureLod(ya, uv - h, 0.0).r, cb = textureLod(yb, uv + h, 0.0).r, ct = 0.0;   // the window's own contrast, both frames
    for (int j = -1; j <= 1; j++) for (int i = -1; i <= 1; i++) {
        vec2 q = uv + vec2(float(i), float(j)) * 1.5 * p.a.xy;
        ct += abs(textureLod(ya, q - h, 0.0).r - ca) + abs(textureLod(yb, q + h, 0.0).r - cb); }
    o = vec4(v, bc / 9.0, ct / 16.0);
}
