#pragma once

#include <string>
#include <string_view>

#include "isla/server/memory/memory_timestamp_utils.hpp"

namespace isla::server::ai_gateway {

[[nodiscard]] std::string BuildStubReply(std::string_view prefix, std::string_view text);
[[nodiscard]] isla::server::memory::Timestamp NowTimestamp();

} // namespace isla::server::ai_gateway
