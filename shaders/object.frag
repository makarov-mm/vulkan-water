#version 450
#extension GL_GOOGLE_include_directive : require
#include "shading.glsl"
layout(location = 0) in  vec3 vWorld;
layout(location = 1) in  vec3 vNormal;
layout(location = 2) in  vec2 vUV;
layout(location = 0) out vec4 outColor;
void main() {
    vec3 albedo = texture(objTex, vUV).rgb;          // white default when the object is untextured
    outColor = vec4(getObjectColorTex(vWorld, normalize(vNormal), albedo), 1.0);
}
