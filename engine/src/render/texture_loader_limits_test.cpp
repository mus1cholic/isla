#include <gtest/gtest.h>

#include <cstdint>

#include "engine/src/render/include/texture_loader_limits.hpp"

namespace {

using isla::client::internal::is_encoded_texture_size_supported;
using isla::client::internal::kMaxEncodedTextureBytes;

TEST(TextureLoaderLimitsTests, RejectsZeroEncodedSize) {
    EXPECT_FALSE(is_encoded_texture_size_supported(0U));
}

TEST(TextureLoaderLimitsTests, AcceptsMaxAllowedEncodedSize) {
    EXPECT_TRUE(is_encoded_texture_size_supported(kMaxEncodedTextureBytes));
}

TEST(TextureLoaderLimitsTests, RejectsEncodedSizeAboveMax) {
    EXPECT_FALSE(is_encoded_texture_size_supported(kMaxEncodedTextureBytes + 1U));
}

} // namespace
