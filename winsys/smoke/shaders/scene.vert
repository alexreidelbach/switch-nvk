#version 450
/* 3D scene vertex shader: transform position by MVP (from a UBO), pass UV. */
layout(location = 0) in vec3 inPos;
layout(location = 1) in vec2 inUV;
layout(binding = 1) uniform UBO { mat4 mvp; } ubo;
layout(location = 0) out vec2 vUV;
void main() {
    gl_Position = ubo.mvp * vec4(inPos, 1.0);
    vUV = inUV;
}
