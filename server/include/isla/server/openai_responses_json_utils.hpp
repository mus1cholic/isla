#pragma once

#include <optional>
#include <string>
#include <string_view>

#include "absl/status/status.h"
#include "absl/status/statusor.h"
#include <nlohmann/json.hpp>

namespace isla::server::ai_gateway {

absl::StatusOr<std::optional<std::string>> ReadOptionalStringField(const nlohmann::json& object,
                                                                   std::string_view field_name);

std::string ExtractJsonErrorMessage(const nlohmann::json& json);

} // namespace isla::server::ai_gateway
