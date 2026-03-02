#include "isla/engine/render/overlay_transparency.hpp"

#include <gtest/gtest.h>

namespace isla::client {

TEST(OverlayTransparencyContractTest, ColorKeyIsBlackAndClearColorMatches) {
    EXPECT_EQ(OverlayTransparencyConfig::kColorKeyRed, 0);
    EXPECT_EQ(OverlayTransparencyConfig::kColorKeyGreen, 0);
    EXPECT_EQ(OverlayTransparencyConfig::kColorKeyBlue, 0);

    EXPECT_FLOAT_EQ(OverlayTransparencyConfig::kClearColor[0], 0.0F);
    EXPECT_FLOAT_EQ(OverlayTransparencyConfig::kClearColor[1], 0.0F);
    EXPECT_FLOAT_EQ(OverlayTransparencyConfig::kClearColor[2], 0.0F);
    EXPECT_FLOAT_EQ(OverlayTransparencyConfig::kClearColor[3], 1.0F);
}

} // namespace isla::client
