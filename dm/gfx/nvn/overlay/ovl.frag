#version 450
// shape.x: 0 disc, 1 ring, 2 rounded rectangle; shape.y: ring / border width in uv units; shape.z: corner radius (rectangles); shape.w: rectangle aspect (w / h)
// glyph.x: atlas cell (-1 none), glyph.y: glyph scale (fraction of the control), glyph.z: glyph alpha
layout(push_constant) uniform P { vec4 rect; vec4 color; vec4 shape; vec4 glyph; vec2 target; } p;
layout(binding = 0) uniform sampler2D font;
layout(location = 0) in vec2 uv;
layout(location = 0) out vec4 o;
void main() {
    float aa = 2.0 / min(p.rect.z, p.rect.w) * 2.0;
    float a;
    if (p.shape.x < 0.5) { float d = length(uv); a = 1.0 - smoothstep(1.0 - aa, 1.0, d); }
    else if (p.shape.x < 1.5) { float d = length(uv); a = (1.0 - smoothstep(1.0 - aa, 1.0, d)) * smoothstep(1.0 - p.shape.y - aa, 1.0 - p.shape.y, d); }
    else { vec2 q = abs(uv) * vec2(p.shape.w, 1.0); vec2 b = vec2(p.shape.w, 1.0) - p.shape.z; float d = length(max(q - b, 0.0)) - p.shape.z; a = 1.0 - smoothstep(-aa, 0.0, d); }
    vec4 col = vec4(p.color.rgb, p.color.a * a);
    if (p.glyph.x >= 0.0) {   // the label: a 16x16 cell of a 16-cell-wide atlas, centred, bilinear then sharpened
        vec2 g = (uv / p.glyph.y) * vec2(p.shape.x > 1.5 ? p.shape.w : 1.0, 1.0) * 0.5 + 0.5;
        if (all(greaterThanEqual(g, vec2(0.0))) && all(lessThanEqual(g, vec2(1.0)))) {
            vec2 t = (vec2(mod(p.glyph.x, 16.0), floor(p.glyph.x / 16.0)) + g) / vec2(16.0, 1.0);
            float s = texture(font, t).r; float ga = smoothstep(0.35, 0.65, s) * p.glyph.z;
            col.rgb = mix(col.rgb, vec3(1.0), ga); col.a = max(col.a, ga);
        }
    }
    o = vec4(col.rgb * col.a, col.a);   // premultiplied
}
