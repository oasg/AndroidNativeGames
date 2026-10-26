#version 450
layout(location = 0) out vec2 v_uv;
void main() {
    vec2 p[3] = vec2[](vec2(-0.65, -0.65), vec2(0.65, -0.65), vec2(0.0, 0.65));
    v_uv = p[gl_VertexIndex] * 0.5 + 0.5;
    gl_Position = vec4(p[gl_VertexIndex], 0.0, 1.0);
}
