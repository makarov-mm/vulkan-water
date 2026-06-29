#version 450
#extension GL_GOOGLE_include_directive : require
#include "common.glsl"
layout(location = 0) in  vec2 vUV;
layout(location = 0) out vec4 outColor;
layout(push_constant) uniform PC { vec4 p0; vec4 p1; } pc; // p0.xyz=oldCenter p1.xyz=newCenter

float volumeInSphere(vec3 center) {
    vec3 toCenter = vec3(vUV.x * 2.0 - 1.0, 0.0, vUV.y * 2.0 - 1.0) - center;
    float t  = length(toCenter) / sphereRadius;
    float dy = exp(-pow(t * 1.5, 6.0));
    float ymin = min(0.0, center.y - dy);
    float ymax = min(max(0.0, center.y + dy), ymin + 2.0 * dy);
    return (ymax - ymin) * 0.1;
}

void main() {
    vec4 info = texture(waterTex, vUV);
    info.r += volumeInSphere(pc.p0.xyz);
    info.r -= volumeInSphere(pc.p1.xyz);
    outColor = info;
}
