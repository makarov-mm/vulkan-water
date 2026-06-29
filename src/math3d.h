// math3d.h — tiny self-contained linear algebra, drop-in for the small subset of
// GLM this project uses. Column-major mat4 with Vulkan (zero-to-one) depth, so the
// memory layout matches std140 mat4 in GLSL exactly (16 contiguous floats, column
// major). No external dependencies.
#pragma once
#include <cmath>

namespace glm {

struct vec2 {
    float x=0, y=0;
    vec2() {}
    vec2(float s): x(s), y(s) {}
    vec2(float x_, float y_): x(x_), y(y_) {}
};

struct vec4;

struct vec3 {
    float x=0, y=0, z=0;
    vec3() {}
    explicit vec3(float s): x(s), y(s), z(s) {}
    vec3(float x_, float y_, float z_): x(x_), y(y_), z(z_) {}
    explicit vec3(const vec4& v);                       // truncate xyz (defined below)
    vec3& operator+=(const vec3& o){ x+=o.x; y+=o.y; z+=o.z; return *this; }
    vec3& operator-=(const vec3& o){ x-=o.x; y-=o.y; z-=o.z; return *this; }
    vec3& operator*=(float s){ x*=s; y*=s; z*=s; return *this; }
};

struct vec4 {
    float x=0, y=0, z=0, w=0;
    vec4() {}
    explicit vec4(float s): x(s), y(s), z(s), w(s) {}
    vec4(float x_, float y_, float z_, float w_): x(x_), y(y_), z(z_), w(w_) {}
    vec4(const vec3& v, float w_): x(v.x), y(v.y), z(v.z), w(w_) {}
    vec4& operator/=(float s){ x/=s; y/=s; z/=s; w/=s; return *this; }
    float&       operator[](int i){ return (&x)[i]; }
    const float& operator[](int i) const { return (&x)[i]; }
};

inline vec3::vec3(const vec4& v): x(v.x), y(v.y), z(v.z) {}

// ---- vec3 operators ----
inline vec3 operator+(vec3 a, const vec3& b){ return vec3(a.x+b.x, a.y+b.y, a.z+b.z); }
inline vec3 operator-(vec3 a, const vec3& b){ return vec3(a.x-b.x, a.y-b.y, a.z-b.z); }
inline vec3 operator-(const vec3& a){ return vec3(-a.x, -a.y, -a.z); }
inline vec3 operator*(vec3 a, float s){ return vec3(a.x*s, a.y*s, a.z*s); }
inline vec3 operator*(float s, vec3 a){ return vec3(a.x*s, a.y*s, a.z*s); }
inline vec3 operator/(vec3 a, float s){ return vec3(a.x/s, a.y/s, a.z/s); }

inline float dot(const vec3& a, const vec3& b){ return a.x*b.x + a.y*b.y + a.z*b.z; }
inline vec3 cross(const vec3& a, const vec3& b){
    return vec3(a.y*b.z - a.z*b.y, a.z*b.x - a.x*b.z, a.x*b.y - a.y*b.x);
}
inline float length(const vec3& a){ return std::sqrt(dot(a,a)); }
inline vec3 normalize(const vec3& a){ float l=length(a); return l>0.0f ? a/l : a; }
inline vec3 min(const vec3& a, const vec3& b){ return vec3(a.x<b.x?a.x:b.x, a.y<b.y?a.y:b.y, a.z<b.z?a.z:b.z); }
inline vec3 max(const vec3& a, const vec3& b){ return vec3(a.x>b.x?a.x:b.x, a.y>b.y?a.y:b.y, a.z>b.z?a.z:b.z); }

template<class T> inline T clamp(T v, T lo, T hi){ return v<lo ? lo : (v>hi ? hi : v); }

template<class T=float> inline T pi(){ return T(3.14159265358979323846); }
template<class T=float> inline T two_pi(){ return T(6.28318530717958647692); }
inline float radians(float deg){ return deg * (3.14159265358979323846f / 180.0f); }

// ---- mat4: column-major, m[col] is a vec4 column ----
struct mat4 {
    vec4 c[4];
    mat4() {}                                           // all zero
    explicit mat4(float diag){ c[0].x=diag; c[1].y=diag; c[2].z=diag; c[3].w=diag; }
    vec4&       operator[](int i){ return c[i]; }
    const vec4& operator[](int i) const { return c[i]; }
};

inline vec4 operator*(const mat4& m, const vec4& v){
    return vec4(
        m.c[0].x*v.x + m.c[1].x*v.y + m.c[2].x*v.z + m.c[3].x*v.w,
        m.c[0].y*v.x + m.c[1].y*v.y + m.c[2].y*v.z + m.c[3].y*v.w,
        m.c[0].z*v.x + m.c[1].z*v.y + m.c[2].z*v.z + m.c[3].z*v.w,
        m.c[0].w*v.x + m.c[1].w*v.y + m.c[2].w*v.z + m.c[3].w*v.w);
}
inline mat4 operator*(const mat4& a, const mat4& b){
    mat4 r;
    for(int i=0;i<4;++i) r.c[i] = a * b.c[i];
    return r;
}

inline mat4 translate(const mat4& m, const vec3& v){
    mat4 r = m;
    r.c[3].x = m.c[0].x*v.x + m.c[1].x*v.y + m.c[2].x*v.z + m.c[3].x;
    r.c[3].y = m.c[0].y*v.x + m.c[1].y*v.y + m.c[2].y*v.z + m.c[3].y;
    r.c[3].z = m.c[0].z*v.x + m.c[1].z*v.y + m.c[2].z*v.z + m.c[3].z;
    r.c[3].w = m.c[0].w*v.x + m.c[1].w*v.y + m.c[2].w*v.z + m.c[3].w;
    return r;
}
inline mat4 scale(const mat4& m, const vec3& v){
    mat4 r;
    r.c[0] = vec4(m.c[0].x*v.x, m.c[0].y*v.x, m.c[0].z*v.x, m.c[0].w*v.x);
    r.c[1] = vec4(m.c[1].x*v.y, m.c[1].y*v.y, m.c[1].z*v.y, m.c[1].w*v.y);
    r.c[2] = vec4(m.c[2].x*v.z, m.c[2].y*v.z, m.c[2].z*v.z, m.c[2].w*v.z);
    r.c[3] = m.c[3];
    return r;
}

// right-handed, depth range [0,1] (Vulkan); matches GLM_FORCE_DEPTH_ZERO_TO_ONE
inline mat4 perspective(float fovy, float aspect, float zNear, float zFar){
    float f = 1.0f / std::tan(fovy * 0.5f);
    mat4 m;
    m.c[0].x = f / aspect;
    m.c[1].y = f;
    m.c[2].z = zFar / (zNear - zFar);
    m.c[2].w = -1.0f;
    m.c[3].z = (zFar * zNear) / (zNear - zFar);
    return m;
}

// right-handed look-at
inline mat4 lookAt(const vec3& eye, const vec3& center, const vec3& up){
    vec3 f = normalize(center - eye);
    vec3 s = normalize(cross(f, up));
    vec3 u = cross(s, f);
    mat4 m(1.0f);
    m.c[0].x = s.x; m.c[1].x = s.y; m.c[2].x = s.z;
    m.c[0].y = u.x; m.c[1].y = u.y; m.c[2].y = u.z;
    m.c[0].z = -f.x; m.c[1].z = -f.y; m.c[2].z = -f.z;
    m.c[3].x = -dot(s, eye);
    m.c[3].y = -dot(u, eye);
    m.c[3].z =  dot(f, eye);
    return m;
}

// full 4x4 inverse (used for screen-ray picking)
inline mat4 inverse(const mat4& m){
    const float* a = &m.c[0].x;   // column-major flat: a[col*4 + row]
    float inv[16], det;
    inv[0]  =  a[5]*a[10]*a[15] - a[5]*a[11]*a[14] - a[9]*a[6]*a[15] + a[9]*a[7]*a[14] + a[13]*a[6]*a[11] - a[13]*a[7]*a[10];
    inv[4]  = -a[4]*a[10]*a[15] + a[4]*a[11]*a[14] + a[8]*a[6]*a[15] - a[8]*a[7]*a[14] - a[12]*a[6]*a[11] + a[12]*a[7]*a[10];
    inv[8]  =  a[4]*a[9]*a[15]  - a[4]*a[11]*a[13] - a[8]*a[5]*a[15] + a[8]*a[7]*a[13] + a[12]*a[5]*a[11] - a[12]*a[7]*a[9];
    inv[12] = -a[4]*a[9]*a[14]  + a[4]*a[10]*a[13] + a[8]*a[5]*a[14] - a[8]*a[6]*a[13] - a[12]*a[5]*a[10] + a[12]*a[6]*a[9];
    inv[1]  = -a[1]*a[10]*a[15] + a[1]*a[11]*a[14] + a[9]*a[2]*a[15] - a[9]*a[3]*a[14] - a[13]*a[2]*a[11] + a[13]*a[3]*a[10];
    inv[5]  =  a[0]*a[10]*a[15] - a[0]*a[11]*a[14] - a[8]*a[2]*a[15] + a[8]*a[3]*a[14] + a[12]*a[2]*a[11] - a[12]*a[3]*a[10];
    inv[9]  = -a[0]*a[9]*a[15]  + a[0]*a[11]*a[13] + a[8]*a[1]*a[15] - a[8]*a[3]*a[13] - a[12]*a[1]*a[11] + a[12]*a[3]*a[9];
    inv[13] =  a[0]*a[9]*a[14]  - a[0]*a[10]*a[13] - a[8]*a[1]*a[14] + a[8]*a[2]*a[13] + a[12]*a[1]*a[10] - a[12]*a[2]*a[9];
    inv[2]  =  a[1]*a[6]*a[15]  - a[1]*a[7]*a[14]  - a[5]*a[2]*a[15] + a[5]*a[3]*a[14] + a[13]*a[2]*a[7]  - a[13]*a[3]*a[6];
    inv[6]  = -a[0]*a[6]*a[15]  + a[0]*a[7]*a[14]  + a[4]*a[2]*a[15] - a[4]*a[3]*a[14] - a[12]*a[2]*a[7]  + a[12]*a[3]*a[6];
    inv[10] =  a[0]*a[5]*a[15]  - a[0]*a[7]*a[13]  - a[4]*a[1]*a[15] + a[4]*a[3]*a[13] + a[12]*a[1]*a[7]  - a[12]*a[3]*a[5];
    inv[14] = -a[0]*a[5]*a[14]  + a[0]*a[6]*a[13]  + a[4]*a[1]*a[14] - a[4]*a[2]*a[13] - a[12]*a[1]*a[6]  + a[12]*a[2]*a[5];
    inv[3]  = -a[1]*a[6]*a[11]  + a[1]*a[7]*a[10]  + a[5]*a[2]*a[11] - a[5]*a[3]*a[10] - a[9]*a[2]*a[7]   + a[9]*a[3]*a[6];
    inv[7]  =  a[0]*a[6]*a[11]  - a[0]*a[7]*a[10]  - a[4]*a[2]*a[11] + a[4]*a[3]*a[10] + a[8]*a[2]*a[7]   - a[8]*a[3]*a[6];
    inv[11] = -a[0]*a[5]*a[11]  + a[0]*a[7]*a[9]   + a[4]*a[1]*a[11] - a[4]*a[3]*a[9]  - a[8]*a[1]*a[7]   + a[8]*a[3]*a[5];
    inv[15] =  a[0]*a[5]*a[10]  - a[0]*a[6]*a[9]   - a[4]*a[1]*a[10] + a[4]*a[2]*a[9]  + a[8]*a[1]*a[6]   - a[8]*a[2]*a[5];
    det = a[0]*inv[0] + a[1]*inv[4] + a[2]*inv[8] + a[3]*inv[12];
    mat4 r(1.0f);
    if(det == 0.0f) return r;
    det = 1.0f / det;
    float* o = &r.c[0].x;
    for(int i=0;i<16;++i) o[i] = inv[i]*det;
    return r;
}

} // namespace glm
