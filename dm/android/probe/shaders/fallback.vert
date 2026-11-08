#version 450
// The fallback-draw test: positions and depth from one vertex stream with a 16-byte stride (built into the pipeline, as the engine does without extended dynamic state)
layout(location = 0) in vec2 pos;
layout(location = 1) in float z;
void main() { gl_Position = vec4(pos, z, 1.0); }
