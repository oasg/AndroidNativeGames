#version 450
/* Repository-owned BC4 sampled-draw probe fragment shader.
 *
 * Writes the sampled value straight to the RGBA8 target so the probe can
 * verify Vulkan's BC4 view semantics: sample == (R, 0, 0, 1).
 */
layout(set = 0, binding = 0) uniform sampler2D tex;
layout(location = 0) in vec2 v_uv;
layout(location = 0) out vec4 out_color;
void main() {
    out_color = texture(tex, v_uv);
}
