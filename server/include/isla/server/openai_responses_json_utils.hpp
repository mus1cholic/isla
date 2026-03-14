#pragma once

#include <optional>
#include <string>
#include <string_view>

#include <nlohmann/json.hpp>
#include "absl/status/status.h"
#include "absl/status/statusor.h"

namespace isla::server::ai_gateway {

absl::StatusOr<std::optional<std::string>> ReadOptionalStringField(const nlohmann::json& object,
                                                                   std::string_view field_name);

} // namespace isla::server::ai_gateway
