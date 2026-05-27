#version 450
/* Fullscreen quad (2 triangles) from gl_VertexIndex; passes UVs to the frag. */
layout(location = 0) out vec2 vUV;
vec2 pos[6] = vec2[](
    vec2(-1.0, -1.0), vec2( 1.0, -1.0), vec2( 1.0,  1.0),
    vec2(-1.0, -1.0), vec2( 1.0,  1.0), vec2(-1.0,  1.0)
);
vec2 uvs[6] = vec2[](
    vec2(0.0, 0.0), vec2(1.0, 0.0), vec2(1.0, 1.0),
    vec2(0.0, 0.0), vec2(1.0, 1.0), vec2(0.0, 1.0)
);
void main() {
    gl_Position = vec4(pos[gl_VertexIndex], 0.0, 1.0);
    vUV = uvs[gl_VertexIndex];
}
