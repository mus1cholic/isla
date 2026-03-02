#include "isla/engine/math/mat4.hpp"

#include <algorithm>
#include <cmath>
#include <numbers>

#include "isla/engine/math/quat.hpp"
#include "isla/engine/math/render_math.hpp"

namespace isla::client {

namespace {

constexpr int kMatDimension = 4;

constexpr int index(int row, int col) {
    return row + (col * kMatDimension);
}

std::size_t to_offset(int row, int col) {
    return static_cast<std::size_t>(index(row, col));
}

// We intentionally suppress bounds checking warnings here.
// This is a hot path for rendering math, and the bounds are implicitly respected
// by internal matrix logic. Using operator[] avoids the branching overhead of std::array::at().
// NOLINTNEXTLINE(cppcoreguidelines-pro-bounds-constant-array-index)
float& element(Mat4& matrix, int row, int col) {
    return matrix.elements[to_offset(row, col)];
}

// NOLINTNEXTLINE(cppcoreguidelines-pro-bounds-constant-array-index)
const float& element(const Mat4& matrix, int row, int col) {
    return matrix.elements[to_offset(row, col)];
}

} // namespace

Mat4 Mat4::identity() {
    return Mat4{
        .elements =
            {
                1.0F, 0.0F, 0.0F, 0.0F,
                0.0F, 1.0F, 0.0F, 0.0F,
                0.0F, 0.0F, 1.0F, 0.0F,
                0.0F, 0.0F, 0.0F, 1.0F,
            },
    };
}

Mat4 Mat4::scale(const Vec3& scale) {
    return Mat4{
        .elements =
            {
                scale.x, 0.0F, 0.0F, 0.0F,
                0.0F, scale.y, 0.0F, 0.0F,
                0.0F, 0.0F, scale.z, 0.0F,
                0.0F, 0.0F, 0.0F, 1.0F,
            },
    };
}

Mat4 Mat4::translation(const Vec3& translation) {
    return Mat4{
        .elements =
            {
                1.0F, 0.0F, 0.0F, 0.0F,
                0.0F, 1.0F, 0.0F, 0.0F,
                0.0F, 0.0F, 1.0F, 0.0F,
                translation.x, translation.y, translation.z, 1.0F,
            },
    };
}

Mat4 Mat4::rotation_x(float radians) {
    const float c = std::cos(radians);
    const float s = std::sin(radians);
    return Mat4{
        .elements =
            {
                1.0F, 0.0F, 0.0F, 0.0F,
                0.0F, c, s, 0.0F,
                0.0F, -s, c, 0.0F,
                0.0F, 0.0F, 0.0F, 1.0F,
            },
    };
}

Mat4 Mat4::rotation_y(float radians) {
    const float c = std::cos(radians);
    const float s = std::sin(radians);
    return Mat4{
        .elements =
            {
                c, 0.0F, -s, 0.0F,
                0.0F, 1.0F, 0.0F, 0.0F,
                s, 0.0F, c, 0.0F,
                0.0F, 0.0F, 0.0F, 1.0F,
            },
    };
}

Mat4 Mat4::rotation_z(float radians) {
    const float c = std::cos(radians);
    const float s = std::sin(radians);
    return Mat4{
        .elements =
            {
                c, s, 0.0F, 0.0F,
                -s, c, 0.0F, 0.0F,
                0.0F, 0.0F, 1.0F, 0.0F,
                0.0F, 0.0F, 0.0F, 1.0F,
            },
    };
}

Mat4 Mat4::from_position_yaw(const Vec3& position, float yaw_radians) {
    const float c = std::cos(yaw_radians);
    const float s = std::sin(yaw_radians);
    return Mat4{
        .elements =
            {
                c, 0.0F, -s, 0.0F,
                0.0F, 1.0F, 0.0F, 0.0F,
                s, 0.0F, c, 0.0F,
                position.x, position.y, position.z, 1.0F,
            },
    };
}

Mat4 Mat4::from_position_scale_euler(const Vec3& position, const Vec3& scale, float pitch_radians,
                                     float yaw_radians, float roll_radians) {
    const Mat4 rotation =
        multiply(multiply(Mat4::rotation_y(yaw_radians), Mat4::rotation_x(pitch_radians)),
                 Mat4::rotation_z(roll_radians));
    return multiply(multiply(Mat4::translation(position), rotation), Mat4::scale(scale));
}

Mat4 Mat4::from_position_scale_quat(const Vec3& position, const Vec3& scale, const Quat& rotation) {
    return multiply(multiply(Mat4::translation(position), quat_to_mat4(rotation)),
                    Mat4::scale(scale));
}

Mat4 Mat4::look_at(const LookAtParams& params) {
    using namespace render_math;

    const Vec3 forward = normalize(params.target - params.eye);
    const Vec3 right = normalize(cross(params.up, forward));
    const Vec3 corrected_up = cross(forward, right);

    Mat4 matrix = Mat4::identity();
    element(matrix, 0, 0) = right.x;
    element(matrix, 1, 0) = corrected_up.x;
    element(matrix, 2, 0) = forward.x;

    element(matrix, 0, 1) = right.y;
    element(matrix, 1, 1) = corrected_up.y;
    element(matrix, 2, 1) = forward.y;

    element(matrix, 0, 2) = right.z;
    element(matrix, 1, 2) = corrected_up.z;
    element(matrix, 2, 2) = forward.z;

    element(matrix, 0, 3) = -dot(right, params.eye);
    element(matrix, 1, 3) = -dot(corrected_up, params.eye);
    element(matrix, 2, 3) = -dot(forward, params.eye);
    return matrix;
}

Mat4 Mat4::perspective(float fov_y_degrees, float aspect_ratio, float near_plane, float far_plane,
                       bool homogeneous_depth) {
    const float safe_aspect = std::max(math::kRuntimeEpsilon, aspect_ratio);
    const float fov_radians = fov_y_degrees * (std::numbers::pi_v<float> / 180.0F);
    const float y_scale = 1.0F / std::tan(fov_radians * 0.5F);
    const float x_scale = y_scale / safe_aspect;
    const float depth_range = far_plane - near_plane;

    Mat4 matrix{};
    element(matrix, 0, 0) = x_scale;
    element(matrix, 1, 1) = y_scale;
    element(matrix, 3, 3) = 0.0F;

    if (std::abs(depth_range) <= math::kRuntimeEpsilon) {
        element(matrix, 2, 2) = 1.0F;
        element(matrix, 2, 3) = -near_plane;
        element(matrix, 3, 2) = 1.0F;
        return matrix;
    }

    if (homogeneous_depth) {
        element(matrix, 2, 2) = (far_plane + near_plane) / depth_range;
        element(matrix, 2, 3) = (-2.0F * near_plane * far_plane) / depth_range;
    } else {
        element(matrix, 2, 2) = far_plane / depth_range;
        element(matrix, 2, 3) = (-near_plane * far_plane) / depth_range;
    }
    element(matrix, 3, 2) = 1.0F;
    return matrix;
}

float* Mat4::data() {
    return elements.data();
}

const float* Mat4::data() const {
    return elements.data();
}

Mat4 multiply(const Mat4& lhs, const Mat4& rhs) {
    Mat4 result{};
    const float* a = lhs.elements.data();
    const float* b = rhs.elements.data();
    float* out = result.elements.data();

    for (int col = 0; col < kMatDimension; ++col) {
        const int col_offset = col * kMatDimension;
        // We intentionally use pointer arithmetic and hardcoded indices for optimal performance in
        // these unrolled math functions
        // NOLINTBEGIN(cppcoreguidelines-pro-bounds-pointer-arithmetic,
        // cppcoreguidelines-avoid-magic-numbers, readability-magic-numbers)
        for (int row = 0; row < kMatDimension; ++row) {
            out[col_offset + row] = (a[row] * b[col_offset]) + (a[4 + row] * b[col_offset + 1]) +
                                    (a[8 + row] * b[col_offset + 2]) +
                                    (a[12 + row] * b[col_offset + 3]);
        }
        // NOLINTEND(cppcoreguidelines-pro-bounds-pointer-arithmetic,
        // cppcoreguidelines-avoid-magic-numbers, readability-magic-numbers)
    }
    return result;
}

Vec3 transform_point(const Mat4& matrix, const Vec3& point) {
    const float* m = matrix.elements.data();
    // We intentionally use pointer arithmetic and hardcoded indices for optimal performance in
    // these unrolled math functions NOLINTBEGIN(cppcoreguidelines-pro-bounds-pointer-arithmetic,
    // cppcoreguidelines-avoid-magic-numbers, readability-magic-numbers)
    const float x = (m[0] * point.x) + (m[4] * point.y) + (m[8] * point.z) + m[12];
    const float y = (m[1] * point.x) + (m[5] * point.y) + (m[9] * point.z) + m[13];
    const float z = (m[2] * point.x) + (m[6] * point.y) + (m[10] * point.z) + m[14];
    const float w = (m[3] * point.x) + (m[7] * point.y) + (m[11] * point.z) + m[15];
    // NOLINTEND(cppcoreguidelines-pro-bounds-pointer-arithmetic,
    // cppcoreguidelines-avoid-magic-numbers, readability-magic-numbers)

    if (std::abs(w) <= math::kRuntimeEpsilon) {
        return Vec3{ .x = x, .y = y, .z = z };
    }

    const float inv_w = 1.0F / w;
    return Vec3{ .x = x * inv_w, .y = y * inv_w, .z = z * inv_w };
}

Mat4 inverse(const Mat4& m) {
    const float* s = m.elements.data();
    std::array<float, 16> inv{};

    inv[0] = (s[5] * s[10] * s[15]) - (s[5] * s[11] * s[14]) - (s[9] * s[6] * s[15]) +
             (s[9] * s[7] * s[14]) + (s[13] * s[6] * s[11]) - (s[13] * s[7] * s[10]);

    inv[4] = (-s[4] * s[10] * s[15]) + (s[4] * s[11] * s[14]) + (s[8] * s[6] * s[15]) -
             (s[8] * s[7] * s[14]) - (s[12] * s[6] * s[11]) + (s[12] * s[7] * s[10]);

    inv[8] = (s[4] * s[9] * s[15]) - (s[4] * s[11] * s[13]) - (s[8] * s[5] * s[15]) +
             (s[8] * s[7] * s[13]) + (s[12] * s[5] * s[11]) - (s[12] * s[7] * s[9]);

    inv[12] = (-s[4] * s[9] * s[14]) + (s[4] * s[10] * s[13]) + (s[8] * s[5] * s[14]) -
              (s[8] * s[6] * s[13]) - (s[12] * s[5] * s[10]) + (s[12] * s[6] * s[9]);

    inv[1] = (-s[1] * s[10] * s[15]) + (s[1] * s[11] * s[14]) + (s[9] * s[2] * s[15]) -
             (s[9] * s[3] * s[14]) - (s[13] * s[2] * s[11]) + (s[13] * s[3] * s[10]);

    inv[5] = (s[0] * s[10] * s[15]) - (s[0] * s[11] * s[14]) - (s[8] * s[2] * s[15]) +
             (s[8] * s[3] * s[14]) + (s[12] * s[2] * s[11]) - (s[12] * s[3] * s[10]);

    inv[9] = (-s[0] * s[9] * s[15]) + (s[0] * s[11] * s[13]) + (s[8] * s[1] * s[15]) -
             (s[8] * s[3] * s[13]) - (s[12] * s[1] * s[11]) + (s[12] * s[3] * s[9]);

    inv[13] = (s[0] * s[9] * s[14]) - (s[0] * s[10] * s[13]) - (s[8] * s[1] * s[14]) +
              (s[8] * s[2] * s[13]) + (s[12] * s[1] * s[10]) - (s[12] * s[2] * s[9]);

    inv[2] = (s[1] * s[6] * s[15]) - (s[1] * s[7] * s[14]) - (s[5] * s[2] * s[15]) +
             (s[5] * s[3] * s[14]) + (s[13] * s[2] * s[7]) - (s[13] * s[3] * s[6]);

    inv[6] = (-s[0] * s[6] * s[15]) + (s[0] * s[7] * s[14]) + (s[4] * s[2] * s[15]) -
             (s[4] * s[3] * s[14]) - (s[12] * s[2] * s[7]) + (s[12] * s[3] * s[6]);

    inv[10] = (s[0] * s[5] * s[15]) - (s[0] * s[7] * s[13]) - (s[4] * s[1] * s[15]) +
              (s[4] * s[3] * s[13]) + (s[12] * s[1] * s[7]) - (s[12] * s[3] * s[5]);

    inv[14] = (-s[0] * s[5] * s[14]) + (s[0] * s[6] * s[13]) + (s[4] * s[1] * s[14]) -
              (s[4] * s[2] * s[13]) - (s[12] * s[1] * s[6]) + (s[12] * s[2] * s[5]);

    inv[3] = (-s[1] * s[6] * s[11]) + (s[1] * s[7] * s[10]) + (s[5] * s[2] * s[11]) -
             (s[5] * s[3] * s[10]) - (s[9] * s[2] * s[7]) + (s[9] * s[3] * s[6]);

    inv[7] = (s[0] * s[6] * s[11]) - (s[0] * s[7] * s[10]) - (s[4] * s[2] * s[11]) +
             (s[4] * s[3] * s[10]) + (s[8] * s[2] * s[7]) - (s[8] * s[3] * s[6]);

    inv[11] = (-s[0] * s[5] * s[11]) + (s[0] * s[7] * s[9]) + (s[4] * s[1] * s[11]) -
              (s[4] * s[3] * s[9]) - s[8] * s[1] * s[7] + s[8] * s[3] * s[5];

    inv[15] = (s[0] * s[5] * s[10]) - (s[0] * s[6] * s[9]) - (s[4] * s[1] * s[10]) +
              (s[4] * s[2] * s[9]) + (s[8] * s[1] * s[6]) - (s[8] * s[2] * s[5]);

    float det = (s[0] * inv[0]) + (s[1] * inv[4]) + (s[2] * inv[8]) + (s[3] * inv[12]);

    if (std::abs(det) < math::kRuntimeEpsilon) {
        return Mat4::identity();
    }

    det = 1.0F / det;

    Mat4 result;
    for (int i = 0; i < 16; i++) {
        result.elements[i] = inv[i] * det;
    }

    return result;
}

} // namespace isla::client



