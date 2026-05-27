// Skybox fragment shader from SaschaWillems/Vulkan "texturecubemap" example, MIT.
// (c) Sascha Willems — https://github.com/SaschaWillems/Vulkan
#version 450

layout (binding = 1) uniform samplerCube samplerCubeMap;

layout (location = 0) in vec3 inUVW;

layout (location = 0) out vec4 outFragColor;

void main()
{
	outFragColor = texture(samplerCubeMap, inUVW);
}
