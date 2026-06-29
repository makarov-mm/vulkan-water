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
    // smooth shading for the refracted underwater object: ambient + diffuse, no
    // high-frequency caustic sampling (which aliases badly on a curved surface).
    vec3 base = vec3(0.55);
    base *= 1.0 - 0.5 / pow((point.y + 1.0 + sphereRadius) / sphereRadius, 3.0);
    vec3 refractedLight = refract(-LIGHT, vec3(0.0, 1.0, 0.0), IOR_AIR / IOR_WATER);
    float diffuse = max(0.0, dot(-refractedLight, n));
    return base * (0.55 + 0.6 * diffuse);
}

// textured object shading: base colour from the glTF texture, lit + caustics
vec3 getObjectColorTex(vec3 point, vec3 n, vec3 albedo) {
    vec3 refractedLight = refract(-LIGHT, vec3(0.0, 1.0, 0.0), IOR_AIR / IOR_WATER);
    float diffuse = max(0.0, dot(-refractedLight, n)) * 0.5;
    vec4 info = texture(waterTex, point.xz * 0.5 + 0.5);
    if (point.y < info.r) {
        vec4 caustic = texture(causticTex,
            0.75 * (point.xz - point.y * refractedLight.xz / refractedLight.y) * 0.5 + 0.5);
        diffuse *= caustic.r * 4.0;
    }
    return albedo * (0.6 + diffuse);                 // ambient term + lit/caustic diffuse
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

// ---- loaded-object refraction via an unsigned distance field (3D texture) ----
// The object is baked into udfTex; world-space AABB is u.objMin/u.objMax.xyz,
// voxel size in u.objMax.w. Distances are stored in WORLD units.
float udfAt(vec3 p) {
    vec3 c = (p - u.objMin.xyz) / max(u.objMax.xyz - u.objMin.xyz, vec3(1e-6));
    if (any(lessThan(c, vec3(0.0))) || any(greaterThan(c, vec3(1.0)))) return 1e3;
    return texture(udfTex, c).r;
}
// sphere-trace the UDF from outside; returns ray t of the surface hit, or -1.
float traceObject(vec3 o, vec3 r, out vec3 hitNormal) {
    hitNormal = vec3(0.0, 1.0, 0.0);
    vec2 tb = intersectCube(o, r, u.objMin.xyz, u.objMax.xyz);
    if (tb.y < 0.0 || tb.x > tb.y) return -1.0;
    float vox = max(u.objMax.w, 1e-4);
    float t = max(tb.x, 0.0);
    bool hit = false;
    for (int i = 0; i < 200; ++i) {
        vec3 p = o + r * t;
        float d = udfAt(p);
        if (d < vox * 0.2) { hit = true; break; }     // precise convergence -> stable hit
        t += d;                                        // true distance step (no overshoot, low jitter)
        if (t > tb.y) return -1.0;
    }
    if (!hit && t > tb.y) return -1.0;                 // grazing: accept the last in-box position
    vec3 p = o + r * t;
    // smooth tetrahedron-gradient normal (4 taps) over one voxel
    float e = vox;
    vec2 k = vec2(1.0, -1.0);
    vec3 grad = k.xyy * udfAt(p + k.xyy * e)
              + k.yyx * udfAt(p + k.yyx * e)
              + k.yxy * udfAt(p + k.yxy * e)
              + k.xxx * udfAt(p + k.xxx * e);
    float gl = length(grad);
    hitNormal = (gl > 1e-5) ? grad / gl : -normalize(r);
    return t;
}

vec3 getSurfaceRayColor(vec3 origin, vec3 ray, vec3 waterColor) {
    vec3 color;
    bool hit = false;
    if (u.objMin.w > 0.5) {                       // a glTF object is loaded -> trace its real geometry
        vec3 n;
        float to = traceObject(origin, ray, n);
        if (to > 0.0) { color = getObjectColor(origin + ray * to, n); hit = true; }
    } else {                                       // built-in ball -> analytic sphere
        float q = intersectSphere(origin, ray, sphereCenter, sphereRadius);
        if (q < 1.0e6) { color = getSphereColor(origin + ray * q); hit = true; }
    }
    if (!hit) {
        if (ray.y < 0.0) {
            vec2 t = intersectPool(origin, ray);
            color = getWallColor(origin + ray * t.y);
        } else {
            vec2 t = intersectPool(origin, ray);
            vec3 h = origin + ray * t.y;
            if (h.y < 2.0 / 12.0) color = getWallColor(h);
            else                  color = getSkyColor(ray);
        }
    }
    if (ray.y < 0.0) color *= waterColor;
    return color;
}

#endif
