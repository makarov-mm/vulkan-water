#version 450
#extension GL_GOOGLE_include_directive : require
#include "common.glsl"
layout(location = 0) in  vec3 oldPos;
layout(location = 1) in  vec3 newPos;
layout(location = 0) out vec4 outColor;
void main() {
    float oldArea = length(dFdx(oldPos)) * length(dFdy(oldPos));
    float newArea = length(dFdx(newPos)) * length(dFdy(newPos));
    float ratio = oldArea / max(newArea, 1e-6);
    vec3 refractedLight = refract(-LIGHT, vec3(0.0, 1.0, 0.0), IOR_AIR / IOR_WATER);
    float shadow = sphereShadow(newPos, -refractedLight);
    outColor = vec4(ratio * 0.2 * shadow, 0.0, 0.0, 1.0);
}
