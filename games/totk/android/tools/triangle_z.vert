#version 450
layout(location = 0) in vec2 pos;
layout(location = 1) in float z;
void main() {
    gl_Position = vec4(pos, z, 1.0);
}
