#version 450
#extension GL_GOOGLE_include_directive : require
#include "common.glsl"
layout(location = 0) in  vec3 inPos;   // world position of pool wall/floor
layout(location = 0) out vec3 vWorld;
void main() {
    vWorld = inPos;
    gl_Position = u.viewProj * vec4(inPos, 1.0);
}
