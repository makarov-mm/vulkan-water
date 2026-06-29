#version 450
#extension GL_GOOGLE_include_directive : require
#include "common.glsl"
layout(location = 0) in  vec2 inXZ;     // grid position in [-1,1]
layout(location = 0) out vec3 vWorld;
void main() {
    vec4 info = texture(waterTex, inXZ * 0.5 + 0.5);
    vec3 world = vec3(inXZ.x, info.r, inXZ.y);
    vWorld = world;
    gl_Position = u.viewProj * vec4(world, 1.0);
}
