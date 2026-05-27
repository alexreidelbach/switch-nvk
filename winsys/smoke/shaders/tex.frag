#version 450
/* Sample the bound texture at an EXPLICIT mip level (pc.lod.x) so the mipmap
 * test is deterministic; sRGB decode + BC1 decompress happen in the texture unit. */
layout(location = 0) in vec2 vUV;
layout(location = 0) out vec4 outColor;
layout(binding = 0) uniform sampler2D tex;
layout(push_constant) uniform PC { vec4 rect; vec4 lod; } pc;
void main() { outColor = textureLod(tex, vUV, pc.lod.x); }
