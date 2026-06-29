#ifndef SHADING_GLSL
#define SHADING_GLSL
#include "common.glsl"

vec3 getSphereColor(vec3 point) {
    vec3 color = vec3(0.5);
    color *= 1.0 - 0.9 / pow((1.0 + sphereRadius - abs(point.x)) / sphereRadius, 3.0);
    color *= 1.0 - 0.9 / pow((1.0 + sphereRadius - abs(point.z)) / sphereRadius, 3.0);
    color *= 1.0 - 0.9 / pow((point.y + 1.0 + sphereRadius) / sphereRadius, 3.0);

    vec3 sphereNormal = (point - sphereCenter) / sphereRadius;
    vec3 refractedLight = refract(-LIGHT, vec3(0.0, 1.0, 0.0), IOR_AIR / IOR_WATER);
    float diffuse = max(0.0, dot(-refractedLight, sphereNormal)) * 0.5;

    vec4 info = texture(waterTex, point.xz * 0.5 + 0.5);
    if (point.y < info.r) {
        vec4 caustic = texture(causticTex,
            0.75 * (point.xz - point.y * refractedLight.xz / refractedLight.y) * 0.5 + 0.5);
        diffuse *= caustic.r * 4.0;
    }
    color += diffuse;
    return color;
}

// generic loaded-object shading (glTF) using an interpolated surface normal
vec3 getObjectColor(vec3 point, vec3 n) {
    vec3 color = vec3(0.5);
    color *= 1.0 - 0.6 / pow((point.y + 1.0 + sphereRadius) / sphereRadius, 3.0);

    vec3 refractedLight = refract(-LIGHT, vec3(0.0, 1.0, 0.0), IOR_AIR / IOR_WATER);
    float diffuse = max(0.0, dot(-refractedLight, n)) * 0.5;

    vec4 info = texture(waterTex, point.xz * 0.5 + 0.5);
    if (point.y < info.r) {
        vec4 caustic = texture(causticTex,
            0.75 * (point.xz - point.y * refractedLight.xz / refractedLight.y) * 0.5 + 0.5);
        diffuse *= caustic.r * 4.0;
    }
    color += diffuse;
    return color;
}

vec3 getWallColor(vec3 point) {
    float scale = 0.5;
    vec3 wallColor;
    vec3 normal;

    if (POOL_SHAPE < 0.5) {
        // box
        if (abs(point.x) > 0.999) {
            wallColor = texture(tilesTex, point.yz * 0.5 + vec2(1.0, 0.5)).rgb;
            normal = vec3(-sign(point.x), 0.0, 0.0);
        } else if (abs(point.z) > 0.999) {
            wallColor = texture(tilesTex, point.yx * 0.5 + vec2(1.0, 0.5)).rgb;
            normal = vec3(0.0, 0.0, -sign(point.z));
        } else {
            wallColor = texture(tilesTex, point.xz * 0.5 + 0.5).rgb;
            normal = vec3(0.0, 1.0, 0.0);
        }
    } else {
        // cylinder
        float rr = length(point.xz);
        if (rr > 0.999) {
            float ang = atan(point.z, point.x);
            wallColor = texture(tilesTex, vec2(ang * 0.5, point.y * 0.5)).rgb;
            normal = vec3(-point.x / max(rr, 1e-4), 0.0, -point.z / max(rr, 1e-4));
        } else {
            wallColor = texture(tilesTex, point.xz * 0.5 + 0.5).rgb;
            normal = vec3(0.0, 1.0, 0.0);
        }
    }

    scale /= length(point);
    scale *= 1.0 - 0.9 / pow(length(point - sphereCenter) / sphereRadius, 4.0);

    vec3 refractedLight = -refract(-LIGHT, vec3(0.0, 1.0, 0.0), IOR_AIR / IOR_WATER);
    float diffuse = max(0.0, dot(refractedLight, normal));
    float shadow = sphereShadow(point, refractedLight);   // soft sphere shadow

    vec4 info = texture(waterTex, point.xz * 0.5 + 0.5);
    if (point.y < info.r) {
        vec4 caustic = texture(causticTex,
            0.75 * (point.xz - point.y * refractedLight.xz / refractedLight.y) * 0.5 + 0.5);
        scale += diffuse * caustic.r * 2.0 * shadow;
    } else {
        scale += diffuse * 0.5 * shadow;
    }
    return wallColor * scale;
}

vec3 getSkyColor(vec3 ray) {
    float t = clamp(ray.y * 0.5 + 0.5, 0.0, 1.0);
    vec3 col = mix(vec3(0.72, 0.80, 0.95), vec3(0.10, 0.30, 0.62), t);
    col += vec3(10.0, 8.0, 6.0) * pow(max(0.0, dot(LIGHT, ray)), 2000.0);
    return col;
}

vec3 getSurfaceRayColor(vec3 origin, vec3 ray, vec3 waterColor) {
    vec3 color;
    float q = intersectSphere(origin, ray, sphereCenter, sphereRadius);
    if (q < 1.0e6) {
        color = getSphereColor(origin + ray * q);
    } else if (ray.y < 0.0) {
        vec2 t = intersectPool(origin, ray);
        color = getWallColor(origin + ray * t.y);
    } else {
        vec2 t = intersectPool(origin, ray);
        vec3 hit = origin + ray * t.y;
        if (hit.y < 2.0 / 12.0) color = getWallColor(hit);
        else                    color = getSkyColor(ray);
    }
    if (ray.y < 0.0) color *= waterColor;
    return color;
}

#endif
