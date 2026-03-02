#include <numbers>

#pragma once

namespace isla::client::math {

// Runtime guard epsilon for float-domain safety checks in core math paths.
inline constexpr float kRuntimeEpsilon = 0.000001F;

// Default tolerance for float comparisons in tests.
inline constexpr float kTestEpsilon = 0.0001F;

// Looser tolerance used in composed/integration-like math assertions.
inline constexpr float kLooseTestEpsilon = 0.001F;

// Degeneracy threshold for triangle area/orientation checks.
inline constexpr float kTriangleAreaEpsilon = 0.0000001F;

// Pi constant.
inline constexpr float kPi = std::numbers::pi_v<float>;

} // namespace isla::client::math

