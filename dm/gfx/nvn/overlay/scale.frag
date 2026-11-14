#version 450
// The game picture's upscaler: bilinear, then contrast-adaptive sharpening at the game frame's own pixel size, so a 720p frame on a 1440p screen keeps its detail
// instead of the soft look of a plain stretch. The sharpening is weaker where the neighbourhood already has hard contrast (it would ring there) and the result is
// held inside the neighbourhood's own range (no bright or dark halos at edges).
layout(location = 0) in vec2 uv;
layout(location = 0) out vec4 o;
layout(set = 0, binding = 0) uniform sampler2D s;
layout(push_constant) uniform P { vec4 src; vec4 tex; } p;   // (tex.w: the colour, grade)
// The picture's colour (DM_COLOR, ☰ -> Colour): 0 as the game draws it; 1 Vibrant (muted colours lifted more than vivid ones, a touch of contrast: Hisui's grey haze);
//   2 Cinematic (stronger contrast, a slightly warm tone). Free here: the upscaler reads every pixel anyway
vec3 grade(vec3 c, float m) {
    if (m < 0.5) return c;
    float l = dot(c, vec3(0.2126, 0.7152, 0.0722));
    if (m < 1.5) { float sat = max(c.r, max(c.g, c.b)) - min(c.r, min(c.g, c.b)); c = mix(vec3(l), c, 1.0 + 0.35 * (1.0 - sat)); c = (c - 0.5) * 1.06 + 0.5; }
    else { c = (c - 0.5) * 1.12 + 0.5; c = mix(vec3(l), c, 1.08); c *= vec3(1.02, 1.0, 0.97); }
    return clamp(c, 0.0, 1.0);
}
void main() {
    vec2 t = p.tex.xy;
    vec3 c = texture(s, uv).rgb;
    vec3 n = texture(s, uv + vec2(0.0, -t.y)).rgb, d = texture(s, uv + vec2(0.0, t.y)).rgb;
    vec3 e = texture(s, uv + vec2(t.x, 0.0)).rgb, w = texture(s, uv + vec2(-t.x, 0.0)).rgb;
    vec3 lo = min(c, min(min(n, d), min(e, w))), hi = max(c, max(max(n, d), max(e, w)));
    vec3 room = clamp(min(lo, 1.0 - hi) / max(hi, vec3(1.0 / 1024.0)), 0.0, 1.0);   // how far this neighbourhood is from hard contrast
    vec3 k = sqrt(room) * p.tex.z;
    vec3 r = c + k * (4.0 * c - n - d - e - w) * 0.25;
    o = vec4(grade(clamp(r, lo, hi), p.tex.w), 1.0);
}
