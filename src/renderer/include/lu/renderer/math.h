#pragma once

#include <algorithm>
#include <array>
#include <cmath>

namespace lu::renderer {

struct Vec2 {
    float x = 0.0f;
    float y = 0.0f;
};

struct Vec3 {
    float x = 0.0f;
    float y = 0.0f;
    float z = 0.0f;
};

struct Vec4 {
    float x = 0.0f;
    float y = 0.0f;
    float z = 0.0f;
    float w = 0.0f;
};

struct Mat4 {
    std::array<float, 16> m{};
};

inline Vec3 operator+(Vec3 a, Vec3 b) { return {a.x + b.x, a.y + b.y, a.z + b.z}; }
inline Vec3 operator-(Vec3 a, Vec3 b) { return {a.x - b.x, a.y - b.y, a.z - b.z}; }
inline Vec3 operator*(Vec3 v, float s) { return {v.x * s, v.y * s, v.z * s}; }
inline Vec3 operator/(Vec3 v, float s) { return {v.x / s, v.y / s, v.z / s}; }

inline float dot(Vec3 a, Vec3 b) {
    return a.x * b.x + a.y * b.y + a.z * b.z;
}

inline Vec3 cross(Vec3 a, Vec3 b) {
    return {
        a.y * b.z - a.z * b.y,
        a.z * b.x - a.x * b.z,
        a.x * b.y - a.y * b.x
    };
}

inline float length(Vec3 v) {
    return std::sqrt(dot(v, v));
}

inline Vec3 normalize(Vec3 v) {
    float len = length(v);
    if (len <= 0.000001f) return {0.0f, 1.0f, 0.0f};
    return v / len;
}

inline Mat4 identity() {
    Mat4 out{};
    out.m[0] = 1.0f;
    out.m[5] = 1.0f;
    out.m[10] = 1.0f;
    out.m[15] = 1.0f;
    return out;
}

inline Mat4 look_at(Vec3 eye, Vec3 target, Vec3 up) {
    Vec3 z = normalize(target - eye);
    Vec3 x = normalize(cross(up, z));
    Vec3 y = cross(z, x);

    Mat4 out = identity();
    out.m[0] = x.x; out.m[4] = x.y; out.m[8]  = x.z; out.m[12] = -dot(x, eye);
    out.m[1] = y.x; out.m[5] = y.y; out.m[9]  = y.z; out.m[13] = -dot(y, eye);
    out.m[2] = z.x; out.m[6] = z.y; out.m[10] = z.z; out.m[14] = -dot(z, eye);
    return out;
}

inline Mat4 perspective(float fov_y_radians, float aspect, float near_plane, float far_plane) {
    float f = 1.0f / std::tan(fov_y_radians * 0.5f);
    Mat4 out{};
    out.m[0] = f / aspect;
    out.m[5] = f;
    out.m[10] = far_plane / (far_plane - near_plane);
    out.m[11] = 1.0f;
    out.m[14] = -(near_plane * far_plane) / (far_plane - near_plane);
    return out;
}

} // namespace lu::renderer
