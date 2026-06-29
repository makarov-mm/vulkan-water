#version 450
#extension GL_GOOGLE_include_directive : require
#include "common.glsl"
layout(location = 0) in  vec2 vUV;
layout(location = 0) out vec4 outColor;
layout(push_constant) uniform PC { vec4 p0; vec4 p1; } pc; // p0.xy=center[-1,1] p0.z=radius p0.w=strength
const float PI = 3.141592653589793;
void main() {
    vec4 info = texture(waterTex, vUV);
    vec2 center = pc.p0.xy * 0.5 + 0.5;
    float drop = max(0.0, 1.0 - length(center - vUV) / pc.p0.z);
    drop = 0.5 - cos(drop * PI) * 0.5;
    info.r += drop * pc.p0.w;
    outColor = info;
}
