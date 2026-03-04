#include "isla/engine/render/overlay_transparency.hpp"

#include <gtest/gtest.h>

namespace isla::client {

TEST(OverlayTransparencyContractTest, ClearColorUsesTransparentBackgroundContract) {
    EXPECT_FLOAT_EQ(OverlayTransparencyConfig::kClearColor[0], 0.0F);
    EXPECT_FLOAT_EQ(OverlayTransparencyConfig::kClearColor[1], 0.0F);
    EXPECT_FLOAT_EQ(OverlayTransparencyConfig::kClearColor[2], 0.0F);
    EXPECT_FLOAT_EQ(OverlayTransparencyConfig::kClearColor[3], 0.0F);
}

} // namespace isla::client
