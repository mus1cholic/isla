#pragma once

#include <string>
#include <string_view>

namespace isla::server::memory {

[[nodiscard]] std::string NormalizeIdentifierComponent(std::string_view text);

} // namespace isla::server::memory
