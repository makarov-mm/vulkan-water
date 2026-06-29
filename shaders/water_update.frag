#version 450
#extension GL_GOOGLE_include_directive : require
#include "common.glsl"
layout(location = 0) in  vec2 vUV;
layout(location = 0) out vec4 outColor;
void main() {
    vec2 texel = 1.0 / vec2(textureSize(waterTex, 0));
    vec4 info = texture(waterTex, vUV);
    vec2 dx = vec2(texel.x, 0.0);
    vec2 dy = vec2(0.0, texel.y);

    float average = (
        texture(waterTex, vUV - dx).r +
        texture(waterTex, vUV - dy).r +
        texture(waterTex, vUV + dx).r +
        texture(waterTex, vUV + dy).r) * 0.25;

    info.g += (average - info.r) * 2.0;
    info.g *= 0.995;          // damping
    info.r += info.g;
    outColor = info;
}
