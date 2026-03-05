#pragma once

#include <string_view>

#include "isla/engine/render/pmx_native_runtime.hpp"

namespace isla::client::pmx_native::internal {

[[nodiscard]] ProbeResult probe_with_saba(std::string_view asset_path);

} // namespace isla::client::pmx_native::internal
