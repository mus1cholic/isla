#pragma once

#include <array>
#include <cstdint>

namespace isla::client {

struct OverlayTransparencyConfig {
    static constexpr std::uint8_t kColorKeyRed = 0;
    static constexpr std::uint8_t kColorKeyGreen = 0;
    static constexpr std::uint8_t kColorKeyBlue = 0;
    static constexpr std::array<float, 4> kClearColor = {0.0F, 0.0F, 0.0F, 1.0F};
};

} // namespace isla::client

