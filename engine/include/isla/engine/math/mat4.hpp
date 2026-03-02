#pragma once

#include <array>

#include "isla/engine/math/math_types.hpp"

namespace isla::client {

struct Quat;

struct LookAtParams {
    Vec3 eye{};
    Vec3 target{};
    Vec3 up{};
};

struct Mat4 {
    std::array<float, 16> elements{};

    static Mat4 identity();
    static Mat4 scale(const Vec3& scale);
    static Mat4 translation(const Vec3& translation);
    static Mat4 rotation_x(float radians);
    static Mat4 rotation_y(float radians);
    static Mat4 rotation_z(float radians);
    static Mat4 from_position_yaw(const Vec3& position, float yaw_radians);
    static Mat4 from_position_scale_euler(const Vec3& position, const Vec3& scale,
                                          float pitch_radians, float yaw_radians,
                                          float roll_radians);
    static Mat4 from_position_scale_quat(const Vec3& position, const Vec3& scale,
                                         const Quat& rotation);

    // Computes a Left-Handed (LH), +Y-Up view matrix.
    static Mat4 look_at(const LookAtParams& params);
    static Mat4 perspective(float fov_y_degrees, float aspect_ratio, float near_plane,
                            float far_plane, bool homogeneous_depth);

    float* data();
    const float* data() const;
};

Mat4 multiply(const Mat4& lhs, const Mat4& rhs);
Mat4 inverse(const Mat4& matrix);
Vec3 transform_point(const Mat4& matrix, const Vec3& point);

} // namespace isla::client


