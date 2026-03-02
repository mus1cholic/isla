#include "isla/engine/math/quat.hpp"

#include <cmath>

#include "isla/engine/math/mat4.hpp"
#include "isla/engine/math/render_math.hpp"

namespace isla::client {

Quat Quat::identity() {
    return Quat{ .x = 0.0F, .y = 0.0F, .z = 0.0F, .w = 1.0F };
}

Quat Quat::from_axis_angle(const Vec3& axis, float angle_radians) {
    const float half_angle = angle_radians * 0.5F;
    const float s = std::sin(half_angle);
    const Vec3 norm_axis = render_math::normalize(axis);

    return Quat{
        .x = norm_axis.x * s,
        .y = norm_axis.y * s,
        .z = norm_axis.z * s,
        .w = std::cos(half_angle),
    };
}

Quat Quat::from_euler_yxz(const EulerYxz& angles) {
    const float half_pitch = angles.pitch_radians * 0.5F;
    const float half_yaw = angles.yaw_radians * 0.5F;
    const float half_roll = angles.roll_radians * 0.5F;

    const float sin_p = std::sin(half_pitch);
    const float cos_p = std::cos(half_pitch);
    const float sin_y = std::sin(half_yaw);
    const float cos_y = std::cos(half_yaw);
    const float sin_r = std::sin(half_roll);
    const float cos_r = std::cos(half_roll);

    const Quat q_yaw{
        .x = 0.0F,
        .y = sin_y,
        .z = 0.0F,
        .w = cos_y,
    };
    const Quat q_pitch{
        .x = sin_p,
        .y = 0.0F,
        .z = 0.0F,
        .w = cos_p,
    };
    const Quat q_roll{
        .x = 0.0F,
        .y = 0.0F,
        .z = sin_r,
        .w = cos_r,
    };

    return multiply(multiply(q_yaw, q_pitch), q_roll);
}

float Quat::length() const {
    return std::sqrt(length_squared());
}

float Quat::length_squared() const {
    return (x * x) + (y * y) + (z * z) + (w * w);
}

void Quat::normalize() {
    const float len_sq = length_squared();
    if (len_sq > math::kRuntimeEpsilon) {
        const float inv_len = 1.0F / std::sqrt(len_sq);
        x *= inv_len;
        y *= inv_len;
        z *= inv_len;
        w *= inv_len;
    } else {
        x = 0.0F;
        y = 0.0F;
        z = 0.0F;
        w = 1.0F;
    }
}

Quat normalize(const Quat& q) {
    Quat res = q;
    res.normalize();
    return res;
}

Quat conjugate(const Quat& q) {
    return Quat{ .x = -q.x, .y = -q.y, .z = -q.z, .w = q.w };
}

Quat inverse(const Quat& q) {
    const float len_sq = q.length_squared();
    if (len_sq < math::kRuntimeEpsilon) {
        return Quat::identity();
    }
    const float inv_len_sq = 1.0F / len_sq;
    return Quat{
        .x = -q.x * inv_len_sq,
        .y = -q.y * inv_len_sq,
        .z = -q.z * inv_len_sq,
        .w = q.w * inv_len_sq,
    };
}

Quat multiply(const Quat& lhs, const Quat& rhs) {
    return Quat{
        .x = (lhs.w * rhs.x) + (lhs.x * rhs.w) + (lhs.y * rhs.z) - (lhs.z * rhs.y),
        .y = (lhs.w * rhs.y) - (lhs.x * rhs.z) + (lhs.y * rhs.w) + (lhs.z * rhs.x),
        .z = (lhs.w * rhs.z) + (lhs.x * rhs.y) - (lhs.y * rhs.x) + (lhs.z * rhs.w),
        .w = (lhs.w * rhs.w) - (lhs.x * rhs.x) - (lhs.y * rhs.y) - (lhs.z * rhs.z),
    };
}

Quat slerp(const Quat& a, const Quat& b, float t) {
    Quat q1 = a;
    Quat q2 = b;

    float cos_theta = (q1.x * q2.x) + (q1.y * q2.y) + (q1.z * q2.z) + (q1.w * q2.w);

    if (cos_theta < 0.0F) {
        q2 = Quat{ .x = -q2.x, .y = -q2.y, .z = -q2.z, .w = -q2.w };
        cos_theta = -cos_theta;
    }

    if (cos_theta > 0.9995F) {
        // Linear interpolation for close quaternions
        Quat res{
            .x = q1.x + (t * (q2.x - q1.x)),
            .y = q1.y + (t * (q2.y - q1.y)),
            .z = q1.z + (t * (q2.z - q1.z)),
            .w = q1.w + (t * (q2.w - q1.w)),
        };
        return normalize(res);
    }

    const float theta = std::acos(cos_theta);
    const float sin_theta = std::sin(theta);
    const float weight1 = std::sin((1.0F - t) * theta) / sin_theta;
    const float weight2 = std::sin(t * theta) / sin_theta;

    return Quat{
        .x = (q1.x * weight1) + (q2.x * weight2),
        .y = (q1.y * weight1) + (q2.y * weight2),
        .z = (q1.z * weight1) + (q2.z * weight2),
        .w = (q1.w * weight1) + (q2.w * weight2),
    };
}

Mat4 quat_to_mat4(const Quat& q) {
    Mat4 res = Mat4::identity();

    const float len_sq = q.length_squared();
    if (len_sq < math::kRuntimeEpsilon) {
        return res;
    }

    const float s = 2.0F / len_sq;

    const float xx = q.x * q.x * s;
    const float yy = q.y * q.y * s;
    const float zz = q.z * q.z * s;
    const float xy = q.x * q.y * s;
    const float xz = q.x * q.z * s;
    const float yz = q.y * q.z * s;
    const float wx = q.w * q.x * s;
    const float wy = q.w * q.y * s;
    const float wz = q.w * q.z * s;

    res.elements[0] = 1.0F - (yy + zz);
    res.elements[1] = (xy + wz);
    res.elements[2] = (xz - wy);
    res.elements[3] = 0.0F;

    res.elements[4] = (xy - wz);
    res.elements[5] = 1.0F - (xx + zz);
    res.elements[6] = (yz + wx);
    res.elements[7] = 0.0F;

    res.elements[8] = (xz + wy);
    res.elements[9] = (yz - wx);
    res.elements[10] = 1.0F - (xx + yy);
    res.elements[11] = 0.0F;

    res.elements[12] = 0.0F;
    res.elements[13] = 0.0F;
    res.elements[14] = 0.0F;
    res.elements[15] = 1.0F;

    return res;
}

} // namespace isla::client


