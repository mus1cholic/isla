#pragma once

#include <string>
#include <string_view>

namespace isla::server::ai_gateway {

[[nodiscard]] std::string TrimAscii(std::string_view text);

} // namespace isla::server::ai_gateway
