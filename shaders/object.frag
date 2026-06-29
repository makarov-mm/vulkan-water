#version 450
#extension GL_GOOGLE_include_directive : require
#include "shading.glsl"
layout(location = 0) in  vec3 vWorld;
layout(location = 1) in  vec3 vNormal;
layout(location = 0) out vec4 outColor;
void main() {
    outColor = vec4(getObjectColor(vWorld, normalize(vNormal)), 1.0);
}
