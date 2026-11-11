#version 450
// The vertex-colour test: the colour the vertex shader made, written as it arrives
layout(location = 2) in vec4 v;
layout(location = 0) out vec4 c;
void main() { c = v; }
