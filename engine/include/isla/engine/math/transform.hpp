#pragma once

#include "isla/engine/math/math_types.hpp"
#include "isla/engine/math/quat.hpp"

namespace isla::client {

inline constexpr Vec3 kDefaultTransformScale{ .x = 1.0F, .y = 1.0F, .z = 1.0F };

struct Transform {
    Vec3 position{ .x = 0.0F, .y = 0.0F, .z = 0.0F };
    Quat rotation = Quat::identity();
    Vec3 scale = kDefaultTransformScale;
};

} // namespace isla::client


