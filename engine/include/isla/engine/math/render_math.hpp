#pragma once

#include "isla/engine/math/constants.hpp"
#include "isla/engine/math/math_types.hpp"

namespace isla::client::render_math {

Vec3 operator+(const Vec3& lhs, const Vec3& rhs);
Vec3 operator-(const Vec3& lhs, const Vec3& rhs);
Vec3 operator*(const Vec3& v, float s);
float dot(const Vec3& lhs, const Vec3& rhs);
Vec3 cross(const Vec3& lhs, const Vec3& rhs);
float length(const Vec3& v);
Vec3 normalize(const Vec3& v);
Vec3 rotate_y(const Vec3& v, float radians);

} // namespace isla::client::render_math


