#version 450
/* Output the interpolated per-object colour (with alpha) for blending. */
layout(location = 0) in vec4 vColor;
layout(location = 0) out vec4 outColor;
void main() { outColor = vColor; }
