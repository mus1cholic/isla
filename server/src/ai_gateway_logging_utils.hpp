#pragma once

#include <string>
#include <string_view>

namespace isla::server::ai_gateway {

[[nodiscard]] std::string SanitizeForLog(std::string_view value);

} // namespace isla::server::ai_gateway
