#version 450
layout(location = 0) out vec4 out_color0;
layout(location = 1) out vec4 out_color1;
void main() {
    out_color0 = vec4(1.0, 0.25, 0.05, 1.0);
    out_color1 = vec4(0.10, 0.80, 0.20, 1.0);
}
