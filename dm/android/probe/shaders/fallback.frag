#version 450
// The fallback-draw test: each draw's colour from a push constant
layout(push_constant) uniform P { vec4 col; } p;
layout(location = 0) out vec4 c;
void main() { c = p.col; }
