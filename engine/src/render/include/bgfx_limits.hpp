#pragma once

#include <cstddef>
#include <cstdint>
#include <limits>

namespace isla::client::internal {

inline constexpr std::size_t kMaxBgfxCopyBytes = std::numeric_limits<std::uint32_t>::max();

} // namespace isla::client::internal
