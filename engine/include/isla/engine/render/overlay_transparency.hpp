#pragma once

#include <array>

namespace isla::client {

struct OverlayTransparencyConfig {
    // Authoritative Phase 4.5 contract: transparent clear background and visible model pixels.
    static constexpr std::array<float, 4> kClearColor = { 0.0F, 0.0F, 0.0F, 0.0F };
};

} // namespace isla::client
