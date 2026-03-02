#pragma once

#include <cstdint>
#include <limits>

namespace isla::client::internal {

constexpr std::uint64_t kMaxEncodedTextureBytes =
    static_cast<std::uint64_t>(std::numeric_limits<std::uint32_t>::max());

constexpr bool is_encoded_texture_size_supported(std::uint64_t size_bytes) {
    return size_bytes > 0U && size_bytes <= kMaxEncodedTextureBytes;
}

} // namespace isla::client::internal

