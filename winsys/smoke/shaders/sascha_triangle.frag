// Fragment shader from SaschaWillems/Vulkan "triangle" example, MIT License.
// (c) Sascha Willems — https://github.com/SaschaWillems/Vulkan
#version 450

layout (location = 0) in vec3 inColor;

layout (location = 0) out vec4 outFragColor;

void main()
{
  outFragColor = vec4(inColor, 1.0);
}
