#ifndef COMMON_GLSL
#define COMMON_GLSL

const float IOR_AIR   = 1.0;
const float IOR_WATER = 1.333;
const float poolHeight = 1.0;
const vec3  abovewaterColor = vec3(0.25, 1.0, 1.25);
const vec3  underwaterColor = vec3(0.4, 0.9, 1.0);

layout(set = 0, binding = 0) uniform SceneUBO {
    mat4 viewProj;
    mat4 objectModel;
    vec4 eye;     // xyz = camera position
    vec4 light;   // xyz = normalized direction TOWARDS the light
    vec4 sphere;  // xyz = proxy center, w = proxy radius
    vec4 misc;    // x = time, y = poolShape (0 = box, 1 = cylinder)
    vec4 objMin;  // xyz = world AABB min of the loaded object, w = hasObject (1/0)
    vec4 objMax;  // xyz = world AABB max of the loaded object, w = voxel size (world units)
} u;

#define EYE          (u.eye.xyz)
#define LIGHT        (u.light.xyz)
#define sphereCenter (u.sphere.xyz)
#define sphereRadius (u.sphere.w)
#define POOL_SHAPE   (u.misc.y)

layout(set = 0, binding = 1) uniform sampler2D waterTex;   // r=height g=velocity ba=normal.xz
layout(set = 0, binding = 2) uniform sampler2D causticTex; // r=intensity
layout(set = 0, binding = 3) uniform sampler2D tilesTex;   // pool tile texture
layout(set = 0, binding = 4) uniform sampler3D udfTex;     // unsigned distance field of the loaded object (world units)
layout(set = 0, binding = 5) uniform sampler2D objTex;     // base-colour texture of the loaded object

vec2 intersectCube(vec3 origin, vec3 ray, vec3 cubeMin, vec3 cubeMax) {
    vec3 tMin = (cubeMin - origin) / ray;
    vec3 tMax = (cubeMax - origin) / ray;
    vec3 t1 = min(tMin, tMax);
    vec3 t2 = max(tMin, tMax);
    float tNear = max(max(t1.x, t1.y), t1.z);
    float tFar  = min(min(t2.x, t2.y), t2.z);
    return vec2(tNear, tFar);
}

// infinite cylinder radius R around the y axis, clamped to y in [ymin, ymax]
vec2 intersectCylinder(vec3 o, vec3 d, float R, float ymin, float ymax) {
    float a = dot(d.xz, d.xz);
    float b = 2.0 * dot(o.xz, d.xz);
    float c = dot(o.xz, o.xz) - R * R;
    float tNear = -1e9, tFar = 1e9;
    if (a > 1e-8) {
        float disc = b * b - 4.0 * a * c;
        if (disc < 0.0) return vec2(1.0, -1.0);        // miss
        float s = sqrt(disc);
        tNear = (-b - s) / (2.0 * a);
        tFar  = (-b + s) / (2.0 * a);
    } else if (c > 0.0) {
        return vec2(1.0, -1.0);                        // parallel & outside
    }
    float ty0 = (ymin - o.y) / d.y;
    float ty1 = (ymax - o.y) / d.y;
    tNear = max(tNear, min(ty0, ty1));
    tFar  = min(tFar,  max(ty0, ty1));
    return vec2(tNear, tFar);
}

// pool intersection dispatched by shape (box / cylinder)
vec2 intersectPool(vec3 o, vec3 d) {
    if (POOL_SHAPE < 0.5)
        return intersectCube(o, d, vec3(-1.0, -poolHeight, -1.0), vec3(1.0, 2.0, 1.0));
    return intersectCylinder(o, d, 1.0, -poolHeight, 2.0);
}

float intersectSphere(vec3 origin, vec3 ray, vec3 center, float radius) {
    vec3 toSphere = origin - center;
    float a = dot(ray, ray);
    float b = 2.0 * dot(toSphere, ray);
    float c = dot(toSphere, toSphere) - radius * radius;
    float disc = b * b - 4.0 * a * c;
    if (disc > 0.0) {
        float t = (-b - sqrt(disc)) / (2.0 * a);
        if (t > 0.0) return t;
    }
    return 1.0e6;
}

// soft shadow of the (proxy) sphere along a ray towards the light.
// returns 1 = fully lit, 0 = fully shadowed, smooth penumbra in between.
float sphereShadow(vec3 origin, vec3 dir) {
    vec3 oc = sphereCenter - origin;
    float t = dot(oc, dir);
    if (t < 0.0) return 1.0;
    vec3 closest = origin + dir * t;
    float dist = length(closest - sphereCenter);
    return smoothstep(sphereRadius, sphereRadius * 1.6, dist);
}

#endif
