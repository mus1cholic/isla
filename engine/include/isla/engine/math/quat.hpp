#pragma once

#include "isla/engine/math/math_types.hpp"

namespace isla::client {

struct Mat4;

struct EulerYxz {
    float pitch_radians = 0.0F;
    float yaw_radians = 0.0F;
    float roll_radians = 0.0F;
};

struct Quat {
    float x = 0.0F;
    float y = 0.0F;
    float z = 0.0F;
    float w = 1.0F;

    static Quat identity();
    static Quat from_axis_angle(const Vec3& axis, float angle_radians);
    // Builds quaternion from Euler angles composed in Y-X-Z order: Ry(yaw) * Rx(pitch) * Rz(roll).
    static Quat from_euler_yxz(const EulerYxz& angles);

    [[nodiscard]] float length() const;
    [[nodiscard]] float length_squared() const;
    void normalize();
};

Quat normalize(const Quat& q);
Quat inverse(const Quat& q);
Quat conjugate(const Quat& q);
Quat multiply(const Quat& lhs, const Quat& rhs);
Quat slerp(const Quat& a, const Quat& b, float t);

Mat4 quat_to_mat4(const Quat& q);

} // namespace isla::client


