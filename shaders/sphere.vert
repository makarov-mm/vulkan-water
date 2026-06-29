#version 450
#extension GL_GOOGLE_include_directive : require
#include "common.glsl"
layout(location = 0) in  vec3 inPos;   // unit sphere position
layout(location = 0) out vec3 vWorld;
void main() {
    vec3 world = sphereCenter + inPos * sphereRadius;
    vWorld = world;
    gl_Position = u.viewProj * vec4(world, 1.0);
}
