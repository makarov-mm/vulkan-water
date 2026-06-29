#version 450
#extension GL_GOOGLE_include_directive : require
#include "common.glsl"
layout(location = 0) in  vec2 vUV;
layout(location = 0) out vec4 outColor;
void main() {
    vec2 texel = 1.0 / vec2(textureSize(waterTex, 0));
    vec4 info = texture(waterTex, vUV);
    vec3 dx = vec3(texel.x, texture(waterTex, vec2(vUV.x + texel.x, vUV.y)).r - info.r, 0.0);
    vec3 dy = vec3(0.0,     texture(waterTex, vec2(vUV.x, vUV.y + texel.y)).r - info.r, texel.y);
    info.ba = normalize(cross(dy, dx)).xz;
    outColor = info;
}
