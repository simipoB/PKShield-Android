#version 450
// Vertex colours as the translated game shaders take them: an RGBA8 unsigned-integer attribute (location 7), each component kept as its bits in a float variable
// (the translator's registers are floats), turned back into an integer later, converted to float and scaled by 1/255. Every shader of the white Mali scenes
// (#110, #111, #128) starts this way. Built without optimisation (-O0) so the driver's compiler sees the pattern as the game's shaders give it. V picks the variant:
//   0 the translator's pattern            1 the plain conversion, no bits in floats (the control)
//   2 the component copied into an integer variable first      3 the whole vector copied into a variable first
//   4 the component xor'd with a zero the compiler cannot see (a push constant) before its bits go into the float
layout(location = 0) in vec4 pos;
layout(location = 7) in uvec4 col;
layout(location = 2) out vec4 o;
layout(push_constant) uniform P { uint zero; } p;
void main() {
    gl_Position = vec4(pos.xy, 0.0, 1.0);
#if V == 1
    float c0 = float(col.x); float c1 = float(col.y); float c2 = float(col.z); float c3 = float(col.w);
#else
#if V == 0
    float t0 = uintBitsToFloat(col.x); float t1 = uintBitsToFloat(col.y); float t2 = uintBitsToFloat(col.z); float t3 = uintBitsToFloat(col.w);
#elif V == 2
    uint u0 = col.x; uint u1 = col.y; uint u2 = col.z; uint u3 = col.w;
    float t0 = uintBitsToFloat(u0); float t1 = uintBitsToFloat(u1); float t2 = uintBitsToFloat(u2); float t3 = uintBitsToFloat(u3);
#elif V == 3
    uvec4 c = col;
    float t0 = uintBitsToFloat(c.x); float t1 = uintBitsToFloat(c.y); float t2 = uintBitsToFloat(c.z); float t3 = uintBitsToFloat(c.w);
#else
    float t0 = uintBitsToFloat(col.x ^ p.zero); float t1 = uintBitsToFloat(col.y ^ p.zero); float t2 = uintBitsToFloat(col.z ^ p.zero); float t3 = uintBitsToFloat(col.w ^ p.zero);
#endif
    float c0 = float(floatBitsToUint(t0)); float c1 = float(floatBitsToUint(t1)); float c2 = float(floatBitsToUint(t2)); float c3 = float(floatBitsToUint(t3));
#endif
    float n0 = c0 * 0.00392156886; float n1 = c1 * 0.00392156886; float n2 = c2 * 0.00392156886; float n3 = c3 * 0.00392156886;
    o.x = n0; o.y = n1; o.z = n2; o.w = n3;
}
