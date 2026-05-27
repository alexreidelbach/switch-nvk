#version 450
/* Hardcoded triangle — no vertex buffer; positions indexed by gl_VertexIndex.
 * Vulkan clip space: x right, y DOWN. This yields a centred, upward triangle. */
vec2 positions[3] = vec2[](
    vec2( 0.0, -0.6),
    vec2( 0.6,  0.6),
    vec2(-0.6,  0.6)
);
void main() {
    gl_Position = vec4(positions[gl_VertexIndex], 0.0, 1.0);
}
