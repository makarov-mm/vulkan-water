#version 450
#extension GL_GOOGLE_include_directive : require
#include "common.glsl"
layout(location = 0) in  vec3 inPos;
layout(location = 1) in  vec3 inNormal;
layout(location = 2) in  vec2 inUV;
layout(location = 0) out vec3 vWorld;
layout(location = 1) out vec3 vNormal;
layout(location = 2) out vec2 vUV;
void main() {
    vec4 world = u.objectModel * vec4(inPos, 1.0);
    vWorld = world.xyz;
    vNormal = normalize(mat3(u.objectModel) * inNormal);
    vUV = inUV;
    gl_Position = u.viewProj * world;
}
