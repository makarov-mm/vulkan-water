#version 450
#extension GL_GOOGLE_include_directive : require
#include "common.glsl"
layout(location = 0) in  vec2 inXZ;          // grid position in [-1,1]
layout(location = 0) out vec3 oldPos;
layout(location = 1) out vec3 newPos;

vec3 project(vec3 origin, vec3 ray, vec3 refractedLight) {
    vec2 tcube = intersectPool(origin, ray);
    origin += ray * tcube.y;
    float tplane = (-origin.y - 1.0) / refractedLight.y;
    return origin + refractedLight * tplane;
}

void main() {
    vec4 info = texture(waterTex, inXZ * 0.5 + 0.5);
    info.ba *= 0.5;
    vec3 normal = vec3(info.b, sqrt(max(0.0, 1.0 - dot(info.ba, info.ba))), info.a);

    vec3 refractedLight = refract(-LIGHT, vec3(0.0, 1.0, 0.0), IOR_AIR / IOR_WATER);
    vec3 ray = refract(-LIGHT, normal, IOR_AIR / IOR_WATER);

    oldPos = project(vec3(inXZ.x, 0.0,    inXZ.y), refractedLight, refractedLight);
    newPos = project(vec3(inXZ.x, info.r, inXZ.y), ray,            refractedLight);

    gl_Position = vec4(0.75 * (newPos.xz + refractedLight.xz / refractedLight.y), 0.0, 1.0);
}
