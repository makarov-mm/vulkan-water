#version 450
#extension GL_GOOGLE_include_directive : require
#include "shading.glsl"
layout(location = 0) in  vec3 vWorld;
layout(location = 0) out vec4 outColor;
void main() {
    if (POOL_SHAPE > 0.5 && length(vWorld.xz) > 1.0) discard;
    vec2 coord = vWorld.xz * 0.5 + 0.5;
    vec4 info = texture(waterTex, coord);
    vec3 normal = vec3(info.b, sqrt(max(0.0, 1.0 - dot(info.ba, info.ba))), info.a);

    vec3 incomingRay = normalize(vWorld - EYE);

    // viewed from above the water surface
    vec3 reflectedRay = reflect(incomingRay, normal);
    vec3 refractedRay = refract(incomingRay, normal, IOR_AIR / IOR_WATER);
    float fresnel = mix(0.25, 1.0, pow(1.0 - max(0.0, dot(normal, -incomingRay)), 3.0));

    vec3 reflectedColor = getSurfaceRayColor(vWorld, reflectedRay, abovewaterColor);
    vec3 refractedColor = getSurfaceRayColor(vWorld, refractedRay, abovewaterColor);

    outColor = vec4(mix(refractedColor, reflectedColor, fresnel), 1.0);
}
