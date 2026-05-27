#version 450
/* Textured quad for nvk_textures. A push constant carries the quad's rect
 * (centre.xy, halfsize.zw) in NDC and the explicit mip LOD to sample (lod.x). */
layout(location = 0) in vec2 inPos;
layout(location = 1) in vec2 inUV;
layout(location = 0) out vec2 vUV;
layout(push_constant) uniform PC { vec4 rect; vec4 lod; } pc;
void main() {
    gl_Position = vec4(inPos * pc.rect.zw + pc.rect.xy, 0.0, 1.0);
    vUV = inUV;
}
