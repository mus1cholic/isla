#include "win32_layered_overlay.hpp"

#include <gtest/gtest.h>

namespace isla::client {

TEST(Win32LayeredOverlayTest, ConfigureReturnsFalseForNullWindow) {
    EXPECT_FALSE(configure_win32_layered_overlay(nullptr));
}

} // namespace isla::client
