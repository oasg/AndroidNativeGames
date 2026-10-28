#version 450
layout(location = 0) in vec2 v_uv;
layout(location = 0) out vec4 out_color;
void main() {
    out_color = vec4(v_uv, 0.25, 1.0);
}
