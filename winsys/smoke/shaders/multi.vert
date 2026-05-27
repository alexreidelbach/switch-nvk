#version 450
/* Flat-coloured quad for nvk_multi. A per-object UBO carries the colour (rgba,
 * the .a drives alpha blending) plus the quad's centre and half-size in NDC;
 * inPos is a unit quad in [-1,1] from the bound vertex buffer. */
layout(location = 0) in vec2 inPos;
layout(binding = 0) uniform UBO { vec4 color; vec2 center; vec2 halfsize; } u;
layout(location = 0) out vec4 vColor;
void main() {
    gl_Position = vec4(inPos * u.halfsize + u.center, 0.0, 1.0);
    vColor = u.color;
}
