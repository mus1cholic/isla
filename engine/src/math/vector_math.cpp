#include "isla/engine/math/render_math.hpp"

#include <cmath>

namespace isla::client::render_math {

Vec3 operator+(const Vec3& lhs, const Vec3& rhs) {
    return { .x = lhs.x + rhs.x, .y = lhs.y + rhs.y, .z = lhs.z + rhs.z };
}

Vec3 operator-(const Vec3& lhs, const Vec3& rhs) {
    return { .x = lhs.x - rhs.x, .y = lhs.y - rhs.y, .z = lhs.z - rhs.z };
}

Vec3 operator*(const Vec3& v, float s) {
    return { .x = v.x * s, .y = v.y * s, .z = v.z * s };
}

float dot(const Vec3& lhs, const Vec3& rhs) {
    return (lhs.x * rhs.x) + (lhs.y * rhs.y) + (lhs.z * rhs.z);
}

Vec3 cross(const Vec3& lhs, const Vec3& rhs) {
    return {
        .x = (lhs.y * rhs.z) - (lhs.z * rhs.y),
        .y = (lhs.z * rhs.x) - (lhs.x * rhs.z),
        .z = (lhs.x * rhs.y) - (lhs.y * rhs.x),
    };
}

float length(const Vec3& v) {
    return std::sqrt(dot(v, v));
}

Vec3 normalize(const Vec3& v) {
    const float len = length(v);
    if (len <= math::kRuntimeEpsilon) {
        return { .x = 0.0F, .y = 0.0F, .z = 0.0F };
    }
    const float inv = 1.0F / len;
    return v * inv;
}

Vec3 rotate_y(const Vec3& v, float radians) {
    const float c = std::cos(radians);
    const float s = std::sin(radians);
    return { .x = (v.x * c) + (v.z * s), .y = v.y, .z = (-v.x * s) + (v.z * c) };
}

} // namespace isla::client::render_math


